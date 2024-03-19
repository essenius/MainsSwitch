#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SafeCString.h>
#include <time.h>
#include <fs.h>

constexpr int RelayPort = 2;
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

void fetchURL(BearSSL::WiFiClientSecure *client, const char *host, const uint16_t port, const char *path) {
  if (!path) {
    path = "/";
  }

  ESP.resetFreeContStack();
  uint32_t freeStackStart = ESP.getFreeContStack();
  Serial.printf("Trying: %s:%d...", host, port);
  client->connect(host, port);
  if (!client->connected()) {
    Serial.printf("*** Can't connect. ***\n-------\n");
    return;
  }
  Serial.printf("Connected!\n-------\n");
  client->write("GET ");
  client->write(path);
  client->write(" HTTP/1.0\r\nHost: ");
  client->write(host);
  client->write("\r\nUser-Agent: ESP8266\r\n");
  client->write("\r\n");
  uint32_t to = millis() + 5000;
  if (client->connected()) {
    do {
      char tmp[32];
      memset(tmp, 0, 32);
      int rlen = client->read((uint8_t*)tmp, sizeof(tmp) - 1);
      yield();
      if (rlen < 0) {
        break;
      }
      Serial.print(tmp);
    } while (millis() < to);
  }
  client->stop();
  uint32_t freeStackEnd = ESP.getFreeContStack();
  Serial.printf("\nCONT stack used: %d\n", freeStackStart - freeStackEnd);
  Serial.printf("BSSL stack used: %d\n-------\n\n", stack_thunk_get_max_usage());
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
    config[address] = value ^ chipId[address % MaxKeySize];
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
  Serial.printf("autoconnect: %d\n", WiFi.getAutoConnect());
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

bool setClock() {
  auto timeInfo = cfg["time"];
  const char* timezone = timeInfo["tz"] | "UTC0";
  const char* server1 = timeInfo["1"] | "pool.ntp.org";
  const char* server2 = timeInfo["2"] | "time.nist.gov";
  configTime(timezone, server1, server2); 

  // wait until we're no longer in the first few hours of 1970
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
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

bool mqttPublish(const char* topic, bool value) {
  return mqttPublish(topic, value ? trueString : falseString);
}

void mqttCallback(const char* topic, byte* payload, unsigned int length) {
  
  if (equals(payload, length, trueString) && !digitalRead(RelayPort)) {
    relaySwitched = true;
    digitalWrite(RelayPort, HIGH);
  }
  if (equals(payload, length, falseString) && digitalRead(RelayPort)) {
    relaySwitched = true;
    digitalWrite(RelayPort, LOW);
  }
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
  Serial.begin(115200);
  delay(250);
  pinMode(RelayPort, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(false);

  startSPIFFS();
  readCertificates(wifiClient);
  getConfig();
  wifiConnect();
  printWifiStatus();
  setClock();

  char path[] = "/rest/items/KNX_SL_Slaapkamer";
  fetchURL(&wifiClient, "nas", 8443, path);
  mqttReady = mqttConnect() && mqttAnnounce();
}

void loop() {
  if (mqttReady) {
    if (relaySwitched) {
      if (mqttPublish("switch/1", digitalRead(RelayPort))) {
        relaySwitched = false;
      }
    } 
    if (!mqttClient.loop()) {
      Serial.println("Reconnecting to MQTT");
      mqttReady = mqttConnect();      
    }
    delay(250);
  }
}
