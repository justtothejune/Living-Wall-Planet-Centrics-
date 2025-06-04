# Living-Wall-Planet-Centrics-
ใน Git นี้จะมีส่วนของ Coding และ ไฟฟ้าของระบบ 
Coding :
  1. App Script ไว้ใช้ในการจัดการหน้า UI และอัพเดทค่าใน Google Sheet
  2. โค้ดของ Arduino IDE จัดการการทำงานต่างๆใน ESP32 ของระบบ

## System Purpose
  - อ่านค่าความเข้มแสง PPFD และสามารถคำนวณออกมาเป็น DLI ที่ต้องการ (โดยมีการกำหนด dli_target ให้เหมาะสมกับพืชชนิดที่ต้องการ)
  - การควบคุมไป LED ในโหมด Manual (On/Off) และ Automatic
  - เก็บข้อมูลการใช้พลังงานของไฟ LED ในช่วงที่เปิดไฟในหน่วย kWh ผ่านมิเตอร์ PZEM-004T
  - ระบบรดน่้ำต้นไม้ที่สามารถสั่งการผ่านทาง UI ในหน้า Googlesheet สามารถตั้งค่าไว้ล่วงหน้าเพื่อการรดน้ำในรอบถัดไปได้
  - การอัปโหลดค่าทุกๆ 5 นาทีในการเก็บข้อมูลขึ้นใน Googlesheet
  - รีเซ็ตข้อมูลรายวันเวลา 06:00 ตอนเช้าของทุกวัน (สร้างแท็บใหม่และเคลียร์ข้อมูลวันก่อนหน้านั้น)
## ESP32 Part
### Pins Setting
1. ESP32 Pins
UART1 (Modbus RTU):
- TXD2 → GPIO 17, RXD2 → GPIO 16
- DE/RE (ไดร์เวอร์ RS-485) → GPIO 5
UART2 (PZEM-004T):
- RX → GPIO 21, TX → GPIO 22
  
  รีเลย์ควบคุม LED: GPIO 4

  รีเลย์ควบคุมปั๊มน้ำ: GPIO 15

2. เซ็นเซอร์ PAR (Modbus RTU)
- เชื่อม RS-485 A/B เข้ากับเซ็นเซอร์
- ใช้ GPIO 5 ควบคุมทิศทางการส่งข้อมูล
3. มิเตอร์ไฟฟ้า PZEM-004T
- สื่อสารผ่าน UART2 เพื่ออ่านค่าแรงดัน, กระแส, กำลัง, kWh
4.รีเลย์ (Relay Modules)
- GPIO 4 (LED_RELAY_PIN): เปิด–ปิดหลอดไฟ LED
- GPIO 15 (PUMP_PIN): เปิด–ปิดปั๊มน้ำ
```python
#define TXD2           17
#define RXD2           16
#define DE_RE_PIN      5
#define LED_RELAY_PIN  4
#define PUMP_PIN       15
#define PZEM_RX_PIN    21
#define PZEM_TX_PIN    22
```

## Coding Part
1. การเชื่อมต่อ Wi-Fi และ NTP

- เชื่อมต่อ Wi-Fi ด้วย SSID/รหัสผ่าน แล้วตั้งอินสแตนซ์ WiFiClientSecure

- ซิงก์เวลาผ่าน NTP (โซนเวลา UTC+7)
```python
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

// ใน setup():
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

```
- ถ้าจะปรับเปลี่ยน Wifi และ Googlesheet ใหม่ ต้องไปแก้ที่ส่วนนี้ก่อน
```python
const char* ssid     = "PlanetCentric";
const char* wifiPass = "BearLab!";
const char* webAppUrl = "https://script.google.com/macros/s/…/exec";
```
2. การดึงค่า Configuration

- ทุก 10 วินาที ทำ HTTP GET ไปยัง Google Apps Script เพื่อรับ JSON ที่ประกอบด้วย:

    1. dli_target (mol·m⁻²·day⁻¹) สามารถตั้งค่าได้ตามที่กำหนดผ่าน UI ใน Googlesheet แล้วค่านั้นจะถูกดึงมาใช้ต่อไป

    2. led โหมด (“On”/“Off”/“Automatic”) ควบคุมผ่าน UI ใน Googlesheet 

    3. ตัวเลือก "watering" (ตารางรดน้ำรายสัปดาห์: วัน, เวลา, ระยะเวลา)

- ใช้ ArduinoJson ในการแยกวิเคราะห์ JSON
```python
void fetchConfig() {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(client, webAppUrl)) return;
  int code = http.GET();
  String payload = http.getString();
  http.end();
  if (code != HTTP_CODE_OK) return;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return;

  // 1. ปรับค่า DLI target
  float fD = doc["dli_target"] | sheetDliTarget;
  if (fabs(fD - sheetDliTarget) > 0.001f) {
    sheetDliTarget = fD;
    dynamicDLI     = fD;
    dliAcc         = 0.0f;
    remainingDLI   = fD;
    Serial.printf("[CONFIG] DLI target -> %.2f (reset)\n", dynamicDLI);
  }

  // 2. ปรับโหมด LED
  String oldMode = ledMode;
  ledMode = doc["led"] | ledMode;
  if (oldMode != ledMode) {
    // ... (logic ตาม section 3.4)
  }

  // 3. ปรับตารางการรดน้ำ
  for (int i = 0; i < 7; i++) {
    water[i].en  = false;
    triggered[i] = false;
  }
  if (doc.containsKey("watering")) {
    for (JsonPair kv : doc["watering"].as<JsonObject>()) {
      String day = String(kv.key().c_str()); day.trim();
      JsonObject e = kv.value().as<JsonObject>();
      const char* ts = e["time"];
      int dur = e["duration"];
      int hh = 0, mm = 0;
      if (sscanf(ts, "%d:%d", &hh, &mm) != 2) continue;
      for (int d = 0; d < 7; d++) {
        if (day.equalsIgnoreCase(WEEKDAYS[d])) {
          water[d].en       = true;
          water[d].startSec = hh*3600 + mm*60;
          water[d].durMs    = dur * 1000UL;
          break;
        }
      }
    }
  }
}
// ใน setup(): fetchConfig();
```
3. การอ่าน PPFD และคำนวณ DLI
- ทุก 5 วินาที (READ_INTERVAL):
  1. อ่านค่า PPFD จากเซ็นเซอร์ผ่าน Modbus 
  2. เก็บลงบัฟเฟอร์ (ARRAY ขนาด 60 ค่า)
  3. คำนวณ DLI เพิ่มเติมด้วยสมการ:
```python
dliAcc += (PPFD × READ_INTERVAL) / 1e6
remainingDLI = dynamicDLI − dliAcc
```
- ค่า remainingDLI คือจำนวน DLI ที่เหลือเพื่อให้ถึงเป้าหมาย
``` python
if (m - lastReadTime >= READ_INTERVAL*1000UL) {
  lastReadTime = m;
  uint8_t r = node.readInputRegisters(0x0000, 6);
  if (r == node.ku8MBSuccess) {
    float par = node.getResponseBuffer(0);
    ppfd[idx] = par;
    if (++idx >= MAX_READINGS) idx = 0;
    if (countReads < MAX_READINGS) countReads++;

    // คำนวณ DLI
    dliAcc += (par * READ_INTERVAL) / 1e6f;
    remainingDLI = max(0.0f, dynamicDLI - dliAcc);
  }
}
```
4. การควบคุม LED
- โหมด “On” (Manual เปิด)
  - เปิดรีเลย์ LED ทันที (GPIO 4 = HIGH)
  - ตั้งแฟล็ก ledManualActive = true และบันทึกเวลา ledStart = millis()
  - ไม่ล้างตัวสะสมเวลาที่ไฟติด (accumLedMs) 
```python
if (ledMode == "On") {
  if (!ledManualActive) {
    digitalWrite(LED_RELAY_PIN, HIGH);
    ledManualActive = true;
    ledStart = m;  // ไม่ล้าง accumLedMs
    Serial.println("[LED][MANUAL] Turned ON");
  }
}
```
- โหมด “Off” (Manual ปิด)
```pyhon
else if (ledMode == "Off") {
  if (ledAutoActive) {
    lastLedDur = m - ledStart;
    accumLedMs += lastLedDur;
    ledWasOn = true;
    ledAutoActive = false;
  }
  else if (ledManualActive) {
    accumLedMs += (m - ledStart);
    ledManualActive = false;
  }
  digitalWrite(LED_RELAY_PIN, LOW);
  Serial.println("[LED][MANUAL] Turned OFF");
}
```
- โหมด “Automatic”
  - สามารถปรับเลขตัวหาร "/0.36f" เป็นค่าอื่นได้อิงจากค่า PPFD ที่ได้จากหลอดไฟ LED เป็นเท่าไหร่ เทียบเป็นค่า PPFD เท่านี้ใน 1 ชั่วโมง จะได้ DLI เท่าไหร่นำมาเป็นตัวหาร
```python
float need    = (dynamicDLI - dliAcc) / 0.36f;  // ปรับตามสเปกตรัม LED
```
  - คำนวณเวลาที่เหลือจนถึง 06:00 น.: หากเวลาปัจจุบัน > 06:00 → ตั้งเป้าเป็น 06:00 ของวันรุ่นไป (เพิ่ม 24 ชั่วโมง)
```python
else { // Automatic
  applyAutoLED(m);
  if (ledWasOn && !ledAutoActive) {
    accumLedMs += lastLedDur;
    ledWasOn = false;
  }
}

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
  float need    = (dynamicDLI - dliAcc) / 0.36f;  // ปรับตามสเปกตรัม LED

  Serial.printf("[LED][AUTO] dliAcc=%.3f, dynamicDLI=%.3f, hrsLeft=%.3f, need=%.3f\n",
                dliAcc, dynamicDLI, hrsLeft, need);

  if (!ledAutoActive && need > hrsLeft) {
    digitalWrite(LED_RELAY_PIN, HIGH);
    ledAutoActive = true;
    ledStart = m;
    Serial.println("[LED][AUTO] LED ON (auto)");
    return;
  }
  if (ledAutoActive && (dliAcc >= dynamicDLI || need <= hrsLeft)) {
    digitalWrite(LED_RELAY_PIN, LOW);
    ledAutoActive = false;
    lastLedDur = m - ledStart;
    ledWasOn = true;
    Serial.println("[LED][AUTO] LED OFF (auto)");
  }
}
```
5. ตารางการรดน้ำ (Water Schedule)
```python
struct WaterSchedule {
  bool en;
  uint32_t startSec;
  uint32_t durMs;
};
WaterSchedule water[7];
bool triggered[7];
int lastDayOfWeek = -1;
bool pumpOn = false;
unsigned long pumpStart = 0;
int prevNowS = -1;

// ใน loop():
if (wday != lastDayOfWeek) {
  lastDayOfWeek = wday;
  for (int i = 0; i < 7; i++) triggered[i] = false;
}

if (!pumpOn
    && water[wday].en
    && !triggered[wday]
    && prevNowS < water[wday].startSec
    && nowS >= water[wday].startSec) {
  digitalWrite(PUMP_PIN, HIGH);
  pumpOn = true;
  pumpStart = millis();
  triggered[wday] = true;
  Serial.println("[WATER] Pump ON");
}
if (pumpOn && (millis() - pumpStart >= water[wday].durMs)) {
  digitalWrite(PUMP_PIN, LOW);
  pumpOn = false;
  Serial.println("[WATER] Pump OFF");
}
```
- watering ถูกอัปเดตใน fetchConfig() ตาม JSON
- เมื่อข้ามวันใหม่ ต้องเซ็ต triggered[] = false
- ตรวจเงื่อนไขเวลาก่อน–หลังเพื่อสั่งเปิด/ปิดปั๊ม
6. การวัดพลังงาน (Energy Metering)
```python
// อ่านค่า Energy (kWh), Power (W), Voltage (V), Current (A)
float energyNow = pzem.energy();
float powerNow  = pzem.power();
float voltNow   = pzem.voltage();
float currNow   = pzem.current();

// หัก Baseline (รีเซ็ตเป็น 0 ที่ 06:00)
float energyToUpload = energyNow - baselineEnergy;
if (energyToUpload < 0) energyToUpload = 0;
```
- ทุกอัปโหลด (5 นาที) อ่าน pzem.energy() เพื่อให้ได้ kWh ปัจจุบัน
- หักลบ baselineEnergy (ซึ่งรีเซ็ตเป็น 0 ที่เวลา 06:00) เพื่ออัปโหลดเฉพาะพลังงานของวันนั้น ๆ
- อ่านพารามิเตอร์เพิ่มเติม pzem.power(), pzem.voltage(), pzem.current() (ไม่ต้องแปลงสมการอะไรเลยสามารถใช้คำสั่งปกติอ่านค่าได้เลย)

7. การรีเซ็ตข้อมูลรายวัน (Daily Reset)
- ตรวจ Edge Detection ข้าม 06:00:00 และ today != lastResetDay (สามารถปรับเวลาได้ตามที่ต้องการแต่ในระบบนี้เป็นรีเซ็ทตอน 6 โมงเช้า)
- ตั้ง lastResetDay = today เพื่อป้องกันรีเซ็ตซ้ำในวันเดียวกัน
- สร้างแท็บใหม่สำหรับ Google Sheets (sheetDate = "YYYY-MM-DD")
- รีเซ็ตตัวสะสมทุกตัว และดึง config ใหม่ทันที
```python
if ((prevNowS < 6*3600) && (nowS >= 6*3600) && (today != lastResetDay)) {
  lastResetDay = today;

  // 3.1 รีเซ็ต DLI
  dliAcc = 0.0f;
  remainingDLI = dynamicDLI;

  // 3.2 รีเซ็ต LED On Time
  accumLedMs = 0UL;
  if (ledMode == "On") {
    digitalWrite(LED_RELAY_PIN, HIGH);
    ledManualActive = true;
    ledStart = millis();
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

  // 3.4 รีเซ็ต lastUploadTime เพื่อรอขอบนาทีหาร 5
  lastUploadTime = 0;

  // 3.5 อัปเดต sheetDate (แท็บใหม่)
  strftime(sheetDate, sizeof(sheetDate), "%Y-%m-%d", tp);
  Serial.printf("[RESET] New sheetDate = %s @ crossed 6:00\n", sheetDate);

  // 3.6 ดึง config ใหม่
  fetchConfig();
  Serial.println("[RESET] Completed Real Reset @ crossed 6:00");
}
```
8. การอัปโหลดข้อมูล (Upload Data)
- จะมีการอัพโหลดขึ้น Googlesheet ทุกๆ 5 นาทีของการเก็บข้อมูล ถ้าจะเปลี่ยนเวลาการอัพโหลดสามารถปรับเปลี่ยน Condition If ในบรรทัดแรกได้
```python
if ((localMin % 5 == 0) && (localSec < 30) && 
    (millis() - lastUploadTime >= UPLOAD_INTERVAL*1000UL)) {
  lastUploadTime = millis();
  // ... คำนวณ avg, ledTime, energyToUpload, hrsLeft ...
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
    "&power="         + String(powerNow,3) +
    "&voltage="       + String(voltNow,3) +
    "&current="       + String(currNow,3) +
    "&date="          + String(dateBuf);
  HTTPClient up;
  if (up.begin(client, url)) {
    int code2 = up.GET();
    Serial.printf("[UPLOAD] HTTP %d\n", code2);
    up.end();
  }
  Serial.printf("[DLI] Accumulated=%.5f\n", dliAcc);
}
```
