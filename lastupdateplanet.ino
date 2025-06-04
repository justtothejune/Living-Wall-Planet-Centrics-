#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ModbusMaster.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <time.h>

// ==== PINS ==== 
#define TXD2           17
#define RXD2           16
#define DE_RE_PIN      5
#define LED_RELAY_PIN  4
#define PUMP_PIN       15
#define PZEM_RX_PIN    21
#define PZEM_TX_PIN    22

// ==== TIMING ==== 
const int READ_INTERVAL       = 5;      // วินาที ระหว่างอ่าน PPFD
const int UPLOAD_INTERVAL     = 300;    // วินาที (5 นาที) ระหว่างอัปโหลด
const unsigned long CONFIG_INTERVAL = 10000UL; // มิลลิวินาที ระหว่างดึง config

// ==== HARDWARE ==== 
HardwareSerial modbusSerial(1);
HardwareSerial pzemSerial(2);
ModbusMaster   node;
PZEM004Tv30    pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);
WiFiClientSecure client;

// ==== NETWORK & SCRIPT URLS ==== 
const char* ssid       = "PlanetCentric";
const char* wifiPass   = "BearLab!";
const char* webAppUrl  =
  "https://script.google.com/macros/s/AKfycbz7yTwAljVhchIorHOSY_nlYFPVmpvG2-JQ9HGMNmcxKcVX_tPg7OlRoqe46C1eLB5K/exec";

// ==== STATE VARIABLES ==== 
unsigned long lastReadTime    = 0;
unsigned long lastUploadTime  = 0;
unsigned long lastConfigFetch = 0;

// DLI tracking
float sheetDliTarget = 6.0f;   // ค่าจาก Google Sheets หรือ Manual
float dynamicDLI      = 6.0f;   // DLI target ปัจจุบัน
float dliAcc         = 0.0f;    // ปริมาณ DLI ที่สะสม (mol/m²/day)
float remainingDLI   = 0.0f;    // เหลือ DLI (for UI)

// PAR (PPFD) buffer
#define MAX_READINGS 60
float ppfd[MAX_READINGS];
int   idx = 0, countReads = 0;

// LED control
String        ledMode         = "Automatic";  // "On", "Off", "Automatic"
bool          ledAutoActive   = false;        // สถานะว่าไฟติดโดย Auto หรือไม่
bool          ledWasOn        = false;        // ช่วงที่ไฟติดแล้วเพิ่งปิดโดย Auto
bool          ledManualActive = false;        // สถานะว่าไฟติดโดย Manual หรือไม่
unsigned long ledStart        = 0;            // เวลาเริ่มเปิดไฟรอบล่าสุด (millis)
unsigned long lastLedDur      = 0;            // ระยะเวลารอบสุดท้ายเมื่อไฟ Auto ปิด
unsigned long accumLedMs      = 0;            // เวลาสะสม LED On Time ต่อวัน (ms)

// Water schedule
struct WaterSchedule {
  bool en;
  uint32_t startSec;
  uint32_t durMs;
};
WaterSchedule water[7];
bool triggered[7];
int  lastDayOfWeek = -1;
bool pumpOn       = false;
unsigned long pumpStart = 0;

// Daily reset tracker 
int lastResetDay  = -1;  // เก็บ tm_yday ของวันที่รีเซ็ตล่าสุด

// “Seconds-of-day” tracking for edge detection 
int prevNowS     = -1;   

char sheetDate[11];
const char* WEEKDAYS[7] = {
  "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

// Energy baseline (kWh) หลังรีเซ็ต
float baselineEnergy = 0.0f;

void preTx()  { digitalWrite(DE_RE_PIN, HIGH); }
void postTx() { digitalWrite(DE_RE_PIN, LOW);  }

// ===== ฟังก์ชันเชื่อม Wi-Fi =====
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to \"%s\"\n", ssid);
  WiFi.begin(ssid, wifiPass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) {  
      Serial.println("[WiFi] Connection timeout");
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected, IP=" + WiFi.localIP().toString());
  client.setInsecure();
}

// ===== ฟังก์ชันตั้งค่า DLI แบบ Manual =====
void setDliManual(float newDli) {
  sheetDliTarget = newDli;
  dynamicDLI     = newDli;
  dliAcc         = 0.0f;
  remainingDLI   = newDli;
  Serial.printf("[MANUAL] DLI manually set to %.2f; dliAcc reset\n", newDli);
}

// ===== ฟังก์ชันดึง JSON config จาก Google Apps Script =====
void fetchConfig() {
  Serial.println("\n[CONFIG] fetchConfig()");
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, webAppUrl)) {
    Serial.println("[CONFIG] HTTP begin failed");
    return;
  }
  int code = http.GET();
  String payload = http.getString();
  http.end();

  Serial.printf("[CONFIG] GET %s → %d\n", webAppUrl, code);
  Serial.println("[CONFIG] JSON:\n" + payload);
  if (code != HTTP_CODE_OK) return;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[CONFIG] JSON parse error: %s\n", err.c_str());
    return;
  }

  // ----- 1. ปรับค่า DLI target -----
  float fD = doc["dli_target"] | sheetDliTarget;
  if (fabs(fD - sheetDliTarget) > 0.001f) {
    sheetDliTarget = fD;
    dynamicDLI     = fD;
    dliAcc         = 0.0f;
    remainingDLI   = fD;
    Serial.printf("[CONFIG] DLI target -> %.2f (reset)\n", dynamicDLI);
  }

  // ----- 2. ปรับโหมด LED (Handle Mode Change) -----
  String oldMode = ledMode;
  ledMode = doc["led"] | ledMode;
  if (oldMode != ledMode) {
    Serial.printf("[CONFIG] LED mode changed: %s -> %s\n", oldMode.c_str(), ledMode.c_str());

    bool wasManual = ledManualActive;
    bool wasAuto   = ledAutoActive;

    ledAutoActive   = false;
    ledWasOn        = false;
    lastLedDur      = 0;
    ledManualActive = false;

    if (ledMode == "Automatic") {
      unsigned long nowMs = millis();
      if (wasAuto) {
        lastLedDur   = nowMs - ledStart;
        accumLedMs  += lastLedDur;
        ledWasOn     = true;
      }
      else if (wasManual) {
        accumLedMs += (nowMs - ledStart);
      }

      time_t tt = time(nullptr);
      struct tm* t = localtime(&tt);
      int currentSecFull = t->tm_hour*3600 + t->tm_min*60 + t->tm_sec;
      int targetSec;
      if (t->tm_hour > 6 || (t->tm_hour == 6 && (t->tm_min > 0 || t->tm_sec > 0))) {
        targetSec = (6 + 24)*3600;
      } else {
        targetSec = 6*3600;
      }
      float hrsLeft = (targetSec - currentSecFull) / 3600.0f;
      float need    = (dynamicDLI - dliAcc) / 0.36f;

      if (need > hrsLeft) {
        digitalWrite(LED_RELAY_PIN, HIGH);
        ledAutoActive = true;
        ledStart      = nowMs;
        Serial.println("[CONFIG] Auto ON because need > hrsLeft");
      }
      else {
        digitalWrite(LED_RELAY_PIN, LOW);
        Serial.println("[CONFIG] Auto OFF immediately (need <= hrsLeft)");
      }
      Serial.println("[CONFIG] Entered Automatic mode");
    }
    else if (ledMode == "On") {
      digitalWrite(LED_RELAY_PIN, HIGH);
      ledManualActive = true;
      ledStart        = millis();
      Serial.println("[CONFIG] Entered Manual On mode, ledStart = millis()");
    }
    else { 
      if (wasAuto) {
        unsigned long nowMs = millis();
        lastLedDur   = nowMs - ledStart;
        accumLedMs  += lastLedDur;
        ledWasOn     = true;
      }
      else if (wasManual) {
        accumLedMs += (millis() - ledStart);
      }
      digitalWrite(LED_RELAY_PIN, LOW);
      Serial.println("[CONFIG] Switch to Off, LED turned OFF immediately");
    }
  }

  // ----- 3. ปรับตารางการรดน้ำ -----
  Serial.println("[CONFIG] Watering schedule:");
  for (int i = 0; i < 7; i++) {
    water[i].en  = false;
    triggered[i] = false;
  }
  if (doc.containsKey("watering")) {
    for (JsonPair kv : doc["watering"].as<JsonObject>()) {
      String day = String(kv.key().c_str());
      day.trim();
      JsonObject e = kv.value().as<JsonObject>();
      const char* ts = e["time"];
      int dur = e["duration"];

      int hh=0, mm=0;
      if (sscanf(ts, "%d:%d", &hh, &mm) != 2) {
        Serial.printf("[CONFIG] Invalid time format: %s\n", ts);
        continue;
      }
      for (int d = 0; d < 7; d++) {
        if (day.equalsIgnoreCase(WEEKDAYS[d])) {
          water[d].en       = true;
          water[d].startSec = hh*3600 + mm*60;
          water[d].durMs    = dur*1000UL;
          Serial.printf("  %s @%02d:%02d for %ds\n",
                        WEEKDAYS[d], hh, mm, dur);
          break;
        }
      }
    }
  }
}

// ===== ฟังก์ชันคำนวณและควบคุม LED ในโหมด Automatic =====
void applyAutoLED(unsigned long m) {
  time_t tt = time(nullptr);
  struct tm* t = localtime(&tt);

  int currentSecFull = t->tm_hour*3600 + t->tm_min*60 + t->tm_sec;
  int targetSec;
  if (t->tm_hour > 6 || (t->tm_hour == 6 && (t->tm_min > 0 || t->tm_sec > 0))) {
    targetSec = (6 + 24)*3600;
  } else {
    targetSec = 6*3600;
  }
  float hrsLeft = (targetSec - currentSecFull) / 3600.0f;
  float need    = (dynamicDLI - dliAcc) / 0.36f;
  Serial.printf("[LED][AUTO] dliAcc=%.3f, dynamicDLI=%.3f, hrsLeft=%.3f, need=%.3f\n",
                dliAcc, dynamicDLI, hrsLeft, need);

  if (!ledAutoActive && need > hrsLeft) {
    digitalWrite(LED_RELAY_PIN, HIGH);
    ledAutoActive = true;
    ledStart      = m;
    Serial.println("[LED][AUTO] LED ON (auto)");
    return;
  }

  if (ledAutoActive && (dliAcc >= dynamicDLI || need <= hrsLeft)) {
    digitalWrite(LED_RELAY_PIN, LOW);
    ledAutoActive = false;
    lastLedDur    = m - ledStart;
    ledWasOn      = true;
    Serial.println("[LED][AUTO] LED OFF (auto due to DLI or need ≤ hrsLeft)");
  }
}

// ===== ฟังก์ชัน setup =====
void setup() {
  Serial.begin(115200);
  delay(100);

  modbusSerial.begin(9600, SERIAL_8N1, RXD2, TXD2);
  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  node.begin(1, modbusSerial);
  node.preTransmission(preTx);
  node.postTransmission(postTx);

  pinMode(DE_RE_PIN, OUTPUT);
  pinMode(LED_RELAY_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(LED_RELAY_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW);

  connectWiFi();

  configTime(7*3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[TIME] Syncing time");
  time_t now = time(nullptr);
  unsigned long startSync = millis();
  while (now < 7LL*3600) {
    if (millis() - startSync > 20000) { 
      Serial.println("\n[TIME] Sync timeout");
      break;
    }
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  if (now >= 7LL*3600) {
    Serial.println("\n[TIME] Time synced");
  }

  struct tm* t = localtime(&now);
  if (t->tm_hour < 6) {
    now -= 86400;  
    t = localtime(&now);
  }
  strftime(sheetDate, sizeof(sheetDate), "%Y-%m-%d", t);
  Serial.printf("[INIT] sheetDate = %s\n", sheetDate);

  prevNowS     = t->tm_hour*3600 + t->tm_min*60 + t->tm_sec;
  lastDayOfWeek= t->tm_wday;

  // ตั้ง lastResetDay ให้เป็น "เมื่อวาน" (tm_yday - 1) เพื่อให้ไม่รีเซ็ตซ้ำทันที
  lastResetDay = (t->tm_yday == 0) ? 364 : (t->tm_yday - 1);

  lastUploadTime = 0;

  baselineEnergy = 0.0f;

  fetchConfig(); 
}

// ===== ฟังก์ชัน loop =====
void loop() {
  unsigned long m = millis();

  // 1. Fetch config ทุก CONFIG_INTERVAL
  if (m - lastConfigFetch >= CONFIG_INTERVAL) {
    lastConfigFetch = m;
    fetchConfig();
  }

  // 2. อ่านเวลาปัจจุบัน
  time_t tt = time(nullptr);
  struct tm* tp = localtime(&tt);
  int wday      = tp->tm_wday;
  int localHour = tp->tm_hour;
  int localMin  = tp->tm_min;
  int localSec  = tp->tm_sec;
  int today     = tp->tm_yday;
  int nowS      = localHour*3600 + localMin*60 + localSec;

  // 3. “ทดสอบ Reset ที่ 6:00” (Edge Detection)
  if ((prevNowS < (6*3600)) && (nowS >= (6*3600)) && (today != lastResetDay)) {
    lastResetDay = today;

    // 3.1 รีเซ็ต DLI
    dliAcc       = 0.0f;
    remainingDLI = dynamicDLI;

    // 3.2 รีเซ็ต LED On Time ทุกโหมด
    accumLedMs = 0UL;
    if (ledMode == "On") {
      digitalWrite(LED_RELAY_PIN, HIGH);
      ledManualActive = true;
      ledStart        = m;
      Serial.println("[RESET] Manual ON (ledStart reset) @ crossed 6:00");
    }
    else {
      digitalWrite(LED_RELAY_PIN, LOW);
      ledManualActive = false;
      ledAutoActive   = false;
      ledWasOn        = false;
      lastLedDur      = 0;
      Serial.println("[RESET] LED OFF @ crossed 6:00");
    }

    // 3.3 รีเซ็ต Energy (PZEM)
    if (pzem.resetEnergy()) {
      baselineEnergy = 0.0f;
      Serial.println("[RESET] Energy reset OK @ crossed 6:00");
    }
    else {
      Serial.println("[RESET] Energy reset FAIL @ crossed 6:00");
    }

    lastUploadTime = 0;

    strftime(sheetDate, sizeof(sheetDate), "%Y-%m-%d", tp);
    Serial.printf("[RESET] New sheetDate = %s @ crossed 6:00\n", sheetDate);

    fetchConfig();
    Serial.println("[RESET] Completed Test Reset @ crossed 6:00");
  }

  // 4. รีเซ็ต triggered array เมื่อข้ามวันใหม่ (เช็ก weekday)
  if (wday != lastDayOfWeek) {
    lastDayOfWeek = wday;
    for (int i = 0; i < 7; i++) triggered[i] = false;
  }

  // 5. ระบบรดน้ำ
  if (!pumpOn
      && water[wday].en
      && !triggered[wday]
      && (prevNowS < water[wday].startSec)
      && (nowS    >= water[wday].startSec))
  {
    digitalWrite(PUMP_PIN, HIGH);
    pumpOn         = true;
    pumpStart      = m;
    triggered[wday]= true;
    Serial.println("[WATER] Pump ON");
  }
  if (pumpOn && (m - pumpStart >= water[wday].durMs)) {
    digitalWrite(PUMP_PIN, LOW);
    pumpOn = false;
    Serial.println("[WATER] Pump OFF");
  }

  // 6. อ่าน PPFD, คำนวณ DLI และควบคุม LED ทุก READ_INTERVAL
  if (m - lastReadTime >= READ_INTERVAL*1000UL) {
    lastReadTime = m;
    uint8_t r = node.readInputRegisters(0x0000, 6);
    if (r == node.ku8MBSuccess) {
      float par = node.getResponseBuffer(0);
      ppfd[idx] = par;
      if (++idx >= MAX_READINGS) idx = 0;
      if (countReads < MAX_READINGS) countReads++;

      // 6.1 คำนวณ DLI (mol/m²/day)
      dliAcc       += (par * READ_INTERVAL) / 1e6f;
      remainingDLI  = max(0.0f, dynamicDLI - dliAcc);

      // 6.2 ควบคุม LED ตามโหมด
      if (ledMode == "On") {
        if (!ledManualActive) {
          digitalWrite(LED_RELAY_PIN, HIGH);
          ledManualActive = true;
          ledStart        = m;
          Serial.println("[LED][MANUAL] Turned ON");
        }
      }
      else if (ledMode == "Off") {
        if (ledAutoActive) {
          lastLedDur   = m - ledStart;
          accumLedMs  += lastLedDur;
          ledWasOn     = true;
          ledAutoActive = false;
        }
        else if (ledManualActive) {
          accumLedMs += (m - ledStart);
          ledManualActive = false;
        }
        digitalWrite(LED_RELAY_PIN, LOW);
        Serial.println("[LED][MANUAL] Turned OFF");
      }
      else { 
        applyAutoLED(m);
        if (ledWasOn && !ledAutoActive) {
          accumLedMs += lastLedDur;
          ledWasOn   = false;
        }
      }

      if ((localMin % 5 == 0) && (localSec < 30) && (m - lastUploadTime >= UPLOAD_INTERVAL*1000UL)) {
        lastUploadTime = m;

        // 7.1 ค่าเฉลี่ย PPFD ของรอบที่ผ่านมา
        float sum = 0;
        for (int i = 0; i < countReads; i++) sum += ppfd[i];
        float avg = (countReads ? sum/countReads : 0);
        countReads = 0;

        // 7.2 คำนวณ LED On Time (Auto + Manual)
        unsigned long currentLedMs;
        if (ledAutoActive || ledManualActive) {
          currentLedMs = accumLedMs + (m - ledStart);
        }
        else {
          currentLedMs = accumLedMs;
        }
        int ledH = currentLedMs / 3600000UL;
        int ledM = (currentLedMs / 60000UL) % 60;
        int ledS = (currentLedMs / 1000UL) % 60;
        char ledTimeStr[9];
        sprintf(ledTimeStr, "%02d:%02d:%02d", ledH, ledM, ledS);

        // 7.3 คำนวณ Hours left until next 06:00 สำหรับ UI
        int currentSecFull = localHour*3600 + localMin*60 + localSec;
        int targetSec;
        if (localHour > 6 || (localHour == 6 && (localMin > 0 || localSec > 0))) {
          targetSec = (6 + 24)*3600;
        } else {
          targetSec = 6*3600;
        }
        float hrsLeft = (targetSec - currentSecFull) / 3600.0f;
        int hhl = int(hrsLeft), hml = int((hrsLeft - hhl)*60);
        char hlStr[6];  sprintf(hlStr, "%d:%02d", hhl, hml);

        // 7.4 คำนวณ Energy หัก baselineEnergy
        float energyNow      = pzem.energy();
        float energyToUpload = energyNow - baselineEnergy;
        if (energyToUpload < 0) energyToUpload = 0;

        // 7.5 สร้าง URL สำหรับอัปโหลด (ใช้ sheetDate ล่าสุด)
        char dateBuf[11];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tp);
        String url = String(webAppUrl) +
          "?sheetName="     + String(sheetDate) +
          "&ppfd="          + String(avg,2) +
          "&dli="           + String(dliAcc,3) +
          "&remaining_dli=" + String(remainingDLI,3) +
          "&hours_left="    + String(hlStr) +
          "&ledontime="     + String(ledTimeStr) +
          "&energy_kwh="    + String(energyToUpload,3) +
          "&power="         + String(pzem.power(),3) +
          "&voltage="       + String(pzem.voltage(),3) +
          "&current="       + String(pzem.current(),3) +
          "&date="          + String(dateBuf);
        Serial.println("[UPLOAD] " + url);

        HTTPClient up;
        if (up.begin(client, url)) {
          int code2 = up.GET();
          Serial.printf("[UPLOAD] HTTP %d\n", code2);
          up.end();
        } else {
          Serial.println("[UPLOAD] Upload begin failed");
        }

        Serial.printf("[DLI] Accumulated=%.5f\n", dliAcc);
      }
    }
    else {
      Serial.printf("[MODBUS] Read error: %02X\n", r);
    }
  }

  prevNowS = nowS;
}
