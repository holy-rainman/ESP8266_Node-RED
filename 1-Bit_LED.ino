#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PW";

// MQTT broker info
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "YOUR_TOPIC";  // Updated topic

WiFiClient espClient;
PubSubClient client(espClient);

#define LED_PIN D1  // GPIO5

void setup_wifi() 
{ delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  int attempt = 0;
  while(WiFi.status() != WL_CONNECTED) 
  { delay(500);
    Serial.print(".");
    if (++attempt > 30) 
    { Serial.println("WiFi connection failed");
      return;
    }
  }

  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// MQTT message handler
void callback(char* topic, byte* payload, unsigned int length) 
{ String msg = "";
  for(unsigned int i = 0; i < length; i++)
    msg += (char)payload[i];

  Serial.print("Received on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(msg);

  if(msg == "led1_1") digitalWrite(LED_PIN, HIGH);
  if(msg == "led1_0") digitalWrite(LED_PIN, LOW);
}

void reconnect() 
{ while(!client.connected()) 
  { Serial.print("Connecting to MQTT...");
    String clientId = "ESP8266Client-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) 
    { Serial.println("connected");
      client.subscribe(mqtt_topic);
    } 
    else 
    { Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void setup() 
{ pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);  // LED off by default
  Serial.begin(9600);
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() 
{ if (!client.connected())
    reconnect();
  client.loop();
}
