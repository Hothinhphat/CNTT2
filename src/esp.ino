#include <ESP8266WiFi.h>
#include <PubSubClient.h>

const char* ssid = "Redmi10x";
const char* password = "11111111";

const char* mqtt_server = "192.168.1.109";

WiFiClient espClient;
PubSubClient client(espClient);

void setup_wifi() {
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void reconnect() {
  while (!client.connected()) {
    client.connect("ESP8266_ServerRoom");
    delay(1000);
  }
}

void setup() {
  Serial.begin(9600);

  setup_wifi();

  client.setServer(mqtt_server, 1883);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }

  client.loop();

  // Dữ liệu giả để test
  client.publish("server/temperature", "28.5");
  client.publish("server/humidity", "65");
  client.publish("server/smoke", "0");
  client.publish("server/door", "1");
  //delay 5s
  delay(5000);
}