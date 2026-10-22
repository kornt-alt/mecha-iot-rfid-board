#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

// เวลาไทย UTC+7 (POSIX TZ กลับเครื่องหมาย) ดึงจาก NTP หลังต่อ Wi-Fi ได้
#define TZ_BANGKOK "<+07>-7"

bool timeValid() {
  return time(nullptr) > 1700000000;  // ก่อน sync นาฬิกาจะเริ่มที่ 1970
}

// fmt แบบ strftime เช่น "%d/%m/%Y %H:%M:%S"
String nowText(const char* fmt) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];  // ISO "%Y-%m-%dT%H:%M:%S+07:00" ยาว 25 ตัว
  strftime(buf, sizeof(buf), fmt, &t);
  return String(buf);
}

const char* wifiStatusText(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED:       return "connected";
    case WL_NO_SSID_AVAIL:   return "SSID not found";
    case WL_CONNECT_FAILED:  return "connect failed";
    case WL_WRONG_PASSWORD:  return "wrong password";
    case WL_DISCONNECTED:    return "disconnected";
    case WL_IDLE_STATUS:     return "idle";
    default:                 return "other";
  }
}

// log ทุกครั้งที่สถานะ Wi-Fi เปลี่ยน — ถ้าต่อไม่ได้ใน 20 วินาที สแกนหา SSID ครั้งเดียวเพื่อดูว่าเห็นไหม (ESP8266 เห็นแค่ 2.4GHz)
void logWifi() {
  static wl_status_t last = (wl_status_t)255;
  static bool scanned = false;
  wl_status_t s = WiFi.status();
  if (s != last) {
    last = s;
    if (s == WL_CONNECTED) {
      Serial.printf("WiFi: connected IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      Serial.printf("WiFi: %s (%d)\n", wifiStatusText(s), s);
    }
  }
  static bool timeLogged = false;
  if (!timeLogged && timeValid()) {
    timeLogged = true;
    Serial.printf("Time synced: %s\n", nowText("%d/%m/%Y %H:%M:%S").c_str());
  }
  if (s != WL_CONNECTED && !scanned && millis() > 20000) {
    scanned = true;
    int n = WiFi.scanNetworks();
    Serial.printf("WiFi scan: %d networks\n", n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  %s%s ch=%d rssi=%d enc=%d\n", WiFi.SSID(i).c_str(),
                    WiFi.SSID(i) == WIFI_SSID ? "  <-- target" : "", WiFi.channel(i), WiFi.RSSI(i),
                    WiFi.encryptionType(i));
    }
  }
}

// RC522 ใช้ SPI ฮาร์ดแวร์: SCK=D5, MISO=D6, MOSI=D7 + SS=D0, RST=D3
// SS ย้ายจาก D8 มา D0 เพราะ D8 ต่อกับ PIEZO บนบอร์ด AX-WiFi
#define RFID_SS_PIN  D0
#define RFID_RST_PIN D3

// PIEZO บน AX-WiFi ต่อที่ D8/GPIO15 (ขา boot ต้องเป็น LOW ตอนรีเซ็ต — ห้ามดึงขึ้น HIGH ค้าง)
#define BUZZER_PIN D8

// ปุ่มสลับโหมด IN/OUT ที่ D4/GPIO2 (ปุ่มกดเด้งกลับ ต่อลง GND ตอนกด)
// GPIO2 เป็นขา boot ต้อง HIGH ตอนรีเซ็ต — ห้ามใช้สวิตช์โยกที่ค้างลง GND และห้ามใช้ LED_BUILTIN (ขาเดียวกัน)
#define MODE_BUTTON_PIN D4
const unsigned long BUTTON_DEBOUNCE_MS = 50;

enum Mode { MODE_IN, MODE_OUT };
Mode mode = MODE_IN;

// WS2812 2 ดวง (SLED#1, SLED#2) บน AX-WiFi ต่อที่ GPIO10 (SD3) — ใช้ได้เพราะแฟลชเป็นโหมด DIO
#define SLED_PIN   10
#define SLED_COUNT 2
Adafruit_NeoPixel sled(SLED_COUNT, SLED_PIN, NEO_GRB + NEO_KHZ800);

void sledFill(uint32_t color) {
  sled.fill(color);
  sled.show();
}

void sledBlink(uint32_t color, byte times) {
  for (byte i = 0; i < times; i++) {
    sledFill(color);
    delay(120);
    sledFill(0);
    if (i + 1 < times) delay(100);
  }
}

uint32_t modeColor(Mode m) {
  return m == MODE_IN ? sled.Color(0, 255, 0) : sled.Color(0, 80, 255);
}

// ไฟหรี่ค้างไว้บอกโหมดปัจจุบันตอนรอแตะบัตร
void sledIdle() {
  uint32_t c = modeColor(mode);
  sledFill(sled.Color(((c >> 16) & 0xFF) / 12, ((c >> 8) & 0xFF) / 12, (c & 0xFF) / 12));
}

void beepOk() {
  tone(BUZZER_PIN, 2700, 80);
  delay(110);
  tone(BUZZER_PIN, 3200, 80);
  delay(90);
}

void beepError() {
  tone(BUZZER_PIN, 400, 600);
  delay(650);
}

// OLED บน AX-WiFi: I2C SDA=D2, SCL=D1, address 0x3C (สแกนเจอจริง)
// ถ้าภาพเลื่อน/มีขยะขอบขวา แปลว่าเป็นชิป SH1106 ให้เปลี่ยนเป็น U8G2_SH1106_128X64_NONAME_F_HW_I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, D1, D2);

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

const unsigned long RESULT_SHOW_MS = 3000;

// บล็อกบัตรใบเดิมที่แตะซ้ำภายในเวลานี้ (จำได้ล่าสุด RECENT_SIZE ใบ กันกรณีแตะสลับ A-B-A)
const unsigned long DUPLICATE_BLOCK_MS = 10000;
const byte RECENT_SIZE = 10;
struct RecentScan { String uid; Mode mode; unsigned long at; };
RecentScan recent[RECENT_SIZE];
byte recentNext = 0;

// คืนเวลาที่เหลือ (ms) ถ้าบัตรนี้เพิ่งแตะในโหมดเดียวกัน, 0 = แตะได้ (แตะ IN แล้วแตะ OUT ทันทีได้)
unsigned long blockedFor(const String& uid) {
  for (byte i = 0; i < RECENT_SIZE; i++) {
    if (recent[i].uid != uid || recent[i].mode != mode) continue;
    unsigned long elapsed = millis() - recent[i].at;
    if (elapsed < DUPLICATE_BLOCK_MS) return DUPLICATE_BLOCK_MS - elapsed;
  }
  return 0;
}

void rememberScan(const String& uid) {
  for (byte i = 0; i < RECENT_SIZE; i++) {
    if (recent[i].uid == uid && recent[i].mode == mode) { recent[i].at = millis(); return; }
  }
  recent[recentNext] = {uid, mode, millis()};
  recentNext = (recentNext + 1) % RECENT_SIZE;
}

unsigned long lastScanAt = 0;
bool showingResult = false;
uint32_t countIn = 0;
uint32_t countOut = 0;

// ---- ส่งผลแตะบัตรไป MECHA-CHECKIN backend ผ่าน WebSocket ----
// protocol: D:\SCRIPT\WEB\MECHA_TIME_IN\docs\ESP_DEVICE_API.md (แก้ข้อความต้องแก้ backend/services/devices.js ด้วย)
#define FW_VERSION "1.1.0"
const bool wsEnabled = sizeof(SERVER_HOST) > 1;  // SERVER_HOST ว่าง = ปิด WS ทำงานออฟไลน์เหมือนเดิม

WebSocketsClient ws;
bool wsReady = false;   // true หลังได้ hello_ok
bool wsBadKey = false;  // key/ชื่อเครื่องผิด — ลองใหม่ทุก 60 วิ
String bootId;          // สุ่มใหม่ทุกบูต ใช้ประกอบ id ของ scan
uint32_t seq = 0;

// คิวรายการที่ยังไม่ได้ ack (อยู่ใน RAM ไฟดับ = หาย) เต็มแล้วทิ้งอันเก่าสุด
struct Pending { String id, card, ts; Mode mode; unsigned long sentAt; };  // sentAt 0 = ยังไม่ได้ส่ง
const byte QUEUE_SIZE = 20;
Pending pending[QUEUE_SIZE];
byte pendingCount = 0;
const unsigned long ACK_TIMEOUT_MS = 5000;  // ส่งแล้วไม่ได้ ack เกินนี้ = ส่งใหม่ (id เดิม server กันซ้ำให้)
const unsigned long SEND_GAP_MS = 150;      // server รับไม่เกิน 10 ข้อความ/วิ — ส่งของค้างทีละอัน

// ผลบนจอหลังแตะบัตร — อัปเดตเมื่อได้ ack / resolved
enum ResultStatus { RES_WAITING, RES_NAMED, RES_UNKNOWN, RES_ERROR, RES_OFFLINE };
const unsigned long ACK_WAIT_MS = 1500;  // ไม่ได้ ack ภายในนี้ = แสดง Saved (offline)
bool shownIsCard = false;  // false = จอแสดงผลแตะซ้ำอยู่
String shownCard, shownId, shownName;
ResultStatus shownStatus = RES_WAITING;
uint32_t shownCount = 0;

String enqueueScan(const String& uid) {
  if (pendingCount == QUEUE_SIZE) {
    Serial.printf("WS: queue full, drop %s\n", pending[0].id.c_str());
    for (byte i = 1; i < QUEUE_SIZE; i++) pending[i - 1] = pending[i];
    pendingCount--;
  }
  Pending& p = pending[pendingCount++];
  p.id = bootId + "-" + String(++seq);
  p.card = uid;
  p.mode = mode;
  p.ts = timeValid() ? nowText("%Y-%m-%dT%H:%M:%S+07:00") : String();  // ว่าง = ส่ง null ให้ server ใช้เวลาตัวเอง
  p.sentAt = 0;
  return p.id;
}

void removeFromQueue(const char* id) {
  for (byte i = 0; i < pendingCount; i++) {
    if (pending[i].id != id) continue;
    for (byte j = i + 1; j < pendingCount; j++) pending[j - 1] = pending[j];
    pendingCount--;
    return;
  }
}

void sendHello() {
  JsonDocument d;
  d["t"] = "hello";
  d["device"] = DEVICE_ID;
  d["key"] = DEVICE_KEY;
  d["fw"] = FW_VERSION;
  d["boot"] = bootId;
  String s;
  serializeJson(d, s);
  ws.sendTXT(s);
}

void sendScan(Pending& p) {
  JsonDocument d;
  d["t"] = "scan";
  d["id"] = p.id;
  d["card"] = p.card;  // string เสมอ — ส่งเป็นตัวเลข 0 นำหน้าจะหาย
  d["mode"] = p.mode == MODE_IN ? "in" : "out";
  if (p.ts.length()) d["ts"] = p.ts;
  else d["ts"] = nullptr;
  String s;
  serializeJson(d, s);
  ws.sendTXT(s);
  p.sentAt = millis() | 1;
  Serial.printf("WS -> %s\n", s.c_str());
}

// ส่งทีละรายการ: ที่ยังไม่ได้ส่ง หรือส่งแล้วรอ ack นานเกิน
void pumpQueue() {
  static unsigned long lastSendAt = 0;
  if (!wsReady || millis() - lastSendAt < SEND_GAP_MS) return;
  for (byte i = 0; i < pendingCount; i++) {
    Pending& p = pending[i];
    if (p.sentAt && millis() - p.sentAt < ACK_TIMEOUT_MS) continue;
    sendScan(p);
    lastSendAt = millis();
    return;
  }
}

// นาฬิกาสำรองจาก server_time (เวลาไทย "YYYY-MM-DDTHH:MM:SS") เผื่อโรงงานบล็อก NTP
void setClockFrom(const char* s) {
  struct tm t = {};
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  timeval tv = {mktime(&t), 0};  // mktime ใช้ TZ จาก configTime = ตีเป็นเวลาไทย
  settimeofday(&tv, nullptr);
  Serial.printf("Clock set from server: %s\n", s);
}

void showReady() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  String clock;
  if (timeValid()) clock = nowText("%d/%m/%Y %H:%M:%S");
  else clock = WiFi.status() == WL_CONNECTED ? "Syncing time..." : "Connecting WiFi...";
  oled.drawStr((128 - oled.getStrWidth(clock.c_str())) / 2, 10, clock.c_str());

  const char* title = mode == MODE_IN ? "CHECK-IN" : "CHECK-OUT";
  oled.setFont(u8g2_font_helvB12_tr);
  oled.drawStr((128 - oled.getStrWidth(title)) / 2, 30, title);
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(22, 46, "Tap your card...");
  char line[24];
  snprintf(line, sizeof(line), "IN:%lu OUT:%lu", (unsigned long)countIn, (unsigned long)countOut);
  oled.drawStr(0, 63, line);
  if (wsEnabled) {
    // สถานะการเชื่อมต่อ backend ให้ช่างดูออกว่าเครื่องหลุด (qN = รายการค้างส่ง)
    String st = wsBadKey ? "WS:KEY" : wsReady ? "WS:OK" : "WS:--";
    if (pendingCount) st += " q" + String(pendingCount);
    oled.drawStr(128 - oled.getStrWidth(st.c_str()), 63, st.c_str());
  }
  oled.sendBuffer();
}

// หน้าผลแตะบัตร: เลขบัตรตัวใหญ่ + บรรทัดล่างเป็นชื่อ/สถานะจาก server
void showCard() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 12, mode == MODE_IN ? "Checked IN" : "Checked OUT");
  if (timeValid()) {
    String hms = nowText("%H:%M:%S");
    oled.drawStr(128 - oled.getStrWidth(hms.c_str()), 12, hms.c_str());
  }
  oled.drawHLine(0, 15, 128);
  oled.setFont(u8g2_font_helvB12_tr);
  oled.drawStr((128 - oled.getStrWidth(shownCard.c_str())) / 2, 38, shownCard.c_str());
  oled.setFont(u8g2_font_6x12_tr);
  String line;
  if (!wsEnabled) line = "#" + String(shownCount) + "  OK";
  else switch (shownStatus) {
    case RES_WAITING: line = "#" + String(shownCount) + "  Sending..."; break;
    case RES_NAMED:   line = shownName; break;
    case RES_UNKNOWN: line = "Unknown card"; break;
    case RES_ERROR:   line = "Server error"; break;
    case RES_OFFLINE: line = "Saved (offline)"; break;
  }
  while (line.length() && oled.getStrWidth(line.c_str()) > 128) line.remove(line.length() - 1);
  oled.drawStr((128 - oled.getStrWidth(line.c_str())) / 2, 60, line.c_str());
  oled.sendBuffer();
}

// name เป็น ASCII เสมอ (ฟอนต์ _tr แสดงไทยไม่ได้) — ไม่มีชื่ออังกฤษใช้รหัสพนักงานแทน
void applyName(const char* empNo, const char* name) {
  shownName = *name ? name : empNo;
  shownStatus = shownName.length() ? RES_NAMED : RES_UNKNOWN;
  showCard();
  lastScanAt = millis();  // ได้ผลแล้วโชว์ต่ออีก RESULT_SHOW_MS
}

void handleWsText(uint8_t* payload, size_t len) {
  Serial.printf("WS <- %.*s\n", (int)len, (const char*)payload);
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  const char* t = d["t"] | "";

  if (!strcmp(t, "hello_ok")) {
    wsReady = true;
    wsBadKey = false;
    ws.setReconnectInterval(5000);
    for (byte i = 0; i < pendingCount; i++) pending[i].sentAt = 0;  // ส่งของค้างใหม่ทั้งหมดผ่าน pumpQueue
    if (!timeValid()) setClockFrom(d["server_time"] | "");
  } else if (!strcmp(t, "ack")) {
    // ok / dup / error → เอาออกจากคิวทั้งหมด (error ห้าม resend วนไม่จบ)
    const char* id = d["id"] | "";
    removeFromQueue(id);
    if (!showingResult || !shownIsCard || shownId != id) return;
    if (!strcmp(d["status"] | "", "error")) {
      shownStatus = RES_ERROR;  // message จาก server เป็นภาษาไทย แสดงบนจอไม่ได้
      showCard();
      lastScanAt = millis();
    } else {
      applyName(d["emp_no"] | "", d["name"] | "");
    }
  } else if (!strcmp(t, "resolved")) {
    // มีคนผูกบัตรที่เคยเป็น Unknown — อัปเดตเฉพาะถ้าจอยังแสดงบัตรนี้อยู่
    if (showingResult && shownIsCard && shownCard == (d["card"] | "")) applyName(d["emp_no"] | "", d["name"] | "");
  } else if (!strcmp(t, "error")) {
    // library ไม่บอก close code 4001 แต่ server ส่ง error นี้ก่อนปิดเสมอ — ไม่ต้อง reconnect ถี่
    const char* code = d["code"] | "";
    if (!strcmp(code, "bad_key") || !strcmp(code, "bad_device")) {
      wsBadKey = true;
      ws.setReconnectInterval(60000);
    }
  }
}

void onWs(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("WS: connected");
      wsReady = false;
      sendHello();  // ต้องส่งภายใน 5 วิ ไม่งั้น server ปิด (4002)
      break;
    case WStype_DISCONNECTED:
      Serial.println("WS: disconnected");
      wsReady = false;
      break;
    case WStype_TEXT:
      handleWsText(payload, len);
      break;
    default:
      break;
  }
}

// เลขบัตร 10 หลัก = UID 4 ไบต์ตีเป็นเลขฐานสิบ เติม 0 ข้างหน้าให้ครบ 10 หลัก (เก็บเป็น text เสมอ)
// เครื่องอ่าน USB แบบคีย์บอร์ดส่วนใหญ่กลับลำดับไบต์ (little-endian) — ถ้าเลขไม่ตรงกับที่พิมพ์บนบัตร ให้ตั้งเป็น false
const bool UID_REVERSED = true;

String uidDecimal(bool reversed) {
  uint32_t n = 0;
  byte len = min(rfid.uid.size, (byte)4);
  for (byte i = 0; i < len; i++) {
    byte b = rfid.uid.uidByte[reversed ? len - 1 - i : i];
    n = (n << 8) | b;
  }
  char buf[11];
  snprintf(buf, sizeof(buf), "%010lu", (unsigned long)n);
  return String(buf);
}

void showDuplicate(const String& uid, unsigned long waitMs) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 12, mode == MODE_IN ? "Already checked IN" : "Already checked OUT");
  oled.drawHLine(0, 15, 128);
  oled.setFont(u8g2_font_helvB12_tr);
  oled.drawStr((128 - oled.getStrWidth(uid.c_str())) / 2, 38, uid.c_str());
  oled.setFont(u8g2_font_6x12_tr);
  char line[24];
  snprintf(line, sizeof(line), "Wait %lus", (waitMs + 999) / 1000);
  oled.drawStr(0, 60, line);
  oled.sendBuffer();
}

void beepDuplicate() {
  tone(BUZZER_PIN, 900, 120);
  delay(170);
  tone(BUZZER_PIN, 900, 120);
  delay(150);
}

String readUid() {
  return uidDecimal(UID_REVERSED);
}

// กดปุ่ม (ขอบขาลง) = สลับโหมด
void pollModeButton() {
  static bool lastStable = HIGH;
  static bool lastRead = HIGH;
  static unsigned long changedAt = 0;

  bool now = digitalRead(MODE_BUTTON_PIN);
  if (now != lastRead) { lastRead = now; changedAt = millis(); }
  if (millis() - changedAt < BUTTON_DEBOUNCE_MS || now == lastStable) return;
  lastStable = now;
  if (now != LOW) return;

  mode = mode == MODE_IN ? MODE_OUT : MODE_IN;
  Serial.printf("Mode -> %s\n", mode == MODE_IN ? "IN" : "OUT");
  tone(BUZZER_PIN, mode == MODE_IN ? 2000 : 1500, 60);
  showingResult = false;
  showReady();
  sledBlink(modeColor(mode), 1);
  sledIdle();
}

void setup() {
  Serial.begin(115200);
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  sled.begin();
  sled.setBrightness(60);
  sledIdle();

  // ต่อ Wi-Fi แบบไม่รอ — อ่านบัตรได้ทันที นาฬิกาขึ้นเองเมื่อ sync NTP เสร็จ
  // ไม่เขียนค่า Wi-Fi ลงแฟลช และล้างค่าที่ค้างจากเฟิร์มแวร์ก่อน ๆ ก่อนต่อ
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  // reason: 2=auth expire, 15/204=handshake timeout (มักรหัสผิด), 201=no AP, 202=auth fail
  static WiFiEventHandler onDisconnect = WiFi.onStationModeDisconnected(
      [](const WiFiEventStationModeDisconnected& e) { Serial.printf("WiFi: disconnect reason=%d\n", e.reason); });
  Serial.printf("\nWiFi: MAC=%s SSID=%s\n", WiFi.macAddress().c_str(), WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(TZ_BANGKOK, NTP_SERVER_1, NTP_SERVER_2);

  bootId = String(ESP.random(), HEX);
  if (wsEnabled) {
    ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    ws.onEvent(onWs);
    ws.setReconnectInterval(5000);
    ws.enableHeartbeat(15000, 3000, 2);  // ping ทุก 15 วิ ไม่ตอบ 2 ครั้ง = ตัดแล้วต่อใหม่
    Serial.printf("WS: ws://%s:%d%s device=%s boot=%s\n", SERVER_HOST, SERVER_PORT, SERVER_PATH, DEVICE_ID,
                  bootId.c_str());
  } else {
    Serial.println("WS: off (SERVER_HOST empty)");
  }

  oled.begin();
  showReady();

  SPI.begin();
  rfid.PCD_Init();
  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("\nRC522 version: 0x%02X\n", ver);
  if (ver == 0x00 || ver == 0xFF) {
    // อ่าน register ไม่ได้ = สาย SPI/ไฟเลี้ยงมีปัญหา
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 24, "RC522 not found!");
    oled.drawStr(0, 44, "Check SS=D0 RST=D3");
    oled.drawStr(0, 58, "SPI D5-D7");
    oled.sendBuffer();
    beepError();
  } else {
    beepOk();
  }
}

void loop() {
  if (wsEnabled) {
    ws.loop();  // ต้องเรียกทุกรอบ — ห้ามเพิ่ม delay() ยาวกว่า ~0.5 วิ ไม่งั้น heartbeat หลุด
    pumpQueue();
  }
  pollModeButton();
  logWifi();

  if (showingResult && shownIsCard && wsEnabled && shownStatus == RES_WAITING && millis() - lastScanAt > ACK_WAIT_MS) {
    shownStatus = RES_OFFLINE;  // ยังอยู่ในคิว จะส่งเองเมื่อต่อได้
    showCard();
  }

  if (showingResult && millis() - lastScanAt > RESULT_SHOW_MS) {
    showingResult = false;
    showReady();
  }

  // วาดหน้ารอใหม่ทุกวินาทีให้นาฬิกาเดิน
  static time_t lastDrawnSec = 0;
  time_t nowSec = time(nullptr);
  if (!showingResult && nowSec != lastDrawnSec) {
    lastDrawnSec = nowSec;
    showReady();
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = readUid();

  unsigned long waitMs = blockedFor(uid);
  if (waitMs > 0) {
    Serial.printf("Duplicate %s blocked (%lus left)\n", uid.c_str(), (waitMs + 999) / 1000);
    showDuplicate(uid, waitMs);
    sledFill(sled.Color(255, 0, 0));
    beepDuplicate();
    sledFill(0);
    delay(100);
    sledBlink(sled.Color(255, 0, 0), 2);
    sledIdle();
    lastScanAt = millis();
    showingResult = true;
    shownIsCard = false;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  rememberScan(uid);
  uint32_t& count = mode == MODE_IN ? countIn : countOut;
  count++;
  String stamp = timeValid() ? nowText("%d/%m/%Y %H:%M:%S") : String("--/--/---- --:--:--");
  Serial.printf("%s  %s #%lu  %s  (reversed=%s, normal=%s, size=%u)\n", stamp.c_str(), mode == MODE_IN ? "IN" : "OUT",
                (unsigned long)count, uid.c_str(), uidDecimal(true).c_str(), uidDecimal(false).c_str(), rfid.uid.size);

  shownIsCard = true;
  shownCard = uid;
  shownCount = count;
  shownStatus = RES_WAITING;
  shownId = enqueueScan(uid);
  pumpQueue();  // ส่งทันทีก่อนเสียง/ไฟ ack จะได้กลับมาระหว่างนั้น
  showingResult = true;
  showCard();
  sledFill(modeColor(mode));
  beepOk();
  sledFill(0);
  delay(100);
  sledBlink(modeColor(mode), 2);
  sledIdle();

  lastScanAt = millis();
  showingResult = true;

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
