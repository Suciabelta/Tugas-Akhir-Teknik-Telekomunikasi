#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_MLX90614.h>

// LCD I2C 16x2 address 0x27
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Sensor
MAX30105 particleSensor;
Adafruit_MLX90614 mlx = Adafruit_MLX90614();

// NRF24L01 config
RF24 radio(9, 10); // CE, CSN
const byte address[6] = "00001";
unsigned long lastSend = 0;

// HR calculation
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg;
int irOffset = 1054;
int perCent;

// ===========================
// Fungsi Regresi Linier Berdasarkan Grafik Excel
// ===========================
// Konversi MLX90612 ke Termometer Standar
float regresiSuhu(float mlxValue) {
  return (mlxValue + 12.462) / 1.3419;
}

// Konversi HR Sensor ke Standar HR
float regresiHR(float sensorHR) {
  return (sensorHR + 21.184) / 1.2051;
}

void setup() {
  Wire.begin();
  Serial.begin(9600);
  delay(500);

  // LCD init
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Siap");

  // MAX30105 init
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("❌ MAX30105 gagal"));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("MAX30105 ERR");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  // MLX90614 init
  if (!mlx.begin()) {
    Serial.println(F("❌ MLX90614 gagal"));
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("MLX90614 ERR");
    while (1);
  }

  // NRF24L01 init
  radio.begin();
  radio.setPALevel(RF24_PA_LOW);
  radio.openWritingPipe(address);
  radio.stopListening();

  delay(1000);
  lcd.clear();
}

void tampilNoBody() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tidak ada tubuh ");
  lcd.setCursor(0, 1);
  lcd.print("terdeteksi     ");
}

void tampilkanLCD(float bpm, int spo2, float suhu) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("HR:");
  lcd.print((int)bpm);
  lcd.print(" SPO2:");
  lcd.print(spo2);
  lcd.print("%");
  lcd.setCursor(0, 1);
  lcd.print("Suhu Tubuh:");
  lcd.print(suhu, 1);
  lcd.print(" C");
}

void loop() {
  long irValue = particleSensor.getIR();

  if (irValue < 50000) {
    tampilNoBody();
    Serial.println(F("⚠️ Tidak ada tubuh"));
    delay(200);
    return;
  }

  float suhuSensor = mlx.readObjectTempC();

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    beatsPerMinute = 60 / (delta / 1000.0);

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      beatAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++) beatAvg += rates[x];
      beatAvg /= RATE_SIZE;
    }

    perCent = irValue / irOffset;
    perCent = constrain(perCent, 0, 100);

    // --- Regresi Linier Berdasarkan Grafik Excel ---
    float hrRegresi   = regresiHR((float)beatAvg);         // HR dikonversi ke standar alat pembanding
    float suhuRegresi = regresiSuhu(suhuSensor);           // Suhu dikonversi ke standar termometer

    Serial.print(F("✅ HR : ")); Serial.print(hrRegresi, 0);
    Serial.print(F(" | SpO2 : ")); Serial.print(perCent);
    Serial.print(F(" | Suhu : ")); Serial.println(suhuRegresi, 1);

    tampilkanLCD(hrRegresi, perCent, suhuRegresi);

    // Kirim via NRF24L01 setiap 5 detik
    if (millis() - lastSend >= 5000) {
      char hrStr[8];
      char spo2Str[8];
      char suhuStr[8];

      dtostrf(hrRegresi, 4, 0, hrStr);     // HR hasil regresi, 0 digit desimal
      itoa(perCent, spo2Str, 10);          // SpO2 (int) ke string
      dtostrf(suhuRegresi, 4, 1, suhuStr); // Suhu hasil regresi, 1 digit desimal

      char text[40];
      snprintf(text, sizeof(text), "BPM:%s SpO2:%s Suhu:%s", hrStr, spo2Str, suhuStr);

      bool success = radio.write(&text, sizeof(text));
      if (success) {
        Serial.print("📤 Data terkirim: ");
        Serial.println(text);
      } else {
        Serial.println("❌ Gagal kirim NRF");
      }

      lastSend = millis();
    }

    delay(200);
  }
}