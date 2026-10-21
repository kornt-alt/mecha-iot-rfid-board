#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>
#include <time.h>
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
  char buf[24];
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
  snprintf(line, sizeof(line), "IN:%lu  OUT:%lu", (unsigned long)countIn, (unsigned long)countOut);
  oled.drawStr(0, 63, line);
  oled.sendBuffer();
}

void showCard(const String& uid) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 12, mode == MODE_IN ? "Checked IN" : "Checked OUT");
  oled.drawHLine(0, 15, 128);
  oled.setFont(u8g2_font_helvB12_tr);
  oled.drawStr((128 - oled.getStrWidth(uid.c_str())) / 2, 38, uid.c_str());
  oled.setFont(u8g2_font_6x12_tr);
  char line[24];
  snprintf(line, sizeof(line), "#%lu  OK", (unsigned long)(mode == MODE_IN ? countIn : countOut));
  oled.drawStr(0, 60, line);
  if (timeValid()) {
    String hms = nowText("%H:%M:%S");
    oled.drawStr(128 - oled.getStrWidth(hms.c_str()), 60, hms.c_str());
  }
  oled.sendBuffer();
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
  pollModeButton();
  logWifi();

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

  showCard(uid);
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
