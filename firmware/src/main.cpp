#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// Pin assignments transcribed from the project document for ESP32 DevKit V1.
constexpr uint8_t RFID_SS_PIN = 5;
constexpr uint8_t RFID_RST_PIN = 22;
constexpr uint8_t LED_GREEN_PIN = 32;
constexpr uint8_t LED_RED_PIN = 33;
constexpr uint8_t BUZZER_PIN = 25;

MFRC522 reader(RFID_SS_PIN, RFID_RST_PIN);

String uidToHex(const MFRC522::Uid &uid) {
  String result;
  result.reserve(uid.size * 2);
  for (byte i = 0; i < uid.size; ++i) {
    if (uid.uidByte[i] < 0x10) result += '0';
    result += String(uid.uidByte[i], HEX);
  }
  result.toUpperCase();
  return result;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin(18, 19, 23, RFID_SS_PIN);  // SCK, MISO, MOSI, SS
  reader.PCD_Init();
  delay(50);
  Serial.println("GOGO SmartLab: RC522 reader ready; present a card.");
}

void loop() {
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) {
    delay(20);
    return;
  }

  const String uid = uidToHex(reader.uid);
  Serial.printf("RFID UID: %s\n", uid.c_str());

  // Reader proof-of-concept only: no card is granted access yet.
  // The next firmware increment will check the ESP32's local credential cache.
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);

  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
  delay(500);  // Debounce repeated reads while the same card remains in place.
}
