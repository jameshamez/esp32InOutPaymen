# PaymentESP - ESP32 Hardware Delivery

ระบบสร้าง QR PromptPay สำหรับติดตั้งบน ESP32 DevKit V1 และจอ OLED SSD1306 จริง รองรับ Static QR, Dynamic QR พร้อมยอดเงิน, หน้าเว็บตั้งค่า, Serial Output, REST POST และ Device Logs

## เริ่มต้นติดตั้งบน Hardware จริง

1. ต่อ OLED กับ ESP32: VCC->3V3, GND->GND, SDA->GPIO 21, SCL->GPIO 22
2. ต่อ ESP32 เข้าคอมพิวเตอร์ด้วยสาย USB Data
3. เปิดโฟลเดอร์นี้ใน Visual Studio Code และติดตั้ง PlatformIO IDE
4. Build และ Upload:

```bash
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -b 115200
```

5. เมื่อเปิดครั้งแรก ESP32 จะสร้าง WiFi AP:
   - SSID: `PaymentESP-Setup`
   - Password: `paymentesp`
6. เชื่อมต่อ AP แล้วเปิด `http://192.168.4.1/setup`
7. กรอก WiFi ของสถานที่ใช้งาน, PromptPay ID และ Webhook URL แล้วกด Save
8. หลัง ESP32 รีสตาร์ต ให้เชื่อมคอมพิวเตอร์กลับเข้า WiFi เดียวกับ ESP32
9. เปิด `http://paymentesp.local` หรือ IP ที่แสดงบน OLED/Serial

ค่าทั้งหมดถูกบันทึกใน NVS ของ ESP32 และยังอยู่หลังปิดเปิดเครื่อง

## Local Dashboard

```bash
npm ci
npm run dashboard
```

เปิด `http://localhost:3001` แล้วตั้ง `ESP32 Hardware URL` เป็น `http://paymentesp.local` หรือ IP ของบอร์ด เปิด `Sync to ESP32 hardware` ก่อนกด Save Settings หรือสร้าง QR

ถ้าต้องการรับ REST POST ที่ Dashboard ให้ใช้ IP ของคอมพิวเตอร์ในวง LAN เช่น `http://192.168.1.10:3001/api/webhook` และอนุญาต Firewall port 3001

### อ่านยอดจาก P-Points Pay-in

ESP32/Wokwi มีส่วน `P-Points Direct` สำหรับอ่านยอดจาก API รูปแบบนี้:

```text
https://p-points.com/sms_payin_rd.php?stn_id=S-24001&bank_id=X-9786&flg=W
```

ตั้งค่า `stn_id`, `bank_id`, `P-Points Amount` และ `P-Points QR Payload` แล้วกด `P-Points Dynamic QR` เพื่อสร้าง QR จาก payload K+ / P-Points จริงพร้อมระบุยอดเงิน หรือกด `P-Points Static QR` เพื่อใช้ payload เดิมแบบไม่ระบุยอดเงิน ระบบจะอ่านยอดรวมปัจจุบันจาก P-Points เพื่อบันทึกเป็น baseline ก่อน เช่น `["13.03","8"]` หมายถึงยอดรวม 13.03 บาท จาก 8 รายการ จากนั้นหน้า ESP32 จะเช็ก P-Points อัตโนมัติทุก 5 วินาที เป็นเวลา 5 นาที โดย OLED จะค้างหน้า QR ระหว่างรอจ่าย ไม่แสดงค่า Diff ระหว่างตรวจสอบ เมื่อยอดใหม่ต่างจาก baseline เช่น `+5.00` จึงจะแสดง `PAID` บน OLED และสั่ง pulse เฉพาะกรณียอดเพิ่มขึ้นเท่านั้น หากครบ 5 นาทีแล้วยังไม่พบยอดเพิ่ม ระบบจะถือว่า QR session หมดอายุและต้องสร้าง QR ใหม่

หมายเหตุ: QR PromptPay ที่สร้างเองไม่ได้มีวันหมดอายุในระบบธนาคารเหมือน Payment Gateway แต่ firmware จะจำกัด session ฝั่งตู้ 5 นาที เพื่อไม่ให้ยอดที่มาช้าหรือยอดจากรอบอื่นไปสั่ง pulse ผิดรอบ

ค่าเริ่มต้นของ `P-Points QR Payload` ถอดมาจาก QR K+ ที่ลูกค้าส่งให้:

```text
00020101021129390016A000000677010111031500499907526116353037645802TH6304E345
```

ข้อควรเช็กกับผู้ให้บริการก่อนใช้งานจริง: `flg=W` มีผล mark read หรือไม่, `amt` เป็นยอดรวมจากหลายรายการหรือรายการล่าสุด, และมี token/IP whitelist สำหรับป้องกันคนอื่นเรียก API หรือไม่

## API ของ ESP32

- `GET /api/static?ref=ORDER-0001`
- `GET /api/dynamic?amount=99.00&ref=ORDER-0001`
- `GET /api/logs`
- `GET /api/config`
- `POST /api/config` พร้อม `promptpay` และ `webhook`
- `GET /api/ppoints/qr?mode=dynamic&amount=15.00&payload=...`
- `GET /api/ppoints/baseline?stn_id=S-24001&bank_id=X-9786`
- `GET /api/ppoints/check?stn_id=S-24001&bank_id=X-9786`
- `GET|POST /setup` สำหรับตั้งค่า Hardware

## Wokwi สำหรับทดสอบเสริม

Wokwi ไม่ใช่ขั้นตอนติดตั้งหลักของชุดส่งมอบนี้ หากต้องการจำลองให้ Build environment แยก:

```bash
pio run -e wokwi
```

จากนั้นเปิด `diagram.json` และสั่ง `Wokwi: Start Simulator` โดย `wokwi.toml` จะใช้ firmware จาก `.pio/build/wokwi/`

### ทดสอบ P-Points โดยเปิดเฉพาะ ESP32/Wokwi

หลัง Wokwi เริ่มทำงาน ให้เปิด URL ของ ESP32/Wokwi เช่น:

```text
http://localhost:8180
```

ในหน้า ESP32 จะมีส่วน `P-Points Direct` พร้อมช่อง `stn_id`, `bank_id`, `P-Points Amount` และ `P-Points QR Payload` ให้กด `P-Points Dynamic QR` เพื่อสร้าง QR จาก payload K+ / P-Points จริงตามยอดที่กรอก หรือ `P-Points Static QR` สำหรับ QR ไม่ระบุยอด จากนั้นระบบจะตั้ง baseline จาก P-Points จริงและ auto check ทุก 5 วินาที เป็นเวลา 5 นาที ระหว่างเช็ก OLED จะยังค้าง QR ไว้ให้สแกน เมื่อพบยอดเพิ่มจะแสดง `PAID` บน OLED และ pulse ตามจำนวนเงิน ไม่ต้องเปิด Local Dashboard `localhost:3001`

### จำลอง SMS เงินเข้าใน Wokwi

เปิด Serial Monitor ที่ `115200` แล้วพิมพ์ตัวอย่างข้อความ:

```text
SMS: เงินเข้า 15.00 บาท
```

หรือใช้คำสั่งสั้น:

```text
PAY 15
```

Firmware จะดึงยอดเงินแรกจากข้อความ แสดง `PAID` บน OLED และสั่ง GPIO 26 pulse ตามจำนวนเงินบาท เช่น `15.00` จะ pulse 15 ครั้ง โดย LED `PAID PULSE` ใน `diagram.json` จะกระพริบตามจำนวน pulse

## ทดสอบ

```bash
g++ -std=c++17 -Iinclude tests/test_promptpay.cpp src/PromptPayQR.cpp -o /tmp/paymentesp-test
/tmp/paymentesp-test
npm run test:dashboard
pio run -e esp32dev
pio run -e wokwi
```

## ขอบเขตระบบ

QR ที่สร้างสามารถใช้โอนเงินเข้า PromptPay ที่กำหนดได้ แต่ REST POST และ Logs ยืนยันเหตุการณ์สร้าง QR เท่านั้น ไม่ใช่การยืนยันว่าเงินเข้าบัญชีแล้ว การตรวจรับเงินจริงต้องเชื่อม Bank API หรือ Payment Gateway เพิ่มเติม
