#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHTesp.h>

// ===== Wi-Fi =====
const char* ssid = "aim";
const char* password = "123456abc";

// ===== HiveMQ Cloud (TLS) =====
const char* mqtt_server = "44ac454a11294918808b336ed3820d82.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "ARAHIM";
const char* mqtt_pass   = "Abc123456";

// ===== Topics =====
const char* topic_sub   = "abu/LedControl";
const char* topic_temp  = "abu/readDHT_Temp";
const char* topic_humid = "abu/readDHT_Humid";

// ===== Objects =====
WiFiClientSecure tlsClient;
PubSubClient client(tlsClient);
DHTesp dht;

// ===== Pins =====
#define LED_PIN D1   // GPIO5
#define DHT_PIN D7   // GPIO13

// ===== Timing =====
unsigned long lastReadTime = 0;
const unsigned long interval = 2000;

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempt > 30) {
      Serial.println("WiFi connection failed");
      return;
    }
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("Received on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if (msg == "led1_1") digitalWrite(LED_PIN, HIGH);
  if (msg == "led1_0") digitalWrite(LED_PIN, LOW);
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP8266Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      client.subscribe(topic_sub);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(9600);
  randomSeed(analogRead(A0));

  setup_wifi();

  // TLS NOTE: for quick testing; in production, load a proper Root CA instead.
  tlsClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  dht.setup(DHT_PIN, DHTesp::DHT11);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long now = millis();
  if (now - lastReadTime > interval) {
    lastReadTime = now;

    TempAndHumidity data = dht.getTempAndHumidity();
    if (!isnan(data.temperature) && !isnan(data.humidity)) {
      char tempStr[8], humidStr[8];
      dtostrf(data.temperature, 1, 2, tempStr);
      dtostrf(data.humidity, 1, 2, humidStr);

      client.publish(topic_temp, tempStr);
      client.publish(topic_humid, humidStr);

      Serial.print("Temp: ");
      Serial.print(tempStr);
      Serial.print(" °C | Humid: ");
      Serial.print(humidStr);
      Serial.println(" %");
    } else {
      Serial.println("Failed to read from DHT11");
    }
  }
}
