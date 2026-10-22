# CHECKIN_RFID

เครื่องแตะบัตร RFID สำหรับ check-in / check-out บน ESP8266 (NodeMCU v1.0 + บอร์ด INEX AX-WiFi)
อ่านบัตรด้วย RC522 แสดงเลขบัตร 10 หลัก + เวลาบน OLED มีโหมด IN/OUT, กันแตะซ้ำ, เสียงบี๊บและไฟ WS2812
และส่งผลแตะบัตรไป MECHA-CHECKIN backend ผ่าน WebSocket (ไม่ตั้ง server ก็ใช้แบบออฟไลน์ได้)

## ฮาร์ดแวร์ที่ต้องมี

| อุปกรณ์ | ขา |
|---|---|
| RC522 (SPI, ไฟ 3.3V) | SCK=D5, MISO=D6, MOSI=D7, **SS=D0**, **RST=D3** |
| OLED 128x64 SSD1306 (บน AX-WiFi) | SDA=D2, SCL=D1, I2C `0x3C` |
| Piezo (บนบอร์ด) | D8 |
| ปุ่มสลับ IN/OUT | D4 ลง GND — **ต้องเป็นปุ่มกดเด้งกลับ** ห้ามใช้สวิตช์ค้าง ไม่งั้นบอร์ดไม่บูต |
| WS2812 x2 (บนบอร์ด) | GPIO10 |

## ติดตั้ง

1. ติดตั้ง [VS Code](https://code.visualstudio.com/) + extension **PlatformIO IDE**
   (library ทั้งหมดใน `platformio.ini` จะโหลดเองตอน build ครั้งแรก)
2. clone repo แล้วเปิดโฟลเดอร์ใน VS Code
3. คัดลอก `include/secrets.h.example` เป็น `include/secrets.h` แล้วกรอกค่า
   (ไฟล์นี้ไม่ขึ้น git — ไม่มีไฟล์นี้จะ build ไม่ผ่าน)

   | ค่า | ความหมาย |
   |---|---|
   | `WIFI_SSID` / `WIFI_PASSWORD` | Wi-Fi 2.4 GHz |
   | `NTP_SERVER_1` / `NTP_SERVER_2` | server เวลา — ถ้าเน็ตบล็อก NTP ภายนอกให้ใส่ NTP ภายในที่ตัวแรก |
   | `SERVER_HOST` | ชื่อเครื่องหรือ IP ของ backend (ไม่ต้องใส่ `ws://`) — **ว่าง = ออฟไลน์ ไม่ส่งข้อมูล** |
   | `SERVER_PORT` | `80` ผ่าน Apache (production), `5000` ต่อตรง Node (dev) |
   | `SERVER_PATH` | ปกติ `/MECHA-CHECKIN/device` |
   | `DEVICE_ID` | ชื่อเครื่อง **ห้ามซ้ำกันระหว่างเครื่อง** (`A-Z a-z 0-9 _ -` ไม่เกิน 32 ตัว) |
   | `DEVICE_KEY` | ต้องตรงกับ `DEVICE_KEY` ใน `backend/.env` |

4. เสียบบอร์ดผ่าน USB แล้ว upload (ปิด serial monitor ก่อน)

```sh
pio run -t upload       # build + flash
pio device monitor      # ดู log ที่ 115200
```

## เช็คว่าใช้งานได้

ใน serial monitor ตอนบูตควรเห็น

- `RC522 version: 0x92` — ถ้าได้ `0x00` / `0xFF` แปลว่าสาย SPI หรือไฟของ RC522 มีปัญหา
- `WiFi: connected IP=...` — ถ้าต่อไม่ได้ใน 20 วินาที จะพิมพ์รายชื่อ Wi-Fi ที่มองเห็นให้ดู
- `Time synced: DD/MM/YYYY HH:MM:SS`
- `WS: ws://...` (หรือ `WS: off` ถ้าไม่ได้ตั้ง `SERVER_HOST`)

หน้าจอรอแตะบัตรมุมขวาล่างบอกสถานะ server: `WS:OK` ต่อได้, `WS:--` ยังไม่ต่อ, `WS:KEY` key/device ไม่ถูกต้อง
ตัวเลข `qN` คือจำนวนรายการที่ค้างส่ง (เก็บใน RAM สูงสุด 20 รายการ หายเมื่อดับไฟ)

## หมายเหตุ

- protocol กับ backend อยู่ที่ `docs/ESP_DEVICE_API.md` ในโปรเจกต์ MECHA_TIME_IN
- `board_build.flash_mode = dio` ต้องคงไว้ (ใช้ GPIO10 กับ WS2812) และ board ต้องเป็น `nodemcuv2`
- รายละเอียดการทำงานของโค้ดดูใน `CLAUDE.md`
