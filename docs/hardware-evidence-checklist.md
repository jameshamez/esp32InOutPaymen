# Hardware Evidence Checklist

จัดทำจาก ESP32 และ OLED ตัวที่จะส่งลูกค้าหลัง Upload และตั้งค่าครบแล้ว

- `01-hardware-wiring.jpg` เห็น ESP32, OLED, VCC, GND, SDA GPIO 21 และ SCL GPIO 22
- `02-oled-ready.jpg` OLED แสดง PaymentESP READY และ IP
- `03-oled-dynamic-qr.jpg` OLED แสดง Dynamic QR, amount และ reference
- `04-serial-output.png` Serial แสดง WiFi connected, IP และ QR Event
- `05-device-logs.png` Device Logs แสดง amount/reference
- `06-rest-post.png` REST receiver แสดง Event และ HTTP status

ก่อนส่งไฟล์ให้ลูกค้า ให้ปิดบัง WiFi Password, PromptPay ID ที่ไม่จำเป็น, Secret Key และข้อมูลส่วนตัวอื่น
