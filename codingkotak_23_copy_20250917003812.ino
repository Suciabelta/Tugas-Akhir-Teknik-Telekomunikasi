#include <nRF24L01.h>
#include <RF24.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <Wire.h>

// NRF24L01
RF24 radio(4, 5);
const byte address[6] = "00001";

// I2C Pin ESP32
#define I2C_SDA 16
#define I2C_SCL 17
LiquidCrystal_I2C lcd(0x27, 16, 2);

// WiFi & Server
const char* ssid = "nopiaaaa";
const char* password = "1234567899";
const char* serverURL = "http://172.20.10.2/iot-kesehatan/insert_sensor_data.php";
const char* userURL   = "http://172.20.10.2/iot-kesehatan/update_user.php";
const char* getuserURL = "http://172.20.10.2/iot-kesehatan/get_user.php";

// Telegram Bot
WiFiClientSecure secured_client;
UniversalTelegramBot bot("7843427008:AAFymcbdSQVTsDZ1fYWwk2EY7aDbP4PESHM", secured_client);
const String chat_id = "8136357799";

// Tombol
#define BTN_SET   32
#define BTN_GROUP 27    
#define BTN_INC   25
#define BTN_DEC   26

// Buzzer
#define BUZZER_PIN 12
bool abnormalDetected = false;
bool sentTelegram = false;
unsigned long lastTelegramTime = 0;
const unsigned long telegramCooldown = 10000; // 10 detik

// Kelompok umur
const char* groupText[4] = {"6-25", "26-45", "46-65", "66-85"};
int groupMin[4] = {6, 26, 46, 66};
int groupMax[4] = {25, 45, 65, 85};
int usia = 6;
int kelompok = 0;
bool setMode = false;
bool lastBtnSet = LOW, lastBtnGroup = LOW, lastBtnInc = LOW, lastBtnDec = LOW;

// NTP Time
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;
const int daylightOffset_sec = 0;

void setup() {
  Serial.begin(115200);

  // Tombol & Buzzer
  pinMode(BTN_SET, INPUT);
  pinMode(BTN_GROUP, INPUT);
  pinMode(BTN_INC, INPUT);
  pinMode(BTN_DEC, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // I2C LCD Setup
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init(); 
  lcd.backlight(); 
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi");

  // WiFi Connect
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); 
    lcd.print(".");
  }
  lcd.clear(); 
  lcd.setCursor(0, 0); lcd.print("WiFi Connected!");
  delay(1000);

  // NTP & SSL
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  secured_client.setInsecure();

  // Init User
  getUserFromServer();
  tampilStatus();

  // NRF24L01 Setup
  radio.begin();
  radio.openReadingPipe(0, address);
  radio.setPALevel(RF24_PA_MAX);
  radio.startListening();
}

void loop() {
  handleButtons();
  if (radio.available()) handleSensorData();
  handleBuzzer();
  delay(50);
}

void handleButtons() {
  // Cek Tombol
  bool nowSet   = digitalRead(BTN_SET);
  bool nowGroup = digitalRead(BTN_GROUP);
  bool nowInc   = digitalRead(BTN_INC);
  bool nowDec   = digitalRead(BTN_DEC);

  if (lastBtnSet == LOW && nowSet == HIGH) {
    bool prevSetMode = setMode;
    setMode = !setMode;
    if (setMode) { kelompok = 0; usia = groupMin[kelompok]; }
    tampilStatus();
    if (prevSetMode && !setMode) updateUserToServer();
  }
  lastBtnSet = nowSet;

  if (setMode) {
    if (lastBtnGroup == LOW && nowGroup == HIGH) {
      kelompok = (kelompok + 1) % 4;
      usia = groupMin[kelompok]; tampilStatus();
    }
    lastBtnGroup = nowGroup;

    if (lastBtnInc == LOW && nowInc == HIGH) {
      if (usia < groupMax[kelompok]) usia++;
      tampilStatus();
    }
    lastBtnInc = nowInc;

    if (lastBtnDec == LOW && nowDec == HIGH) {
      if (usia > groupMin[kelompok]) usia--;
      tampilStatus();
    }
    lastBtnDec = nowDec;
  }
}

// Cek Data NRF24L01
void handleSensorData() {
  char text[32] = "";
  radio.read(&text, sizeof(text));
  Serial.print("Diterima: "); Serial.println(text);

  int bpm = 0, spo2 = 0;
  float suhu = 0;
  sscanf(text, "BPM:%d SpO2:%d Suhu:%f", &bpm, &spo2, &suhu);
  tampilData(bpm, spo2, suhu);

  bool isAbnormal = (bpm < 60 || bpm > 100 || spo2 < 95 || suhu < 36.0 || suhu > 37.6);
  unsigned long now = millis();

  if (isAbnormal) {
    abnormalDetected = true;
    if (!sentTelegram || (now - lastTelegramTime > telegramCooldown)) {
      if (sendTelegramNotification(bpm, spo2, suhu)) {
        sentTelegram = true;
        lastTelegramTime = now;
      }
    }
  } else {
    abnormalDetected = false;
    sentTelegram = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Kirim ke Server
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String waktuESP = getFormattedTime();
    String postData = "bpm=" + String(bpm) + "&spo2=" + String(spo2) + "&suhu_tubuh=" + String(suhu, 2) + "&waktu_esp=" + waktuESP;
    int httpResponseCode = http.POST(postData);
    String response = http.getString();

    Serial.print("[SEND] "); Serial.println(postData);
    Serial.print("[RESPONSE] "); Serial.println(response);
    http.end();
  } else {
    Serial.println("[ERR] WiFi not connected!");
  }
}

// Buzzer Loop
void handleBuzzer() {
  static unsigned long lastBuzz = 0;
  if (abnormalDetected) {
    unsigned long now = millis();
    if ((now - lastBuzz) < 200) {
      digitalWrite(BUZZER_PIN, HIGH);
    } else if ((now - lastBuzz) < 400) {
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      lastBuzz = now;
    }
  }
}

void tampilStatus() {
  lcd.clear();
  if (setMode) {
    lcd.setCursor(0, 0); lcd.print("SET Kel :"); lcd.print(kelompok + 1);
    lcd.setCursor(0, 1); lcd.print("Usia : "); lcd.print(usia);
  } else {
    lcd.setCursor(0, 0); lcd.print("HR:--- SPO2:---%");
    lcd.setCursor(0, 1); lcd.print("T:--.- "); lcd.print(usia); lcd.print(" tahun");
  }
}

void tampilData(int bpm, int spo2, float suhu) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("HR:"); lcd.print(bpm); lcd.print(" SPO2:"); lcd.print(spo2); lcd.print("%");
  lcd.setCursor(0, 1); lcd.print("T:"); lcd.print(suhu, 1); lcd.print(" "); lcd.print(usia); lcd.print(" tahun");
}

void updateUserToServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(userURL);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String postData = "kelompok=" + String(kelompok + 1) + "&usia=" + String(usia);
    int httpResponseCode = http.POST(postData);
    String response = http.getString();
    Serial.print("[UPDATE USER] "); Serial.println(postData);
    Serial.print("[RESPONSE] "); Serial.println(response);
    http.end();

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("User Updated!");
    lcd.setCursor(0, 1); lcd.print("Kel:"); lcd.print(kelompok+1); lcd.print(" Usia:"); lcd.print(usia);
    delay(3000);
    tampilStatus();
  } else {
    Serial.println("[ERR] WiFi not connected (user update)!");
  }
}

void getUserFromServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(getuserURL);
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<128> doc;
      if (!deserializeJson(doc, payload)) {
        int kelompok_from_server = doc["kelompok"] | 0;
        int umur_from_server     = doc["umur"] | 6;
        kelompok = kelompok_from_server > 0 ? kelompok_from_server-1 : 0;
        usia = umur_from_server;
        Serial.print("Init User: kelompok="); Serial.print(kelompok+1);
        Serial.print(" umur="); Serial.println(usia);
      }
    }
    http.end();
  }
}

bool sendTelegramNotification(int bpm, int spo2, float suhu) {
  String msg = "🚨 *Abnormal Detected!*\n";
  msg += "❤️ *HR:* " + String(bpm) + " bpm\n";
  msg += "🩸 *SpO2:* " + String(spo2) + "%\n";
  msg += "🌡️ *Suhu:* " + String(suhu, 1) + " °C\n";
  msg += "👤 *Usia:* " + String(usia) + " tahun";

  for (int i = 0; i < 3; i++) {
    if (bot.sendMessage(chat_id, msg, "Markdown")) {
      Serial.println("[TELEGRAM] Notifikasi terkirim!");
      return true;
    } else {
      Serial.println("[TELEGRAM] Gagal kirim. Retry...");
      delay(1000);
    }
  }
  return false;
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "0000-00-00 00:00:00";
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}
