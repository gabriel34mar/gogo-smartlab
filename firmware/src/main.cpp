#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <MFRC522.h>
#include <SPI.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <vector>

#include "secrets.h"

#ifndef WIFI_SSID
#error "Define WIFI_SSID in firmware/secrets.h"
#endif
#ifndef WIFI_PASSWORD
#error "Define WIFI_PASSWORD in firmware/secrets.h"
#endif
#ifndef SMARTLAB_API_BASE_URL
#error "Define SMARTLAB_API_BASE_URL in firmware/secrets.h"
#endif
#ifndef SMARTLAB_DEVICE_TOKEN
#error "Define SMARTLAB_DEVICE_TOKEN in firmware/secrets.h"
#endif
#ifndef SMARTLAB_DEVICE_ID
#define SMARTLAB_DEVICE_ID "door-01"
#endif

// ESP32 DevKit V1 pin map from the project document.
constexpr uint8_t RFID_SS_PIN = 5;
constexpr uint8_t RFID_RST_PIN = 22;
constexpr uint8_t LED_GREEN_PIN = 32;
constexpr uint8_t LED_RED_PIN = 33;
constexpr uint8_t BUZZER_PIN = 25;

constexpr size_t MAX_QUEUED_EVENTS = 100;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t CREDENTIAL_SYNC_MS = 120000;
constexpr uint32_t EVENT_UPLOAD_MS = 5000;
constexpr uint32_t TIME_SYNC_MS = 300000;

const char *CREDENTIALS_PATH = "/credentials.json";
const char *EVENTS_PATH = "/events.json";

MFRC522 reader(RFID_SS_PIN, RFID_RST_PIN);
WiFiClient apiClient;
std::vector<String> authorizedUids;
bool credentialSnapshotLoaded = false;
uint32_t lastWifiAttempt = 0;
uint32_t lastCredentialSync = 0;
uint32_t lastEventUpload = 0;
uint32_t lastTimeSync = 0;

String apiUrl(const char *path) {
  String base = SMARTLAB_API_BASE_URL;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + path;
}

String normalizedUid(const MFRC522::Uid &uid) {
  String value;
  value.reserve(uid.size * 2);
  for (byte i = 0; i < uid.size; ++i) {
    if (uid.uidByte[i] < 0x10) value += '0';
    value += String(uid.uidByte[i], HEX);
  }
  value.toUpperCase();
  return value;
}

bool validUid(const String &uid) {
  if (uid.length() < 4 || uid.length() > 20) return false;
  for (size_t i = 0; i < uid.length(); ++i) {
    if (!isHexadecimalDigit(uid[i])) return false;
  }
  return true;
}

bool loadCredentialSnapshot() {
  File file = LittleFS.open(CREDENTIALS_PATH, "r");
  if (!file) return false;

  DynamicJsonDocument doc(12000);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error || !doc["credentials"].is<JsonArray>()) return false;

  std::vector<String> loaded;
  JsonArrayConst entries = doc["credentials"].as<JsonArrayConst>();
  for (JsonVariantConst entry : entries) {
    if (!entry.is<const char *>()) return false;
    String uid = entry.as<String>();
    uid.toUpperCase();
    if (!validUid(uid)) return false;
    loaded.push_back(uid);
  }
  authorizedUids = loaded;
  credentialSnapshotLoaded = true;
  Serial.printf("Loaded %u cached credentials.\n",
                static_cast<unsigned>(authorizedUids.size()));
  return true;
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  Serial.println("Connecting to local Wi-Fi...");
}

bool authorizedRequest(HTTPClient &http, const char *path) {
  if (!http.begin(apiClient, apiUrl(path))) return false;
  http.setTimeout(3000);
  http.addHeader("Authorization", String("Bearer ") + SMARTLAB_DEVICE_TOKEN);
  return true;
}

bool syncCredentialSnapshot() {
  HTTPClient http;
  if (!authorizedRequest(http, "/v1/credentials")) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Credential sync failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(12000);
  DeserializationError error = deserializeJson(doc, http.getString());
  http.end();
  if (error || !doc["credentials"].is<JsonArray>() ||
      !doc["version"].is<const char *>()) {
    Serial.println("Credential sync failed: invalid JSON response.");
    return false;
  }

  JsonArray entries = doc["credentials"].as<JsonArray>();
  std::vector<String> newUids;
  for (JsonVariant entry : entries) {
    if (!entry.is<const char *>()) return false;
    String uid = entry.as<String>();
    uid.toUpperCase();
    if (!validUid(uid)) return false;
    newUids.push_back(uid);
  }

  File file = LittleFS.open(CREDENTIALS_PATH, "w");
  if (!file) {
    Serial.println("Could not persist credential snapshot.");
    return false;
  }
  size_t written = serializeJson(doc, file);
  file.close();
  if (written == 0) return false;

  authorizedUids = newUids;
  credentialSnapshotLoaded = true;
  Serial.printf("Credential snapshot updated (%u cards, version %s).\n",
                static_cast<unsigned>(authorizedUids.size()),
                doc["version"].as<const char *>());
  return true;
}

bool syncClockFromPi() {
  HTTPClient http;
  if (!authorizedRequest(http, "/v1/time")) return false;
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, http.getString());
  http.end();
  if (error || !doc["utc_time"].is<const char *>()) return false;

  int year, month, day, hour, minute, second;
  const char *stamp = doc["utc_time"];
  if (sscanf(stamp, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day,
             &hour, &minute, &second) != 6) return false;

  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  time_t epoch = timegm(&parts);
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  Serial.println("Clock synchronized with Raspberry Pi.");
  return true;
}

String currentUtcTimestamp() {
  time_t now = time(nullptr);
  if (now < 1760000000) return "1970-01-01T00:00:00Z";
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

String makeEventId() {
  char id[40];
  snprintf(id, sizeof(id), "%08lx-%08lx-%08lx",
           static_cast<unsigned long>(ESP.getEfuseMac() & 0xffffffff),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return String(id);
}

bool readEventQueue(DynamicJsonDocument &doc) {
  File file = LittleFS.open(EVENTS_PATH, "r");
  if (!file) {
    doc.to<JsonArray>();
    return true;
  }
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  return !error && doc.is<JsonArray>();
}

bool saveEventQueue(JsonArrayConst queue) {
  File file = LittleFS.open(EVENTS_PATH, "w");
  if (!file) return false;
  size_t written = serializeJson(queue, file);
  file.close();
  return written > 0 || queue.size() == 0;
}

bool appendEvent(const String &uid, bool allowed) {
  DynamicJsonDocument doc(36000);
  if (!readEventQueue(doc)) {
    Serial.println("Event queue is unreadable; refusing to grant access.");
    return false;
  }
  JsonArray queue = doc.as<JsonArray>();
  if (queue.size() >= MAX_QUEUED_EVENTS) {
    Serial.println("Event queue full; refusing access until synchronization.");
    return false;
  }

  JsonObject event = queue.createNestedObject();
  event["event_id"] = makeEventId();
  event["device_id"] = SMARTLAB_DEVICE_ID;
  event["card_uid"] = uid;
  event["authorized"] = allowed;
  event["occurred_at"] = currentUtcTimestamp();
  event["timestamp_valid"] = time(nullptr) >= 1760000000;
  return saveEventQueue(queue);
}

void uploadQueuedEvents() {
  DynamicJsonDocument doc(36000);
  if (!readEventQueue(doc)) {
    Serial.println("Cannot read event queue; keeping local data.");
    return;
  }
  JsonArray queue = doc.as<JsonArray>();
  if (queue.size() == 0) return;

  DynamicJsonDocument requestDoc(36000);
  requestDoc["events"] = queue;
  String payload;
  serializeJson(requestDoc, payload);

  HTTPClient http;
  if (!authorizedRequest(http, "/v1/events/batch")) return;
  http.addHeader("Content-Type", "application/json");
  int status = http.POST(payload);
  if (status != HTTP_CODE_OK) {
    Serial.printf("Event upload failed: HTTP %d; keeping %u events.\n",
                  status, static_cast<unsigned>(queue.size()));
    http.end();
    return;
  }

  DynamicJsonDocument response(256);
  DeserializationError error = deserializeJson(response, http.getString());
  http.end();
  if (error || response["accepted"].as<size_t>() != queue.size()) {
    Serial.println("Server acknowledgement did not match; keeping local events.");
    return;
  }

  DynamicJsonDocument empty(64);
  empty.to<JsonArray>();
  if (saveEventQueue(empty.as<JsonArrayConst>())) {
    Serial.printf("Uploaded %u queued events.\n",
                  static_cast<unsigned>(queue.size()));
  }
}

bool isAuthorized(const String &uid) {
  if (!credentialSnapshotLoaded) return false;
  for (const String &authorizedUid : authorizedUids) {
    if (uid == authorizedUid) return true;
  }
  return false;
}

void signalAccess(bool allowed) {
  digitalWrite(LED_GREEN_PIN, allowed ? HIGH : LOW);
  digitalWrite(LED_RED_PIN, allowed ? LOW : HIGH);
  if (!allowed) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(180);
    digitalWrite(BUZZER_PIN, LOW);
  }
  delay(allowed ? 900 : 500);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);
}

void handleCard() {
  if (!reader.PICC_IsNewCardPresent() || !reader.PICC_ReadCardSerial()) return;

  String uid = normalizedUid(reader.uid);
  bool allowed = isAuthorized(uid);
  bool recorded = appendEvent(uid, allowed);
  if (!recorded) allowed = false;  // No grant when the audit event cannot persist.

  Serial.printf("UID=%s decision=%s cache=%s event_saved=%s\n",
                uid.c_str(), allowed ? "ALLOW" : "DENY",
                credentialSnapshotLoaded ? "loaded" : "missing",
                recorded ? "yes" : "no");
  signalAccess(allowed);
  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed; access will remain denied.");
  } else {
    loadCredentialSnapshot();
  }

  SPI.begin(18, 19, 23, RFID_SS_PIN);  // SCK, MISO, MOSI, SS
  reader.PCD_Init();
  delay(50);
  startWifi();
  lastCredentialSync = millis() - CREDENTIAL_SYNC_MS;
  lastTimeSync = millis() - TIME_SYNC_MS;
  Serial.println("GOGO SmartLab ready. Local cache authorizes cards when offline.");
}

void loop() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttempt >= WIFI_RETRY_MS) {
    startWifi();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastCredentialSync >= CREDENTIAL_SYNC_MS) {
      lastCredentialSync = now;
      syncCredentialSnapshot();
    }
    if (now - lastTimeSync >= TIME_SYNC_MS) {
      lastTimeSync = now;
      syncClockFromPi();
    }
    if (now - lastEventUpload >= EVENT_UPLOAD_MS) {
      lastEventUpload = now;
      uploadQueuedEvents();
    }
  }

  handleCard();
  delay(10);
}
