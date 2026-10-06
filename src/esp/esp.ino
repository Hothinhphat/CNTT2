#include <splash.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>             // Thư viện WiFiManager (by tzapu)
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>            // Dùng cho màn hình OLED 1.3 inch

// ================= 1. CẤU HÌNH OLED 1.3" & CẢM BIẾN =================
#define OLED_RESET    -1
#define i2c_Address   0x3c              // Địa chỉ I2C OLED (thường là 0x3C)
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define BUTTON_PIN    0                 // Chân D3 (GPIO 0) - Nút đa năng
#define DHTPIN        2                 // Chân D4 (GPIO 2) - DHT11
#define DHTTYPE       DHT11 
#define WATER_PIN     12                // Chân D6 (GPIO 12) - Cảm biến rò nước
#define SMOKE_AO      A0                // Chân Analog A0   - Cảm biến khói MQ-2
#define DOOR_PIN      14                // Chân D5 (GPIO 14) - Cảm biến cửa từ
#define BUZZER_PIN    13                // Chân D7 (GPIO 13) - Còi hú (Active LOW)

DHT dht(DHTPIN, DHTTYPE);

// ================= 2. MẠNG & HIVEMQ CLOUD =================
const char* mqtt_server = "724ee7f0b4dc45f8b7b56148847ee0d6.s1.eu.hivemq.cloud"; 
const int   mqtt_port   = 8883;                             
const char* mqtt_user   = "hothinhphatvd4";                   
const char* mqtt_pass   = "Pmhh01947469";                   

WiFiClientSecure espClient;
PubSubClient client(espClient);

ESP8266WebServer server(80);
WiFiManager wm; // Biến WebPortal toàn cục

// Biến quản lý trạng thái còi & Báo động
bool previousAlarmState = false;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = HIGH;                // HIGH = Tắt (Active LOW)

// Biến Mute còi 15 phút
bool isMuted = false;
unsigned long muteStartTime = 0;
const unsigned long MUTE_DURATION = 15 * 60 * 1000; // 15 phút = 900,000 ms

// Biến chuyển trang & Debounce Nút bấm
int currentPage = 1; 
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

unsigned long lastDisplayUpdate = 0;
unsigned long lastMqttSend = 0;

String wifiErrorDetail = "OK";
String mqttErrorDetail = "OK";

// ================= 3. CHẨN ĐOÁN LỖI MẠNG =================
String getMQTTErrorString(int state) {
  switch (state) {
    case -4: return F("ERR_TIMEOUT(rc:-4)");
    case -3: return F("ERR_LOST   (rc:-3)");
    case -2: return F("ERR_SVR_DOWN(rc:-2)");
    case 4:  return F("ERR_BAD_CRED(rc:4)");
    case 5:  return F("ERR_UNAUTH  (rc:5)");
    default: return "ERR_FAIL (" + String(state) + ")";
  }
}

// ================= 4. CẤU HÌNH WIFIMANAGER & WEB OTA =================
void setupWiFiManager() {
  // Hiển thị nút bấm "Update Firmware" trực tiếp trên Menu chính
  std::vector<const char *> menu = {"wifi", "update", "info", "exit"};
  wm.setMenu(menu);  
  // wm.setWebPortalClientCheck(true); //xác thực http
  // wm.setConfigPortalPassword("12345678"); // Mật khẩu bảo vệ WebPortal

  // Thời gian chờ trang cấu hình (10s kết nối, 20s phát AP)
  wm.setConnectTimeout(10);
  wm.setConfigPortalTimeout(20);

  // Tên Wi-Fi AP phát ra khi chưa kết nối được mạng cũ
  if (!wm.autoConnect("ESP8266_Server_Config", "12345678")) {
    wifiErrorDetail = "AP Mode";
    Serial.println(F("Co loi WiFi, dang chay che do Offline/AP..."));
  } else {
    wifiErrorDetail = "CONNECTED";
    Serial.println(F("WiFi Connected Successfully!"));
  }
  wm.startWebPortal(); // Luôn duy trì WebPortal ngầm trên IP nội bộ
}

// ================= 5. ĐỌC NÚT BẤM ĐA NĂNG =================
void checkButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static bool buttonState = HIGH;
    static unsigned long pressStartTime = 0;

    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) { 
        pressStartTime = millis(); 
      } else { 
        unsigned long pressDuration = millis() - pressStartTime;

        if (pressDuration >= 5000) {
          // NHẤN GIỮ > 5s: Xóa Wi-Fi cũ và mở lại Portal Cấu hình Web
          wm.resetSettings();
          ESP.restart();
        } else if (pressDuration >= 1500) {
          // NHẤN GIỮ > 1.5s: Bật/Tắt Còi tạm thời 15 phút (Mute Mode)
          isMuted = !isMuted;
          if (isMuted) muteStartTime = millis();
        } else {
          // NHẤN NGẮN < 1.5s: Chuyển trang OLED (Trang 1 / Trang 2)
          currentPage++;
          if (currentPage > 2) currentPage = 1;
        }
      }
    }
  }
  lastButtonState = reading;
}

// ================= 6. KẾT NỐI HIVEMQ CLOUD (SSL/TLS + LWT) =================
bool reconnectMQTT() {
  client.setKeepAlive(15);
  String clientId = "ESP8266_Server_" + String(ESP.getChipId());
  
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

// ================= 7. KHỞI TẠO (SETUP) =================
void setup() {
  Serial.begin(9600);
  
  // 1. Tắt còi ngay từ khi mở nguồn (Active LOW)
  digitalWrite(BUZZER_PIN, HIGH);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(WATER_PIN, INPUT_PULLUP);
  pinMode(DOOR_PIN, INPUT_PULLUP);

  // 2. Khởi tạo OLED 1.3"
  display.begin(i2c_Address, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 15);
  display.println(F("SERVER MONITOR 1.3\""));
  display.setCursor(10, 35);
  display.println(F("Warming up sensors.."));
  display.display();

  // Chờ 3s ổn định nguồn & dây sấy MQ-2
  delay(10000);
  dht.begin();
  
  // 3. Cấu hình Wi-Fi & Tối ưu RAM SSL 
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  
  espClient.setInsecure();
  espClient.setBufferSizes(512, 512); // Tối ưu buffer SSL giúp dư ra ~10KB RAM

  // Cập nhật màn hình báo đang nối Wi-Fi
  display.setCursor(10, 50);
  display.println(F("Connecting WiFi..."));
  display.display();

  // 4. Khởi chạy WiFiManager

  
  setupWiFiManager();
  

  server.begin();

  // 6. Cấu hình MQTT Client
  client.setServer(mqtt_server, mqtt_port);
}

// ================= 8. LUỒNG CHÍNH (LOOP) =================
void loop() {
  // Duy trì WebPortal
  wm.process();

  // 1. Quét nút bấm đa năng liên tục
  checkButton();

  // 2. Duy trì kết nối MQTT
  if (WiFi.status() == WL_CONNECTED) {
    wifiErrorDetail = "CONNECTED";
    if (!client.connected()) {
      static unsigned long lastReconnectAttempt = 0;
      if (millis() - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = millis();
        reconnectMQTT();
      }
    } else {
      client.loop();
    }
  } else {
    wifiErrorDetail = "NO_WIFI";
    mqttErrorDetail = "WAITING_WIFI";
  }

  // Chống tràn RAM (Smart Auto-Recovery)
  if (ESP.getFreeHeap() < 8192) {
    if (client.connected()) {
      client.publish("server/status", "CRITICAL_LOW_RAM_REBOOTING");
    }
    delay(500);
    ESP.restart();
  }

  // Kiểm tra hết thời gian Mute còi 15 phút chưa
  if (isMuted && (millis() - muteStartTime > MUTE_DURATION)) {
    isMuted = false;
  }

  // 3. Đọc dữ liệu cảm biến & Xử lý Logic Báo động
  bool hasAlarm = false;

  // --- A. NHIỆT ĐỘ & ĐỘ ẨM (DHT11) ---
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  float sendTemp = isnan(temp) ? -999.0 : temp;
  float sendHum  = isnan(hum)  ? -999.0 : hum;

  String dispTemp = "ERR";
  if (isnan(temp)) {
    hasAlarm = true; // DHT11 lỗi -> Kêu còi
  } else {
    if (temp > 35.0) {
      dispTemp = String(temp, 1) + "C [ALARM!]";
      hasAlarm = true; 
    } else {
      dispTemp = String(temp, 1) + " C";
    }
  }

  String dispHum = "ERR";
  if (isnan(hum)) {
    hasAlarm = true; // DHT11 lỗi -> Kêu còi
  } else {
    dispHum = String(hum, 1) + " %";
  }

  // --- B. KHÓI (MQ-2 Analog A0) ---
  int smokeAnalog = analogRead(SMOKE_AO);
  int sendSmoke = 0;
  String dispSmoke = "OK";
  
  if (smokeAnalog < 80 || smokeAnalog > 1000) {
    sendSmoke = -1;      // Lỗi tuột dây / Hỏng cảm biến
    dispSmoke = "ERR";
    hasAlarm = true;     // MQ-2 tuột dây -> Kêu còi
  } else if (smokeAnalog > 400) {
    sendSmoke = 1;       // Phát hiện khói
    dispSmoke = "ALARM!";
    hasAlarm = true;     
  }

  // --- C. RÒ NƯỚC (Digital D6) ---
  int isWater = (digitalRead(WATER_PIN) == LOW) ? 1 : 0;
  String dispWater = (isWater == 1) ? "ALARM!" : "OK";
  if (isWater == 1) hasAlarm = true;

  // --- D. CỬA TỪ (Digital D5) ---
  int isDoor = (digitalRead(DOOR_PIN) == HIGH) ? 1 : 0;
  String dispDoor = (isDoor == 1) ? "OPEN [ALARM!]" : "CLOSED";
  if (isDoor == 1) hasAlarm = true;

  // 4. ĐIỀU KHIỂN CÒI BÁO ĐỘNG NGẮT QUẢNG 2000MS (CÓ MUTE MODE)
  if (hasAlarm && !isMuted) {
    if (!previousAlarmState) {
      currentPage = 1;
      buzzerState = LOW; // Active LOW -> Kêu ngay
      digitalWrite(BUZZER_PIN, LOW);
      lastBuzzerToggle = millis();
    } else {
      if (millis() - lastBuzzerToggle > 2000) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      }
    }
  } else {
    buzzerState = HIGH;
    digitalWrite(BUZZER_PIN, HIGH);
  }
  previousAlarmState = hasAlarm;

  // 5. GỬI DỮ LIỆU LÊN HIVE MQ CLOUD (2s/lần)
  if (client.connected() && (millis() - lastMqttSend > 2000)) {
    lastMqttSend = millis();

    char jsonPayload[160];
    snprintf(jsonPayload, sizeof(jsonPayload),
      "{\"temp\":%.1f,\"hum\":%.1f,\"water\":%d,\"smoke\":%d,\"door\":%d,\"heap\":%u}",
      sendTemp, sendHum, isWater, sendSmoke, isDoor, ESP.getFreeHeap());

    client.publish("server/telemetry", jsonPayload);
  }

  // 6. HIỂN THỊ OLED 1.3" (200ms/lần)
  if (millis() - lastDisplayUpdate > 200) {
    lastDisplayUpdate = millis();

    display.clearDisplay();
    display.setTextSize(1);

    if (currentPage == 1) {
      // ---------------- TRANG 1: SENSORS DASHBOARD ----------------
      display.setCursor(4, 0);
      if (isMuted) display.println(F("SENSORS [MUTED]"));
      else         display.println(F("SENSORS (1/2)"));
      
      display.setCursor(0, 16);
      display.print(F("Temp : ")); display.println(dispTemp);

      display.setCursor(0, 28);
      display.print(F("Hum  : ")); display.println(dispHum);

      display.setCursor(0, 40);
      display.print(F("Water: ")); display.print(dispWater);
      display.print(F(" |Smoke:")); display.println(dispSmoke);

      display.setCursor(0, 52);
      display.print(F("Door : ")); display.println(dispDoor);

    } else if (currentPage == 2) {
      // ---------------- TRANG 2: NETWORK & INFRASTRUCTURE ----------------
      display.setCursor(4, 0);
      display.println(F("NETWORK (2/2)"));

      display.setCursor(0, 16);
      display.print(F("WiFi: "));
      if (WiFi.status() == WL_CONNECTED) {
        display.println(WiFi.SSID());
      } else {
        display.println(wifiErrorDetail);
      }

      display.setCursor(0, 28);
      display.print(F("IP  : "));
      if (WiFi.status() == WL_CONNECTED) display.println(WiFi.localIP().toString());
      else display.println(F("N/A"));

      display.setCursor(0, 40);
      display.print(F("MQTT: ")); display.println(mqttErrorDetail);

      uint32_t freeHeapBytes = ESP.getFreeHeap();
      uint32_t freeHeapKB    = freeHeapBytes / 1024;
      int freePercent        = (freeHeapBytes * 100) / (80 * 1024);

      display.setCursor(0, 52);
      display.print(F("RAM : ")); 
      display.print(freeHeapKB);
      display.print(F("K/80K ("));
      display.print(freePercent);
      display.print(F("%)"));
    }

    display.display();
  }
}
