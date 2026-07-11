# Requirement Checklist - ESP32 Hardware Edition

| Requirement | การทำงาน | สถานะส่งมอบ |
|---|---|---|
| ESP32 เชื่อมต่อ WiFi | ตั้งค่าผ่าน Setup AP และบันทึก NVS | พร้อมทดสอบบนบอร์ดจริง |
| ตั้งค่า PromptPay ID | Setup page, Dashboard และ API | ผ่านการทดสอบ Source/Integration |
| Static QR PromptPay | QR ไม่มี Tag ยอดเงิน | ผ่าน |
| Dynamic QR PromptPay | QR ระบุยอดเงินได้ | ผ่าน |
| OLED SSD1306 | GPIO 21/22, address 0x3C | พร้อมทดสอบบนจอจริง |
| Serial Output | mode, ID, amount, reference, เวลา, payload | ผ่านใน Firmware/Wokwi |
| REST API POST | ส่ง JSON และบันทึก HTTP status | ผ่าน Integration/Wokwi |
| Logging | เก็บ 20 รายการล่าสุด | ผ่าน |
| Source Code | PlatformIO Hardware + Wokwi environments | พร้อมส่ง |
| Firmware | `esp32dev` binaries | พร้อมส่ง |
| คู่มือติดตั้ง | ขั้นตอน Upload, Setup AP และตรวจรับ | พร้อมส่ง |

## ต้องทำก่อนส่งเครื่องลูกค้า

- Upload Firmware ลงบอร์ดตัวจริง
- ทดสอบสายและจอ OLED จริง
- ตั้ง WiFi/PromptPay ที่จะใช้ หรือให้ลูกค้าตั้งผ่าน Setup AP
- ตรวจชื่อผู้รับด้วยแอปธนาคาร
- บันทึก Serial และภาพ Hardware Acceptance ของเครื่องตัวจริง

ระบบไม่รวมการตรวจสอบยอดเงินเข้าบัญชีอัตโนมัติ
