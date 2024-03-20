// Copyright 2024 Rik Essenius
// 
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
// except in compliance with the License. You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and limitations under the License.

#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SafeCString.h>
#include <time.h>
#include <fs.h>

constexpr int RelayPort = 0; // GPIO0
constexpr const char* trueString = "true";
constexpr const char* falseString = "false";

BearSSL::WiFiClientSecure wifiClient;
BearSSL::X509List caCert;
BearSSL::X509List clientCert;
BearSSL::PrivateKey clientKey;
PubSubClient mqttClient(wifiClient);
JsonDocument cfg;

const char* hostName = nullptr;
bool relaySwitched = true;
bool mqttReady = false;

bool equals(byte* payload, unsigned int length, const char* expectation) {
  if (length != strlen(expectation)) return false;
  for (int i = 0; i< length; i++) {
    if (expectation[i] != static_cast<char>(payload[i])) return false;
  }
  return true;
}

bool getConfig() {
  constexpr int MaxConfigSize = 512;
  char config[MaxConfigSize];
  constexpr byte MaxKeySize = 4;
  byte chipId[MaxKeySize];
  EEPROM.begin(MaxConfigSize);
  // create keys using the chip ID. The config is encoded so it isn't immediately obvious what's in there.
  chipId[1] = (ESP.getChipId() >> 16) & 0xff;
  chipId[2] = (ESP.getChipId() >> 8) & 0xff;
  chipId[3] = ESP.getChipId() & 0xff;
  chipId[0] = chipId[1] ^ chipId[2] ^ chipId[3];

  bool isFinished = false;
  int address = 0;
  while (!isFinished && address < MaxConfigSize) {
    byte value = EEPROM.read(address);
    config[address] = static_cast<char>(value ^ chipId[address % MaxKeySize]);
    isFinished = config[address] == 0;
    address++;
  }
  
  Serial.println(config);
  auto error = deserializeJson(cfg, config);
  if (error) {
      Serial.printf("Could not interpret config as json: %s\n", error.c_str()); 
      return false;
  }
  return true;
}

void printWifiStatus() {
  Serial.printf("ssid: %s\n", WiFi.SSID().c_str());
  Serial.printf("ip address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("host name: %s\n", WiFi.hostname().c_str());
  Serial.printf("signal strength (RSSI): %d dBm\n", WiFi.RSSI());
  Serial.printf("channel: %d\n", WiFi.channel());
  Serial.printf("mac address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("bssid: %s\n", WiFi.BSSIDstr().c_str());
  Serial.printf("autoConnect: %d\n", WiFi.getAutoConnect());
}

String readFromFile(const char* fileName) {
    String result = "";
    File file = SPIFFS.open(fileName, "r");
    if (!file) {
        Serial.printf("Could not find '%s'\n", fileName);
    } else {
        if (file.available()) {
            result = file.readString();
        } else {
            Serial.printf("Could not read from '%s'\n", fileName);
        }
        file.close();
    }    
    return result;
}

void readCertificates(BearSSL::WiFiClientSecure& client) {
  String caCertificate = readFromFile("/ca.crt");
  if (caCertificate != "") {
    caCert.append(caCertificate.c_str());
    client.setTrustAnchors(&caCert);
  } else {
    Serial.println("Could not set CA Certificate");
  }

  String clientCertificate = readFromFile("/host.crt");
  if (clientCertificate != "") {
    clientCert.append(clientCertificate.c_str());
  } else {
    Serial.println("Could not set Client Certificate");
    return;
  }
  
  String clientKey1 = readFromFile("/host.key");
  if (clientKey1 != "") {
    clientKey.parse(clientKey1.c_str());
  } else {
    Serial.println("Could not set Client key");
    return;
  }
  client.setClientRSACert(&clientCert, &clientKey); 
}

bool setClock() {
  auto timeInfo = cfg["time"];
  const char* timezone = timeInfo["tz"] | "UTC0";
  const char* server1 = timeInfo["1"] | "pool.ntp.org";
  const char* server2 = timeInfo["2"] | "time.nist.gov";
  configTime(timezone, server1, server2); 

  // wait until we're no longer in the first few hours of 1970
  time_t now = time(nullptr);
  while (now < static_cast<time_t>(3600) * 16) {
    delay(500);
    now = time(nullptr);
  }
  Serial.printf("\nTime: %s\n", ctime(&now));
  return true;
}

void startSPIFFS() { 
  SPIFFS.begin();                            
  Serial.println("SPIFFS started. Contents:");
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {                      
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\t%s (%d bytes)\n", fileName.c_str(), fileSize);
    }
    Serial.printf("\n");
  }
  FSInfo fsInfo;
  SPIFFS.info(fsInfo);
  Serial.printf("Info\n\tTotal bytes: %d\n\tUsed bytes: %d\n\tBlock size: %d\n\tPage size: %d\n\tMax open files: %d\n\tMax path length: %d\n",
    fsInfo.totalBytes, fsInfo.usedBytes, fsInfo.blockSize, fsInfo.pageSize, fsInfo.maxOpenFiles, fsInfo.maxPathLength);
}

bool mqttAnnounce() {
  if (!mqttPublish("$homie", "4.0.0")) {
    Serial.println("Could not publish message");
    return false;
  } else {
    mqttPublish("$name", hostName);
    mqttPublish("$nodes", "switch");
    mqttPublish("$implementation", "esp8266");
    mqttPublish("$state", "ready");
    mqttPublish("$extensions", "");
    mqttPublish("switch/$name","Switch");
    mqttPublish("switch/$type","1");
    mqttPublish("switch/$properties","1");
    mqttPublish("switch/1/$name","1");
    mqttPublish("switch/1/$datatype","Boolean");
    mqttPublish("switch/1/$settable","true"); 
  }
  return true;
}

void mqttCallback(const char* topic, byte* payload, unsigned int length) {
  if (equals(payload, length, trueString) && (digitalRead(RelayPort) == HIGH)) {
    relaySwitched = true;
    digitalWrite(RelayPort, LOW);
  }
  if (equals(payload, length, falseString) && (digitalRead(RelayPort) == LOW)) {
    relaySwitched = true;
    digitalWrite(RelayPort, HIGH);
  }
}

bool mqttConnect() {
  if (!cfg.containsKey("mqtt")) return false;
  auto mqttCfg = cfg["mqtt"];
  const char* broker = mqttCfg["broker"];
  int port = mqttCfg["port"] | 1883;
  mqttClient.setBufferSize(512);
  mqttClient.setServer(broker, port);
  mqttClient.setCallback(mqttCallback);
  const char* user = mqttCfg["user"];
  const char* password = mqttCfg["password"];

  int tries=3;
  while (!mqttClient.connect(hostName, user, password) && --tries > 0) {
    Serial.printf("Could not connect to MQTT broker %s:%d -  state %d\n", broker, port, mqttClient.state()); 
    yield();
  }
  if (!mqttClient.connected()) return false;

  char buffer[50];
  SafeCString::sprintf(buffer, "homie/%s/switch/1/set", hostName);
    
  if (!mqttClient.subscribe(buffer)) {
      Serial.println("Could not subscribe");
      return false;  
  } else {
      Serial.printf("Subscribed to topic '%s'\n", buffer);
  }  
  return true;
}

bool mqttPublish(const char* topic, const char* value) {
  char buffer[100];
  if (hostName == nullptr) {
    Serial.println("hostName not set");
    return false;
  }
  SafeCString::sprintf(buffer, "homie/%s/%s", hostName, topic);
  Serial.printf("Publishing '%s' to '%s' ", value, topic);
  if (!mqttClient.publish(buffer, value)) {
    Serial.println("failed");
    return false;
  }
  Serial.println("succeeded");
  return true;
}

bool mqttPublish(const char* topic, bool value) {
  return mqttPublish(topic, value ? trueString : falseString);
}

bool wifiConnect() {
  if (!cfg.containsKey("wifi")) return false;
  auto wifi = cfg["wifi"];
  hostName = wifi["host"];
  if (!WiFi.hostname(hostName)) {
    Serial.printf("Could not set host name to '%s'\n", hostName);
  }
  WiFi.begin( wifi["ssid"].as<const char*>(), wifi["password"].as<const char*>());

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  WiFi.setAutoReconnect(true);
  Serial.println("Connected to wifi");
  return true;
}

void setup() {
  // port value high means the relay is off
  pinMode(RelayPort, OUTPUT);
  digitalWrite(RelayPort, HIGH);
  
  // the LED shows we are initializing
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(250);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(false);

  startSPIFFS();
  readCertificates(wifiClient);
  getConfig();
  wifiConnect();
  printWifiStatus();
  setClock();
  mqttReady = mqttConnect() && mqttAnnounce();
}

void loop() {
  if (mqttReady) {
    // show the world we're done initializing by switching off the LED
    if (digitalRead(LED_BUILTIN) == LOW) {
      digitalWrite(LED_BUILTIN, HIGH);
    }
    if (relaySwitched) {
      if (mqttPublish("switch/1", !digitalRead(RelayPort))) {
        relaySwitched = false;
      }
    } 
    if (!mqttClient.loop()) {
      // we got disconnected. Show that via the LED
      digitalWrite(LED_BUILTIN, LOW);
      Serial.println("Reconnecting to MQTT");
      mqttReady = mqttConnect();      
    }
    delay(250);
  }
}
