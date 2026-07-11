# PaymentESP Delivery Package

ชุดส่งมอบแบ่งเป็น 4 ส่วน:

- `source/` Source Code ESP32, Dashboard, tests และไฟล์ตั้งค่า
- `firmware/` Firmware ที่ Build แล้วสำหรับ ESP32 DevKit V1
- `documents/` README, คู่มือติดตั้ง, Test Report และ Requirement Checklist
- `evidence/` ภาพ OLED, Serial, Device Logs, REST POST และข้อมูล event

เริ่มต้นใช้งานจากคู่มือ Word ใน `documents/` หรือ `documents/installation.md` ชุดนี้ใช้ ESP32/OLED จริงเป็นเป้าหมายหลัก

ก่อนส่งลูกค้าต้อง Upload `firmware/firmware.bin` หรือ Build/Upload จาก `source/` ลงบอร์ดจริง และทำ Hardware Acceptance ตาม `documents/test-report.md`

ห้ามนำไฟล์ `.env`, WiFi Password หรือ Secret Key ใส่ในชุดส่งมอบ ระบบตามขอบเขตนี้ไม่ต้องใช้ Payment Gateway Key
