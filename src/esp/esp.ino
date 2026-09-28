#include <splash.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h> // Dùng cho màn hình OLED 1.3 inch

// ================= 1. CẤU HÌNH OLED 1.3" & CẢM BIẾN =================
#define OLED_RESET -1
#define i2c_Address 0x3c // Địa chỉ I2C OLED (thường là 0x3C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define BUTTON_PIN   0    // Chân D3 (GPIO 0) - Nút bấm chuyển trang
#define DHTPIN       2    // Chân D4 (GPIO 2) - DHT11
#define DHTTYPE      DHT11 
#define WATER_PIN    12   // Chân D6 (GPIO 12) - Cảm biến rò nước
#define SMOKE_AO     A0   // Chân Analog A0   - Cảm biến khói MQ-2
#define DOOR_PIN     14   // Chân D5 (GPIO 14) - Cảm biến cửa từ
#define BUZZER_PIN   13   // Chân D7 (GPIO 13) - Còi hú (Active LOW)

DHT dht(DHTPIN, DHTTYPE);

// ================= 2. MẠNG & HIVEMQ CLOUD =================
const char* ssid        = "Redmi";                           
const char* password    = "11111111";                        
const char* mqtt_server = "724ee7f0b4dc45f8b7b56148847ee0d6.s1.eu.hivemq.cloud"; 
const int   mqtt_port   = 8883;                              
const char* mqtt_user   = "hothinhphatvd4";                  
const char* mqtt_pass   = "Pmhh01947469";                  

WiFiClientSecure espClient;
PubSubClient client(espClient);

// Biến quản lý trạng thái
bool previousAlarmState = false;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = HIGH; // HIGH = Tắt (Active LOW)

// Biến chuyển trang & Debounce
int currentPage = 1; 
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

unsigned long lastReconnectAttempt = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastMqttSend = 0;

String wifiErrorDetail = "OK";
String mqttErrorDetail = "OK";

// ================= 3. CHẨN ĐOÁN LỖI MẠNG =================
String getWiFiErrorString(uint8_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:  return "ERR_NO_SSID";
    case WL_CONNECT_FAILED: return "ERR_W_PASS";
    case WL_DISCONNECTED:   return "ERR_DISCONN";
    default:                return "ERR_WIFI_FAIL";
  }
}

String getMQTTErrorString(int state) {
  switch (state) {
    case -4: return "ERR_TIMEOUT(rc:-4)";
    case -3: return "ERR_LOST   (rc:-3)";
    case -2: return "ERR_SVR_DOWN(rc:-2)";
    case 4:  return "ERR_BAD_CRED(rc:4)";
    case 5:  return "ERR_UNAUTH  (rc:5)";
    default: return "ERR_FAIL (" + String(state) + ")";
  }
}

// ================= 4. ĐỌC NÚT BẤM NON-BLOCKING =================
void checkButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static bool buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) { 
        currentPage++;
        if (currentPage > 2) currentPage = 1;
      }
    }
  }
  lastButtonState = reading;
}

// ================= 5. KẾT NỐI HIVEMQ CLOUD (SSL/TLS + LWT) =================
bool reconnectMQTT() {
  client.setKeepAlive(15);
  String clientId = "ESP8266_Server_" + String(random(0xffff), HEX);
  
  const char* willTopic   = "server/status";
  const char* willMessage = "OFFLINE";

  if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass, willTopic, 1, true, willMessage)) {
    client.publish("server/status", "ONLINE", true);
    mqttErrorDetail = "ONLINE (OK)";
    return true;
  } else {
    mqttErrorDetail = getMQTTErrorString(client.state());
    return false;
  }
}

// ================= 6. KHỞI TẠO (SETUP) =================
void setup() {
  Serial.begin(9600);
  
  // SỬA LỖI 1: Tắt còi ngay từ khi mở nguồn (Kéo HIGH trước)
  digitalWrite(BUZZER_PIN, HIGH);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(WATER_PIN, INPUT_PULLUP);
  pinMode(DOOR_PIN, INPUT_PULLUP);

  // Khởi tạo OLED 1.3"
  display.begin(i2c_Address, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("SERVER MONITOR 1.3\"");
  display.setCursor(10, 40);
  display.println("Connecting Cloud...");
  display.display();

  dht.begin();
  
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // Tắt tiết kiệm điện Wi-Fi
  WiFi.begin(ssid, password);
  
  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);
}

// ================= 7. LUỒNG CHÍNH (LOOP) =================
void loop() {
  // 1. Quét nút bấm liên tục
  checkButton();

  // 2. Quản lý Mạng ngầm
  if (WiFi.status() == WL_CONNECTED) {
    wifiErrorDetail = "CONNECTED";
    if (!client.connected()) {
      if (millis() - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = millis();
        reconnectMQTT();
      }
    } else {
      client.loop();
    }
  } else {
    wifiErrorDetail = getWiFiErrorString(WiFi.status());
    mqttErrorDetail = "WAITING_WIFI";
    if (millis() - lastWiFiRetry > 10000) {
      lastWiFiRetry = millis();
      WiFi.begin(ssid, password);
    }
  }

  // 3. Đọc dữ liệu cảm biến & Xử lý Logic
  bool hasAlarm = false;

  // --- A. NHIỆT ĐỘ & ĐỘ ẨM (DHT11) ---
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  float sendTemp = isnan(temp) ? -999.0 : temp;
  float sendHum  = isnan(hum)  ? -999.0 : hum;

  String dispTemp = "ERR";
  if (!isnan(temp)) {
    if (temp > 35.0) {
      dispTemp = String(temp, 1) + "C [ALARM!]";
      hasAlarm = true; // Kích hoạt Alarm khi quá nhiệt
    } else {
      dispTemp = String(temp, 1) + " C";
    }
  }

  String dispHum = "ERR";
  if (!isnan(hum)) {
    dispHum = String(hum, 1) + " %";
  }

  // --- B. KHÓI (MQ-2 Analog) ---
  int smokeAnalog = analogRead(SMOKE_AO);
  int sendSmoke = 0;
  String dispSmoke = "OK";

  if (smokeAnalog < 15 || smokeAnalog > 1015) {
    sendSmoke = -1;      // Lỗi tuột dây
    dispSmoke = "ERR";
  } else if (smokeAnalog > 400) {
    sendSmoke = 1;       // Phát hiện khói
    dispSmoke = "ALARM!";
    hasAlarm = true;     // Kích hoạt Alarm khi có khói
  }

  // --- C. RÒ NƯỚC (Digital) ---
  int isWater = (digitalRead(WATER_PIN) == LOW) ? 1 : 0;
  String dispWater = (isWater == 1) ? "ALARM!" : "OK";
  if (isWater == 1) hasAlarm = true;

  // --- D. CỬA TỪ (Digital) ---
  int isDoor = (digitalRead(DOOR_PIN) == HIGH) ? 1 : 0;
  String dispDoor = (isDoor == 1) ? "OPEN [ALARM!]" : "CLOSED";
  if (isDoor == 1) hasAlarm = true;

  if (hasAlarm) {
    // Khi vừa phát hiện sự cố lần đầu: Ép còi kêu NGAY LẬP TỨC và nhảy về Trang 1
    if (!previousAlarmState) {
      currentPage = 1;
      buzzerState = LOW; // Kêu ngay (Active LOW)
      digitalWrite(BUZZER_PIN, LOW);
      lastBuzzerToggle = millis();
    } else {
      // Sau đó duy trì nhịp ngắt quãng 1000ms (1s kêu / 1s tắt)
      if (millis() - lastBuzzerToggle > 1000) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      }
    }
  } else {
    // Tắt còi hoàn toàn khi không có sự cố
    buzzerState = HIGH;
    digitalWrite(BUZZER_PIN, HIGH);
  }
  previousAlarmState = hasAlarm;

  // 4. GỬI DỮ LIỆU SẠCH LÊN MQTT SERVER (2s/lần)
  if (client.connected() && (millis() - lastMqttSend > 2000)) {
    lastMqttSend = millis();

    String jsonPayload = "{";
    jsonPayload += "\"temp\":" + String(sendTemp, 1) + ",";
    jsonPayload += "\"hum\":" + String(sendHum, 1) + ",";
    jsonPayload += "\"water\":" + String(isWater) + ",";
    jsonPayload += "\"smoke\":" + String(sendSmoke) + ",";
    jsonPayload += "\"door\":" + String(isDoor);
    jsonPayload += "}";

    client.publish("server/telemetry", jsonPayload.c_str());
  }

  // 5. HIỂN THỊ OLED 1.3"
  if (millis() - lastDisplayUpdate > 200) {
    lastDisplayUpdate = millis();

    display.clearDisplay();
    display.setTextSize(1);

    if (currentPage == 1) {
      // ---------------- TRANG 1: SENSORS DASHBOARD ----------------
      display.setCursor(4, 0);
      display.println("SENSORS (1/2)");
      
      display.setCursor(0, 16);
      display.print("Temp: "); display.println(dispTemp);

      display.setCursor(0, 28);
      display.print("Hum : "); display.println(dispHum);

      display.setCursor(0, 40);
      display.print("Water: "); display.print(dispWater);
      display.print(" |Smoke:"); display.println(dispSmoke);

      display.setCursor(0, 52);
      display.print("Door: "); display.println(dispDoor);

    } else if (currentPage == 2) {
      // ---------------- TRANG 2: NETWORK & INFRASTRUCTURE ----------------
      display.setCursor(4, 0);
      display.println("NETWORK (2/2)");

      display.setCursor(0, 16);
      display.print("WiFi: ");
      if (WiFi.status() == WL_CONNECTED) {
        display.println(WiFi.SSID());
      } else {
        display.println(wifiErrorDetail);
      }

      display.setCursor(0, 28);
      display.print("IP  : ");
      if (WiFi.status() == WL_CONNECTED) display.println(WiFi.localIP().toString());
      else display.println("N/A");

      display.setCursor(0, 40);
      display.print("MQTT: "); display.println(mqttErrorDetail);

      display.setCursor(0, 52);
      display.print("Server: HiveMQ Cloud");
    }

    display.display();
  }
}
