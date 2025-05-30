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
const int READ_INTERVAL       = 5;            // seconds
const int UPLOAD_INTERVAL     = 300;          // seconds
const unsigned long CONFIG_INTERVAL = 10000UL; // ms between config fetches

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
  "https://script.google.com/macros/s/"
  "AKfycbz7yTwAljVhchIorHOSY_nlYFPVmpvG2-JQ9HGMNmcxKcVX_tPg7OlRoqe46C1eLB5K/exec";

//https://script.google.com/macros/s/AKfycbwdHw7kho_cd1D7HD-5oXj-tvMKa9V9uN-Mf8uFzDozr0dX6_bZGVLXBpaiygXI6dypdg/exec
// ==== STATE ====
unsigned long lastReadTime    = 0;
unsigned long lastUploadTime  = 0;
unsigned long lastConfigFetch = 0;

// DLI tracking
float sheetDliTarget = 6.0;
float dynamicDLI      = 6.0;
float dliAcc         = 0.0;
float remainingDLI   = 0.0;

// PAR buffer
#define MAX_READINGS 60
float ppfd[MAX_READINGS];
int   idx = 0, countReads = 0;

// LED control
String  ledMode       = "Automatic";
bool    ledAutoActive = false;
bool    ledWasOn      = false;
unsigned long ledStart = 0, lastLedDur = 0;

// Water schedule
struct WaterSchedule { bool en; uint32_t startSec; uint32_t durMs; };
WaterSchedule water[7];
bool triggered[7];
int  lastDayOfWeek = -1;
bool pumpOn       = false;
unsigned long pumpStart = 0;
int  prevNowS     = -1;

// Daily reset tracker (day of year)
int lastResetDay  = -1;

// Today's sheet name
char sheetDate[11];
const char* WEEKDAYS[7] = {
  "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
};

// RS-485 helpers
void preTx()  { digitalWrite(DE_RE_PIN, HIGH); }
void postTx() { digitalWrite(DE_RE_PIN, LOW);  }

// Standard Wi-Fi connect
void connectWiFi() {
  Serial.printf("Connecting to Wi-Fi \"%s\"", ssid);
  WiFi.begin(ssid, wifiPass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println("\nWi-Fi connected, IP=" + WiFi.localIP().toString());
  client.setInsecure();
}

// Automatic LED logic based on DLI
void applyAutoLED(unsigned long m) {
  time_t tt = time(nullptr);
  struct tm* t = localtime(&tt);
  // new: hours until next 06:00
  int currentMin = t->tm_hour * 60 + t->tm_min;
  int targetMin  = (t->tm_hour >= 6 ? (24 + 6) * 60 : 6 * 60);
  float hrsLeft  = (targetMin - currentMin) / 60.0f;
  float need    = (dynamicDLI - dliAcc) / 0.47f;
  if (!ledAutoActive && need > hrsLeft) {
    digitalWrite(LED_RELAY_PIN, HIGH);
    ledAutoActive = true;
    ledStart      = m;
    Serial.println("LED ON (auto)");
  }
  if (ledAutoActive && dliAcc >= dynamicDLI) {
    digitalWrite(LED_RELAY_PIN, LOW);
    ledAutoActive = false;
    lastLedDur    = m - ledStart;
    ledWasOn      = true;
    Serial.println("LED OFF (auto)");
  }
}

// Fetch JSON config & parse
void fetchConfig() {
  Serial.println("\n>>> fetchConfig()");
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, webAppUrl)) {
    Serial.println("HTTP begin failed");
    return;
  }
  int code = http.GET();
  String pl = http.getString();
  http.end();

  Serial.printf("GET %s → %d\n", webAppUrl, code);
  Serial.println("JSON:\n" + pl);
  if (code != HTTP_CODE_OK) return;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, pl)) {
    Serial.println("JSON parse error");
    return;
  }

  // Control DLI target 
  float fD = doc["dli_target"] | sheetDliTarget;
  if (fabs(fD - sheetDliTarget) > 0.001f) {
    sheetDliTarget = fD;
    dynamicDLI     = fD;
    dliAcc         = 0.0f;
    Serial.printf("DLI target -> %.2f (reset)\n", dynamicDLI);
  }

  // Control LED mode
  String oldMode = ledMode;
  ledMode = doc["led"] | ledMode;
  if (oldMode != ledMode) {
    Serial.printf("LED %s -> %s\n", oldMode.c_str(), ledMode.c_str());
    if (ledMode == "Automatic") {
      ledAutoActive = false;
      applyAutoLED(millis());
    }
  }
  Serial.printf("DLI: %.2f  LED: %s\n",
                (float)doc["dli_target"],
                doc["led"].as<const char*>());

  // Water Control Schedule
  Serial.println("Watering schedule:");
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
      int hh  = (ts[0]-'0')*10 + (ts[1]-'0');
      int mm  = (ts[3]-'0')*10 + (ts[4]-'0');
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

  // NTP sync for Bangkok (UTC+7)
  configTime(7*3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 7LL*3600) {
    delay(500);
    Serial.print('.');
    now = time(nullptr);
  }
  Serial.println("\nTime synced.");

  struct tm* t = localtime(&now);
  if (t->tm_hour < 6) {
    now -= 86400;
    t = localtime(&now);
  }
  strftime(sheetDate, sizeof(sheetDate), "%Y-%m-%d", t);

  prevNowS     = t->tm_hour*3600 + t->tm_min*60 + t->tm_sec;
  lastDayOfWeek= t->tm_wday;
  lastResetDay = t->tm_yday;

  fetchConfig();
}

void loop() {
  unsigned long m = millis();

  // Fetch config every CONFIG_INTERVAL
  if (m - lastConfigFetch >= CONFIG_INTERVAL) {
    lastConfigFetch = m;
    fetchConfig();
  }

  // Get local time
  time_t tt = time(nullptr);
  struct tm* tp = localtime(&tt);
  int wday      = tp->tm_wday;
  int localHour = tp->tm_hour;
  int localMin  = tp->tm_min;
  int localSec  = tp->tm_sec;
  int today     = tp->tm_yday;
  int nowS      = localHour*3600 + localMin*60 + localSec;

  // Daily reset at 06:00 once per day สามารถปรับแก้วันและเวลาในการ Reset ระบบได้
  if (localHour == 6 && localMin == 0 && localSec == 0 && lastResetDay != today) {
    lastResetDay = today;
    dliAcc         = 0.0f;
    remainingDLI   = dynamicDLI;
    ledAutoActive  = false;
    ledWasOn       = false;
    lastLedDur     = 0;
    digitalWrite(LED_RELAY_PIN, LOW);

    if (pzem.resetEnergy()) Serial.println("Energy reset OK");
    else                     Serial.println("Energy reset FAIL");

    strftime(sheetDate, sizeof(sheetDate), "%Y-%m-%d", tp);
    fetchConfig();
    Serial.println("⏰ Daily reset @ 06:00");
  }

  // Reset triggers at midnight (new weekday)
  if (wday != lastDayOfWeek) {
    lastDayOfWeek = wday;
    for (int i = 0; i < 7; i++) triggered[i] = false;
  }

  // Edge-detect pump ON: only when crossing startSec
  if (!pumpOn
      && water[wday].en
      && !triggered[wday]
      && prevNowS < water[wday].startSec
      && nowS    >= water[wday].startSec)
  {
    digitalWrite(PUMP_PIN, HIGH);
    pumpOn         = true;
    pumpStart      = m;
    triggered[wday]= true;
    Serial.println("Pump ON");
  }
  // Turn OFF after duration
  if (pumpOn && (m - pumpStart >= water[wday].durMs)) {
    digitalWrite(PUMP_PIN, LOW);
    pumpOn = false;
    Serial.println("Pump OFF");
  }

  prevNowS = nowS;

  // PPFD read, DLI, LED, and upload
  if (m - lastReadTime >= READ_INTERVAL*1000UL) {
    lastReadTime = m;
    uint8_t r = node.readInputRegisters(0x0000, 6);
    if (r == node.ku8MBSuccess) {
      float par = node.getResponseBuffer(0);
      ppfd[idx] = par;
      if (++idx >= MAX_READINGS) idx = 0;
      if (countReads < MAX_READINGS) countReads++;

      dliAcc += (par * READ_INTERVAL) / 1e6f;
      remainingDLI = max(0.0f, dynamicDLI - dliAcc);

      if (ledMode == "On")      digitalWrite(LED_RELAY_PIN, HIGH);
      else if (ledMode == "Off") digitalWrite(LED_RELAY_PIN, LOW);
      else applyAutoLED(m);

      // Upload every UPLOAD_INTERVAL, aligned to 5-min boundaries
      if ((m - lastUploadTime >= UPLOAD_INTERVAL*1000UL) && (localMin % 5 == 0)) {
        lastUploadTime = m;

        float sum = 0;
        for (int i = 0; i < countReads; i++) sum += ppfd[i];
        float avg = (countReads ? sum/countReads : 0);
        countReads = 0;

        unsigned long ledMs = ledAutoActive ? (m - ledStart)
                                           : (ledWasOn ? lastLedDur : 0);
        int lh = ledMs/3600000UL,
            lm = (ledMs/60000UL)%60,
            ls = (ledMs/1000UL)%60;
        char ledT[9]; sprintf(ledT, "%02d:%02d:%02d", lh, lm, ls);

        // Hours left until next 06:00
        int nowMins    = localHour*60 + localMin;
        int targetMins = (localHour >= 6 ? (24+6)*60 : 6*60);
        float hrsLeft  = (targetMins - nowMins) / 60.0f;
        int hhl = int(hrsLeft), hml = int((hrsLeft - hhl)*60);
        char hlStr[6]; sprintf(hlStr, "%d:%02d", hhl, hml);

        char dateBuf[11];
        strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tp);

        String url = String(webAppUrl) +
          "?sheetName="    + String(sheetDate) +
          "&ppfd="         + String(avg,2) +
          "&dli="          + String(dliAcc,3) +
          "&remaining_dli="+ String(remainingDLI,3) +
          "&hours_left="   + String(hlStr) +
          "&ledontime="    + String(ledT) +
          "&energy_kwh="   + String(pzem.energy(),3) +
          "&power="        + String(pzem.power(),3) +
          "&voltage="      + String(pzem.voltage(),3) +
          "&current="      + String(pzem.current(),3) +
          "&date="         + String(dateBuf);
        Serial.println("Upload: " + url);

        HTTPClient up;
        if (up.begin(client, url)) {
          int code2 = up.GET();
          Serial.printf("UPLOAD HTTP %d\n", code2);
          up.end();
        } else {
          Serial.println("Upload begin failed");
        }

        Serial.printf("DLI now: %.5f\n", dliAcc);
      }
    }
  }
}
