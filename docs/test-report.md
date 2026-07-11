# รายงานผลทดสอบก่อนส่งมอบ - Hardware Edition

วันที่ทดสอบ: 28 มิถุนายน 2026

## ขอบเขต

ทดสอบ Source และ Firmware สำหรับ ESP32 DevKit V1 จริง รวมโหมดตั้งค่า WiFi ผ่าน `PaymentESP-Setup`, การบันทึก NVS, PromptPay QR, Dashboard, Serial, REST POST และ Logging โดย Wokwi ใช้เป็นหลักฐานจำลองเสริม

## ผลทดสอบอัตโนมัติ

| รายการ | วิธีตรวจสอบ | ผล |
|---|---|---|
| Build firmware Hardware | `pio run -e esp32dev` | ผ่าน |
| Build firmware Wokwi | `pio run -e wokwi` | ผ่าน |
| PromptPay CRC/Payload | Host unit test | ผ่าน |
| Static/Dynamic QR | Host unit test | ผ่าน |
| Dashboard integration | `npm run test:dashboard` | ผ่าน |
| Setup AP/NVS source path | ตรวจ Source และ Compile | ผ่าน |
| REST POST/Logging | Integration test และ Wokwi evidence | ผ่าน |

## Hardware Acceptance ก่อนส่งเครื่อง

รายการนี้ต้องตรวจซ้ำกับ ESP32/OLED ตัวที่จะส่งลูกค้า:

1. Upload ด้วย `pio run -e esp32dev -t upload`
2. บอร์ดเปิด AP `PaymentESP-Setup`
3. เข้า `192.168.4.1/setup` และบันทึก WiFi/PromptPay ได้
4. รีสตาร์ตแล้ว OLED แสดง READY และ IP
5. เปิด `paymentesp.local` หรือ IP ของบอร์ดได้
6. ปิดเปิดไฟใหม่แล้วค่าจาก NVS ยังคงเดิม
7. Static QR และ Dynamic QR สแกนได้และชื่อผู้รับถูกต้อง
8. Serial, Device Logs และ REST POST แสดง amount/reference ตรงกัน

หากยังไม่ได้ต่อบอร์ดจริง ให้สถานะ Hardware Acceptance เป็น `รอตรวจบนเครื่องจริง` ห้ามใช้ภาพ Wokwi แทนผลตรวจ Hardware

## ขอบเขตผลการชำระเงิน

REST POST status `202` หมายถึงระบบรับเหตุการณ์สร้าง QR แล้ว ไม่ใช่การยืนยันว่าเงินเข้าบัญชี การตรวจรับเงินจริงต้องใช้ Bank API หรือ Payment Gateway เพิ่มเติม
