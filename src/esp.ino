#include <ESP8266WiFi.h>
#include <PubSubClient.h>

const char* ssid = "Redmi";
const char* password = "11111111";

const char* mqtt_server = "192.168.155.117";

WiFiClient espClient;
PubSubClient client(espClient);

// Biến đếm số lần gửi dữ liệu
int count = 0;

void setup_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("ESP IP: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to MQTT...");

    if (client.connect("ESP8266_ServerRoom")) {
      Serial.println("connected!");
    } else {
      Serial.print("failed, state=");
      Serial.print(client.state());
      Serial.println(" - retrying...");
      delay(1000);
    }
  }
}

void setup() {
  Serial.begin(9600);
  delay(1000);

  Serial.println();
  Serial.println("=== ESP8266 SERVER ROOM ===");

  setup_wifi();

  client.setServer(mqtt_server, 1883);

  Serial.print("MQTT Broker: ");
  Serial.println(mqtt_server);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  // Tăng số lần gửi dữ liệu
  count++;

  // Dữ liệu giả để test
  client.publish("server/temperature", "28.5");
  client.publish("server/humidity", "65");
  client.publish("server/smoke", "0");
  client.publish("server/door", "1");

  Serial.print("Data published - Lan gui thu: ");
  Serial.println(count);

  delay(5000);
}