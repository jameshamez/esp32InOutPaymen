# การติดตั้ง Firmware ลง ESP32 และ OLED จริง

## 1. ต่อสาย OLED SSD1306

| OLED | ESP32 DevKit V1 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

จอใช้ I2C address `0x3C` และความละเอียด `128x64` ควรถอดสาย USB ก่อนเปลี่ยนการต่อสาย

## 2. Upload Firmware

```bash
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -b 115200
```

หาก Upload ไม่เริ่ม ให้เลือก Serial Port ที่ถูกต้อง กดปุ่ม BOOT ค้างระหว่างขึ้น `Connecting...` แล้วปล่อยเมื่อเริ่มเขียน Flash

## 3. ตั้งค่าเครื่องครั้งแรก

เมื่อยังไม่มี WiFi หรือเชื่อมต่อไม่ได้ ESP32 จะเปิด Setup AP อัตโนมัติ:

- SSID: `PaymentESP-Setup`
- Password: `paymentesp`
- URL: `http://192.168.4.1/setup`

กรอก WiFi SSID, WiFi Password, PromptPay ID และ Webhook URL จากนั้นกด Save and restart ESP32 ค่าจะถูกบันทึกใน NVS

## 4. เข้าใช้งานหลังเชื่อม WiFi

เชื่อมโทรศัพท์หรือคอมพิวเตอร์เข้า WiFi เดียวกับ ESP32 แล้วเปิด:

```text
http://paymentesp.local
```

ถ้า `.local` เปิดไม่ได้ ให้ใช้ IP ที่แสดงบน OLED หรือ Serial Monitor

## 5. ตั้งค่า Dashboard

ตั้ง ESP32 Hardware URL เป็น `http://paymentesp.local` หรือ IP ของบอร์ด แล้วเปิด Sync to ESP32 hardware

Webhook ของเครื่องจริงต้องเป็น IP/Domain ที่ ESP32 เข้าถึงได้ ห้ามใช้ `localhost` หรือ `host.wokwi.internal` เช่น:

```text
http://192.168.1.10:3001/api/webhook
```

## 6. ตรวจรับก่อนส่งลูกค้า

- ESP32 boot ได้โดยไม่ต่อคอมพิวเตอร์
- OLED แสดง READY และ IP
- `GET /api/config` ตอบ JSON
- Save PromptPay ID แล้วปิดเปิดเครื่อง ค่ายังคงเดิม
- Static QR และ Dynamic QR สแกนได้
- Serial, Device Logs และ REST POST แสดง amount/reference ตรงกัน
