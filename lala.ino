#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <EEPROM.h>
#include <time.h>  // NTP/time

// ===== Wi-Fi =====
const char* ssid     = "aim"; // <========================================================= CHANGE
const char* password = "123456abc"; // <=================================================== CHANGE

// ===== HiveMQ Cloud (TLS) =====
const char* mqtt_server = "44ac454a11294918808b336ed3820d82.s1.eu.hivemq.cloud";  // <===== CHANGE
const char* mqtt_user   = "ARAHIM"; // <=================================================== CHANGE
const char* mqtt_pass   = "Abc123456";  // <=============================================== CHANGE
const int   mqtt_port   = 8883;

// ===== Topics =====
#define MQTT_NS "ali"          // <===================================== CHANGE THIS TO CLUB MEMBER's NAME

// All topics auto-build from the prefix:
const char* topic_temp      = MQTT_NS "/readDHT_Temp";
const char* topic_humid     = MQTT_NS "/readDHT_Humid";
const char* topic_soil      = MQTT_NS "/readSoil";
const char* topic_relay     = MQTT_NS "/RelayControl";
const char* topic_status    = MQTT_NS "/RelayStatus";
const char* topic_autowater = MQTT_NS "/autoWater";   // e.g. "1:11", "01:11", "13:07:00"
const char* topic_alert     = MQTT_NS "/alert";       // final OFF-time alert

// ===== Pins =====
#define LED_PIN   D0
#define RLY1_PIN  D2
#define RLY2_PIN  D5
#define DHT_PIN   D6

#define DHTTYPE   DHT11
DHT dht(DHT_PIN, DHTTYPE);

// ===== Objects =====
WiFiClientSecure tlsClient;
PubSubClient client(tlsClient);

// ===== Timing =====
unsigned long lastSensorTick = 0;
unsigned long lastLedTick    = 0;
unsigned long lastTimeTick   = 0;
const unsigned long intervalSensor = 2000; // 2 s
const unsigned long intervalLED    = 250;  // LED refresh
const unsigned long intervalTime   = 1000; // check schedule every 1 s

// ===== Mode & States =====
enum RelayMode { AUTO_MODE, MANUAL_MODE };
RelayMode relay1Mode = AUTO_MODE;
bool relay1State = false;     // true=ON
int  soilPercent = -1;        // 0..100 from A0

// ===== Thresholds (percent) =====
int SOIL_ON_THRESHOLD  = 80;  // AUTO: ON when > 80% (dry)
int SOIL_OFF_THRESHOLD = 80;  // AUTO: OFF when <= 80%
int SOIL_SAFE_LIMIT    = 40;  // MANUAL: stop & AUTO when <= 40%

// ===== Manual max run time =====
const unsigned long MANUAL_MAX_MS = 30000; // 30 seconds
unsigned long manualStartMs = 0;

// ===== OFF-time assessment (higher % = drier) =====
const int SOIL_MIN_DROP = 5;     // minimal % drop to count as watering OK
int soilBaseline = -1;           // captured at ON

// ===== LED polarity =====
const bool LED_ACTIVE_LOW = false;
inline void setLed(bool on)   { if (LED_ACTIVE_LOW) digitalWrite(LED_PIN, on?LOW:HIGH); else digitalWrite(LED_PIN, on?HIGH:LOW); }
inline void updateStatusLED() { bool ok = (WiFi.status()==WL_CONNECTED) && client.connected(); setLed(ok); }

// ===== Timezone (Malaysia) =====
const long TZ_OFFSET = 8 * 3600;  // UTC+8
const int  DST_OFFSET = 0;        // no DST in MY

// ===== Daily schedule (persisted) =====
int  targetSecOfDay = -1;   // 0..86399; -1 = none
bool scheduleEnabled = false;
int  lastTriggeredDoy = -1; // day-of-year of last trigger (RAM only)

// ===== EEPROM layout =====
/*
  addr 0..3  : MAGIC (0xA5A5A5A5)
  addr 4..7  : targetSecOfDay (uint32_t)
  addr 8     : enabled (uint8_t 0/1)
*/
const uint32_t EEPROM_MAGIC = 0xA5A5A5A5;
const size_t   EEPROM_SIZE  = 16;

void eepromSaveSchedule(uint32_t secOfDay, bool enabled) {
  EEPROM.put(0, EEPROM_MAGIC);
  EEPROM.put(4, secOfDay);
  uint8_t en = enabled ? 1 : 0;
  EEPROM.put(8, en);
  EEPROM.commit();
  Serial.printf("EEPROM saved: secOfDay=%u enabled=%u\n", secOfDay, en);
}

bool eepromLoadSchedule(uint32_t &secOfDay, bool &enabled) {
  uint32_t magic=0, sec=0; uint8_t en=0;
  EEPROM.get(0, magic);
  EEPROM.get(4, sec);
  EEPROM.get(8, en);
  if (magic != EEPROM_MAGIC) {
    Serial.println("EEPROM: no valid schedule (magic mismatch)");
    return false;
  }
  if (sec > 86399) {
    Serial.println("EEPROM: invalid secOfDay");
    return false;
  }
  secOfDay = sec;
  enabled  = (en == 1);
  Serial.printf("EEPROM loaded: secOfDay=%u enabled=%u\n", secOfDay, en);
  return true;
}

// ---- time helpers ----
bool getLocalTM(struct tm* info) {
  time_t now = time(nullptr);
  if (now < 8 * 3600 * 2) return false;  // not synced
  localtime_r(&now, info);
  return true;
}

int parseTimeToSecOfDay(String s) {
  s.trim();
  int h = 0, m = 0, sec = 0;
  int c1 = s.indexOf(':');
  if (c1 < 0) return -1;
  int c2 = s.indexOf(':', c1 + 1);
  if (c2 < 0) {
    // H:MM or HH:MM
    h = s.substring(0, c1).toInt();
    m = s.substring(c1 + 1).toInt();
    sec = 0;
  } else {
    // HH:MM:SS
    h   = s.substring(0, c1).toInt();
    m   = s.substring(c1 + 1, c2).toInt();
    sec = s.substring(c2 + 1).toInt();
  }
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return -1;
  return h * 3600 + m * 60 + sec;
}

int currentSecOfDay() {
  struct tm nowTm;
  if (!getLocalTM(&nowTm)) return -1;
  return nowTm.tm_hour * 3600 + nowTm.tm_min * 60 + nowTm.tm_sec;
}

// ---- helpers: ON/OFF transitions with baseline & alert ----
void relay1TurnOn(const char* statusTag) {
  relay1State   = true;
  digitalWrite(RLY1_PIN, LOW);
  manualStartMs = millis();
  soilBaseline  = soilPercent;   // capture baseline at ON
  client.publish(topic_status, statusTag);
  Serial.printf("[RLY1 ON] Baseline soil=%d%%\n", soilBaseline);
}

void relay1TurnOff(const char* statusTag) {
  relay1State = false;
  digitalWrite(RLY1_PIN, HIGH);

  // OFF-time assessment vs baseline
  if (soilBaseline >= 0 && soilPercent >= 0) {
    int drop = soilBaseline - soilPercent; // positive => wetter
    if (drop >= SOIL_MIN_DROP) {
      client.publish(topic_alert, "System OK");
      Serial.printf("[RLY1 OFF] System OK (Δ%+d%%)\n", drop);
    } else {
      client.publish(topic_alert, "Water tank is empty");
      Serial.printf("[RLY1 OFF] Tank empty (Δ%+d%%)\n", drop);
    }
  } else {
    // If we somehow missed a baseline, still send non-empty status
    client.publish(topic_alert, "System OK");
    Serial.println("[RLY1 OFF] No baseline — default OK");
  }
  soilBaseline = -1;

  relay1Mode = AUTO_MODE;
  client.publish(topic_status, statusTag);
}

// ===== Wi-Fi Connect =====
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempt > 60) {
      Serial.println("\nWiFi connection failed");
      break;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  }
}

// ===== MQTT Callback =====
void callback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.print("Received on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if (String(topic) == topic_autowater) {
    int sec = parseTimeToSecOfDay(msg);
    if (sec >= 0) {
      targetSecOfDay   = sec;
      scheduleEnabled  = true;
      eepromSaveSchedule((uint32_t)sec, true);
      lastTriggeredDoy = -1; // allow trigger next time the clock reaches it
      int hh = sec / 3600;
      int mm = (sec % 3600) / 60;
      int ss = sec % 60;
      Serial.printf("New daily schedule set: %02d:%02d:%02d (saved to EEPROM)\n", hh, mm, ss);
    } else {
      Serial.println("Invalid autoWater time. Use H:MM, HH:MM, or HH:MM:SS");
    }
    return;
  }

  // Manual overrides (still available)
  if (String(topic) == topic_relay) {
    if (msg == "rly1_on") {
      relay1Mode = MANUAL_MODE;
      relay1TurnOn("manual_on");
    }
    if (msg == "rly1_off") {
      relay1TurnOff("auto_resume");
    }
    if (msg == "rly2_on")  digitalWrite(RLY2_PIN, LOW);
    if (msg == "rly2_off") digitalWrite(RLY2_PIN, HIGH);
  }
}

// ===== MQTT Reconnect =====
void reconnect() {
  while (!client.connected()) {
    updateStatusLED();
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP8266Client-" + String(ESP.getChipId(), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      client.subscribe(topic_relay);
      client.subscribe(topic_autowater);
      updateStatusLED();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

// ===== Sensor Readings & Control =====
void readAndControl() {
  // DHT11 + Soil (one line print)
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  int raw = analogRead(A0);                // 0..1023
  soilPercent = map(raw, 0, 1023, 0, 100); // 0..100%

  if (!isnan(t) && !isnan(h)) {
    char tempStr[8], humidStr[8], soilStr[8];
    dtostrf(t, 1, 2, tempStr);
    dtostrf(h, 1, 2, humidStr);
    dtostrf((double)soilPercent, 1, 0, soilStr);

    client.publish(topic_temp,  tempStr);
    client.publish(topic_humid, humidStr);
    client.publish(topic_soil,  soilStr);

    Serial.printf("Temp: %.2f °C | Humid: %.2f %% | Soil: %d %% (raw=%d)\n",
                  t, h, soilPercent, raw);
  } else {
    Serial.printf("Temp/Humid read failed | Soil: %d %% (raw=%d)\n", soilPercent, raw);
  }

  // ---- RELAY LOGIC ----
  if (relay1Mode == AUTO_MODE) {
    if (soilPercent > SOIL_ON_THRESHOLD) {
      if (!relay1State) {
        relay1Mode = MANUAL_MODE;   // reuse run window handling
        relay1TurnOn("auto_on");
      }
    } else if (soilPercent <= SOIL_OFF_THRESHOLD) {
      if (relay1State) {
        relay1TurnOff("auto_resume");
      }
    }
  } else { // MANUAL with safety cut-off OR timeout
    bool soilSafe    = (soilPercent <= SOIL_SAFE_LIMIT);
    bool timeExpired = (millis() - manualStartMs >= MANUAL_MAX_MS);
    if (soilSafe || timeExpired) {
      relay1TurnOff(soilSafe ? "auto_resume_safe" : "auto_resume_timeout");
    }
  }
}

// ===== Time-based trigger check (daily) =====
void checkTimerTrigger() {
  if (!scheduleEnabled || targetSecOfDay < 0) return;

  struct tm nowTm;
  if (!getLocalTM(&nowTm)) return; // wait until NTP sync

  int nowSec = nowTm.tm_hour * 3600 + nowTm.tm_min * 60 + nowTm.tm_sec;
  int doy    = nowTm.tm_yday; // 0..365

  // Prevent re-triggering multiple times in the same day
  if (lastTriggeredDoy == doy) return;

  int diff = abs(nowSec - targetSecOfDay);
  int wrap = 86400 - diff;
  int best = (diff < wrap) ? diff : wrap;

  if (best <= 5) {
    // Fire once per day
    relay1Mode = MANUAL_MODE;
    relay1TurnOn("manual_on_timer");
    Serial.println("Relay 1 manual ON (timer trigger fired)");
    lastTriggeredDoy = doy;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(RLY1_PIN, OUTPUT);
  pinMode(RLY2_PIN, OUTPUT);

  setLed(false);
  digitalWrite(RLY1_PIN, HIGH);
  digitalWrite(RLY2_PIN, HIGH);

  // EEPROM init & load saved schedule
  EEPROM.begin(EEPROM_SIZE);
  uint32_t sec; bool en;
  if (eepromLoadSchedule(sec, en)) {
    targetSecOfDay  = (int)sec;
    scheduleEnabled = en;
    if (scheduleEnabled) {
      int hh = targetSecOfDay / 3600;
      int mm = (targetSecOfDay % 3600) / 60;
      int ss = targetSecOfDay % 60;
      Serial.printf("Loaded schedule from EEPROM: %02d:%02d:%02d (daily)\n", hh, mm, ss);
    }
  }

  dht.begin();

  // WiFi + NTP
  setup_wifi();
  configTime(TZ_OFFSET, DST_OFFSET, "pool.ntp.org", "time.nist.gov");

  // MQTT
  tlsClient.setInsecure();  // testing only; load CA for production
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  updateStatusLED();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long now = millis();

  if (now - lastLedTick >= intervalLED) {
    lastLedTick = now;
    updateStatusLED();
  }
  if (now - lastTimeTick >= intervalTime) {
    lastTimeTick = now;
    checkTimerTrigger();
  }
  if (now - lastSensorTick >= intervalSensor) {
    lastSensorTick = now;
    readAndControl();
  }
}
