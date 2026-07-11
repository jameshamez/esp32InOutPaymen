# คู่มือติดตั้ง PaymentESP บน ESP32 จริง

ขั้นตอนหลักของชุดส่งมอบนี้คือ Upload firmware ลง ESP32 DevKit V1 และตั้งค่าผ่าน Setup AP โดยไม่ต้องแก้ WiFi ใน Source Code

## ขั้นตอนย่อ

1. ต่อ OLED: VCC->3V3, GND->GND, SDA->GPIO 21, SCL->GPIO 22
2. เปิด `source/` ใน Visual Studio Code และติดตั้ง PlatformIO IDE
3. รัน `pio run -e esp32dev -t upload`
4. เปิด Serial Monitor ด้วย `pio device monitor -b 115200`
5. เชื่อม WiFi `PaymentESP-Setup` รหัส `paymentesp`
6. เปิด `http://192.168.4.1/setup`
7. กรอก WiFi, PromptPay ID และ Webhook แล้วบันทึก
8. หลังรีสตาร์ต เปิด `http://paymentesp.local` หรือ IP จาก OLED
9. รัน Dashboard ด้วย `npm ci` และ `npm run dashboard`
10. ตั้ง Device URL ให้ตรงกับ ESP32 แล้วทดสอบ Static/Dynamic QR

## คำสั่งตรวจสอบ

```bash
curl http://paymentesp.local/api/config
curl "http://paymentesp.local/api/static?ref=STATIC-TEST"
curl "http://paymentesp.local/api/dynamic?amount=15.00&ref=DYNAMIC-TEST"
curl http://paymentesp.local/api/logs
```

## การกู้คืน WiFi

ถ้า ESP32 เชื่อม WiFi ที่บันทึกไว้ไม่ได้ภายในประมาณ 20 วินาที จะกลับมาเปิด `PaymentESP-Setup` อัตโนมัติ ให้เชื่อม AP และตั้งค่าที่ `192.168.4.1/setup` ใหม่

## Wokwi

ใช้เฉพาะทดสอบเสริม โดย Build ด้วย `pio run -e wokwi` แล้วสั่ง Wokwi: Start Simulator ไม่ใช่ขั้นตอนที่ลูกค้าต้องทำเมื่อรับเครื่องจริง

## หมายเหตุขอบเขต

ระบบบันทึกเวลาและยอดของเหตุการณ์สร้าง QR และส่ง REST POST แต่ไม่ตรวจสอบยอดเงินเข้าบัญชีธนาคารอัตโนมัติ
