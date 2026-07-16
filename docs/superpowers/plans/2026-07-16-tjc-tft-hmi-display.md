# TJC TFT HMI Display Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the SSD1306 I2C OLED driver in the ESP32 firmware with UART text commands targeting a TJC4832T135 Nextion-protocol touchscreen, so every existing payment/QR/status display path renders on the new hardware instead.

**Architecture:** A new framework-independent module (`DisplayHMI.h`/`.cpp`, host-testable like `PromptPayQR.h`/`.cpp`) builds Nextion/TJC protocol command strings. `main.cpp` sends those strings over `Serial2` (GPIO16 RX2 / GPIO17 TX2, already wired) instead of drawing pixels via Adafruit_GFX/SSD1306. All seven existing `render*()` call sites are rewritten to update four text components (`t0`-`t3`) and one QR component (`qr0`) on a single display page.

**Tech Stack:** C++17 (Arduino/ESP32 framework for `main.cpp`, host g++ for the new pure module and its test), PlatformIO, existing `ricmoo/QRCode` library (kept, used elsewhere).

## Global Constraints

- Display component names are fixed and must match exactly what the user creates in the TJC USART HMI Editor: `qr0`, `t0`, `t1`, `t2`, `t3` (spec: `docs/superpowers/specs/2026-07-16-tjc-tft-hmi-display-design.md`).
- UART: GPIO16 = RX2, GPIO17 = TX2, baud 9600 (matches customer's existing wiring and TJC factory default).
- `qr0` payload length capped at `DISPLAY_QR_MAX_LEN` = 80 chars; over the limit, skip the QR update and show `"QR too long"` on `t0` instead.
- Every render function must explicitly set `qr0` (to a payload or to `""`) — TFT components persist independently, unlike the OLED's implicit full-screen clear.
- `qrcode.h` / `buildQrCode()` stays in `main.cpp` — still used by `qrSvgFromText()` for the web UI's `/api/qr.svg`, untouched by this plan.
- No touch input handling, no multi-page navigation — out of scope (see spec).
- `main.cpp` also carries in-progress, uncommitted 2-pin payment-pulse (GPIO26/GPIO27) and P-Points
  5-second monitor code (`twoPinPulseMode`, `processPpointsMonitor`, `handlePulseConfig`,
  `handlePpointsMonitor`, etc.). None of it touches display rendering or GPIO16/17 — leave it
  untouched. Task 2's "replace" snippets were re-verified against the current file with this code
  present; if a snippet fails to match exactly when implementing, stop and re-read the surrounding
  function rather than approximating the edit.

---

### Task 1: `DisplayHMI` module (TDD, host-testable)

**Files:**
- Create: `include/DisplayHMI.h`
- Create: `src/DisplayHMI.cpp`
- Test: `tests/test_display_hmi.cpp`

**Interfaces:**
- Produces: `std::string buildHmiTextCommand(const std::string& component, const std::string& value)` — returns `component + ".txt=\"" + escaped(value) + "\"" + "\xFF\xFF\xFF"`, escaping `\` and `"` in `value` by prefixing each with `\`.
- Produces: `constexpr size_t DISPLAY_QR_MAX_LEN = 80;`

- [ ] **Step 1: Create the header**

Create `include/DisplayHMI.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>

constexpr size_t DISPLAY_QR_MAX_LEN = 80;

std::string buildHmiTextCommand(const std::string& component, const std::string& value);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_display_hmi.cpp`:

```cpp
#include <cassert>
#include <iostream>

#include "DisplayHMI.h"

int main() {
  const std::string simple = buildHmiTextCommand("t0", "PAID");
  assert(simple == std::string("t0.txt=\"PAID\"") + "\xFF\xFF\xFF");

  const std::string withQuoteAndBackslash = buildHmiTextCommand("t3", "say \"hi\\bye\"");
  assert(withQuoteAndBackslash ==
         std::string("t3.txt=\"say \\\"hi\\\\bye\\\"\"") + "\xFF\xFF\xFF");

  const std::string empty = buildHmiTextCommand("qr0", "");
  assert(empty == std::string("qr0.txt=\"\"") + "\xFF\xFF\xFF");

  assert(DISPLAY_QR_MAX_LEN == 80);

  std::cout << "DisplayHMI tests passed\n";
  return 0;
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `g++ -std=c++17 -Iinclude tests/test_display_hmi.cpp src/DisplayHMI.cpp -o /tmp/paymentesp-test-hmi`
Expected: FAIL — compiler error, `src/DisplayHMI.cpp` does not exist yet (`fatal error: src/DisplayHMI.cpp: No such file or directory` or linker "undefined reference to buildHmiTextCommand").

- [ ] **Step 4: Write minimal implementation**

Create `src/DisplayHMI.cpp`:

```cpp
#include "DisplayHMI.h"

namespace {

std::string escapeHmiValue(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 4);
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::string buildHmiTextCommand(const std::string& component, const std::string& value) {
  std::string command = component + ".txt=\"" + escapeHmiValue(value) + "\"";
  command += '\xFF';
  command += '\xFF';
  command += '\xFF';
  return command;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `g++ -std=c++17 -Iinclude tests/test_display_hmi.cpp src/DisplayHMI.cpp -o /tmp/paymentesp-test-hmi && /tmp/paymentesp-test-hmi`
Expected: PASS — prints `DisplayHMI tests passed`

- [ ] **Step 6: Commit**

```bash
git add include/DisplayHMI.h src/DisplayHMI.cpp tests/test_display_hmi.cpp
git commit -m "feat: add DisplayHMI module for Nextion/TJC protocol commands"
```

---

### Task 2: Replace SSD1306 driver with DisplayHMI in `main.cpp`

**Files:**
- Modify: `src/main.cpp`
- Modify: `platformio.ini`

**Interfaces:**
- Consumes: `buildHmiTextCommand(component, value)` and `DISPLAY_QR_MAX_LEN` from Task 1.
- Produces: `sendToDisplay(component, value)` and `sendQrToDisplay(payload)` helpers used by all render functions below.

- [ ] **Step 1: Update includes and remove OLED globals**

In `src/main.cpp`, replace the include block (original lines 1–19):

```cpp
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <qrcode.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <stdexcept>
#include <vector>

#include "PromptPayQR.h"
```

with:

```cpp
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <qrcode.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <stdexcept>
#include <vector>

#include "DisplayHMI.h"
#include "PromptPayQR.h"
```

Then, in the constants block, replace:

```cpp
constexpr size_t MAX_LOGS = 20;
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET = -1;
constexpr int PAYMENT_PULSE_PIN = 26;
```

with:

```cpp
constexpr size_t MAX_LOGS = 20;
constexpr int DISPLAY_UART_RX_PIN = 16;
constexpr int DISPLAY_UART_TX_PIN = 17;
constexpr unsigned long DISPLAY_UART_BAUD = 9600;
constexpr int PAYMENT_PULSE_PIN = 26;
```

Then replace the globals line:

```cpp
WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences preferences;
```

with:

```cpp
WebServer server(80);
Preferences preferences;
```

- [ ] **Step 2: Insert the send helpers before the first render function**

`renderEventToOled` (defined right after `buildQrCode`) is the first function that will call the
new helpers, and C++ requires a function to be defined before its first use in the same file —
so the helpers must be inserted **between** `buildQrCode` and `renderEventToOled`, not down near
`setupDisplay()` (which is hundreds of lines later in the file).

Replace:

```cpp
bool buildQrCode(const std::string& text, QRCode& qrcode, std::vector<uint8_t>& buffer) {
  for (uint8_t version = 6; version <= 15; ++version) {
    buffer.assign(qrcode_getBufferSize(version), 0);
    if (qrcode_initText(&qrcode, buffer.data(), version, ECC_LOW, text.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void renderEventToOled(const QrEvent& event) {
```

with:

```cpp
bool buildQrCode(const std::string& text, QRCode& qrcode, std::vector<uint8_t>& buffer) {
  for (uint8_t version = 6; version <= 15; ++version) {
    buffer.assign(qrcode_getBufferSize(version), 0);
    if (qrcode_initText(&qrcode, buffer.data(), version, ECC_LOW, text.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void sendToDisplay(const std::string& component, const std::string& value) {
  if (!displayReady) {
    return;
  }
  const std::string command = buildHmiTextCommand(component, value);
  Serial2.write(reinterpret_cast<const uint8_t*>(command.data()), command.size());
}

void sendQrToDisplay(const std::string& payload) {
  if (payload.empty()) {
    sendToDisplay("qr0", "");
    return;
  }
  if (payload.size() > DISPLAY_QR_MAX_LEN) {
    sendToDisplay("t0", "QR too long");
    sendToDisplay("qr0", "");
    return;
  }
  sendToDisplay("qr0", payload);
}

void renderEventToOled(const QrEvent& event) {
```

(this step only inserts the two helpers — `renderEventToOled`'s body is rewritten next, in Step 3)

- [ ] **Step 3: Rewrite `renderEventToOled`**

Replace:

```cpp
void renderEventToOled(const QrEvent& event) {
  if (!displayReady) {
    return;
  }

  QRCode qrcode;
  std::vector<uint8_t> buffer;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (!buildQrCode(event.payload, qrcode, buffer)) {
    display.setCursor(0, 0);
    display.println("QR too large");
    display.display();
    return;
  }

  const int maxQrPixels = 56;
  const int scale = std::max(1, maxQrPixels / qrcode.size);
  const int qrPixels = qrcode.size * scale;
  const int offsetX = 2;
  const int offsetY = (SCREEN_HEIGHT - qrPixels) / 2;

  for (uint8_t y = 0; y < qrcode.size; ++y) {
    for (uint8_t x = 0; x < qrcode.size; ++x) {
      if (qrcode_getModule(&qrcode, x, y)) {
        display.fillRect(offsetX + x * scale, offsetY + y * scale, scale, scale, SSD1306_WHITE);
      }
    }
  }

  display.setTextSize(1);
  display.setCursor(64, 4);
  display.println("PromptPay");
  display.setCursor(64, 18);
  display.println(event.mode == "dynamic" ? "Dynamic" : "Static");
  display.setCursor(64, 32);
  display.println(event.amount.empty() ? "No amount" : event.amount.c_str());
  display.setCursor(64, 46);
  display.println(event.reference.c_str());
  display.display();
}
```

with:

```cpp
void renderEventToOled(const QrEvent& event) {
  sendToDisplay("t0", "PromptPay");
  sendToDisplay("t1", event.mode == "dynamic" ? "Dynamic" : "Static");
  sendToDisplay("t2", event.amount.empty() ? "No amount" : event.amount);
  sendToDisplay("t3", event.reference);
  sendQrToDisplay(event.payload);
}
```

- [ ] **Step 4: Rewrite `renderPaymentConfirmed`**

Replace:

```cpp
void renderPaymentConfirmed(const std::string& amount, const std::string& reference) {
  if (!displayReady) {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println("PAID");
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.print("THB ");
  display.println(amount.c_str());
  display.setCursor(0, 44);
  display.println(reference.c_str());
  display.display();
}
```

with:

```cpp
void renderPaymentConfirmed(const std::string& amount, const std::string& reference) {
  sendToDisplay("t0", "PAID");
  sendToDisplay("t1", "THB " + amount);
  sendToDisplay("t2", reference);
  sendToDisplay("t3", "");
  sendToDisplay("qr0", "");
}
```

- [ ] **Step 5: Rewrite `renderPpointsDelta`**

Replace:

```cpp
void renderPpointsDelta(const std::string& total,
                        const std::string& delta,
                        const std::string& count,
                        bool baseline,
                        bool pulseTriggered) {
  if (!displayReady) {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("P-Points");
  display.setCursor(0, 14);
  display.print("Total ");
  display.println(total.c_str());
  display.setCursor(0, 28);
  display.print("Diff ");
  display.println(delta.c_str());
  display.setCursor(0, 42);
  display.print("Count ");
  display.println(count.c_str());
  display.setCursor(0, 56);
  display.println(baseline ? "Baseline" : (pulseTriggered ? "Pulse OK" : "No pulse"));
  display.display();
}
```

with:

```cpp
void renderPpointsDelta(const std::string& total,
                        const std::string& delta,
                        const std::string& count,
                        bool baseline,
                        bool pulseTriggered) {
  sendToDisplay("t0", "P-Points");
  sendToDisplay("t1", "Total " + total);
  sendToDisplay("t2", "Diff " + delta);
  const std::string status = baseline ? "Baseline" : (pulseTriggered ? "Pulse OK" : "No pulse");
  sendToDisplay("t3", "Count " + count + " " + status);
  sendToDisplay("qr0", "");
}
```

- [ ] **Step 6: Rewrite `renderPpointsExpired`**

Replace:

```cpp
void renderPpointsExpired(const std::string& total, const std::string& count) {
  if (!displayReady) {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("P-Points");
  display.setCursor(0, 14);
  display.println("QR expired");
  display.setCursor(0, 28);
  display.print("Total ");
  display.println(total.c_str());
  display.setCursor(0, 42);
  display.print("Count ");
  display.println(count.c_str());
  display.setCursor(0, 56);
  display.println("Create new QR");
  display.display();
}
```

with:

```cpp
void renderPpointsExpired(const std::string& total, const std::string& count) {
  sendToDisplay("t0", "P-Points");
  sendToDisplay("t1", "QR expired");
  sendToDisplay("t2", "Total " + total);
  sendToDisplay("t3", "Count " + count);
  sendToDisplay("qr0", "");
}
```

- [ ] **Step 7: Rewrite `renderCustomerQrStatus`**

Note: this function's body is not a byte-for-byte match of the other render functions' vintage —
it picked up a "compact vs full-size QR" branch (`compactQr`) in the in-progress pulse/monitor
work already on disk. The replacement below still removes all of it, since the TFT's `qr0`
component handles its own sizing and this pixel-scaling logic has no TFT equivalent.

Replace:

```cpp
void renderCustomerQrStatus(const std::string& bankId,
                            const std::string& stationId,
                            const std::string& reference,
                            const std::string& payload) {
  if (!displayReady) {
    return;
  }

  QRCode qrcode;
  std::vector<uint8_t> buffer;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (!payload.empty() && buildQrCode(payload, qrcode, buffer)) {
    const int maxQrPixels = 42;
    const int scale = std::max(1, maxQrPixels / qrcode.size);
    const int qrPixels = qrcode.size * scale;
    const bool compactQr = qrPixels <= maxQrPixels;
    const int offsetX = compactQr ? 2 : (SCREEN_WIDTH - qrcode.size) / 2;
    const int offsetY = compactQr ? (SCREEN_HEIGHT - qrPixels) / 2 : (SCREEN_HEIGHT - qrcode.size) / 2;

    for (uint8_t y = 0; y < qrcode.size; ++y) {
      for (uint8_t x = 0; x < qrcode.size; ++x) {
        if (qrcode_getModule(&qrcode, x, y)) {
          display.fillRect(offsetX + x * scale, offsetY + y * scale, scale, scale, SSD1306_WHITE);
        }
      }
    }

    if (!compactQr) {
      display.display();
      return;
    }
  }

  display.setCursor(50, 0);
  display.println("P-Points");
  display.setCursor(50, 14);
  display.println(shortenForDisplay(bankId, 12).c_str());
  display.setCursor(50, 28);
  display.println(shortenForDisplay(stationId, 12).c_str());
  display.setCursor(50, 42);
  display.println(shortenForDisplay(reference, 12).c_str());
  display.display();
}
```

with:

```cpp
void renderCustomerQrStatus(const std::string& bankId,
                            const std::string& stationId,
                            const std::string& reference,
                            const std::string& payload) {
  sendToDisplay("t0", "P-Points");
  sendToDisplay("t1", shortenForDisplay(bankId, 12));
  sendToDisplay("t2", shortenForDisplay(stationId, 12));
  sendToDisplay("t3", shortenForDisplay(reference, 12));
  sendQrToDisplay(payload);
}
```

- [ ] **Step 8: Rewrite `setupDisplay`**

`setupDisplay()` is defined much later in the file (near `connectWifi()`), well after the
helpers from Step 2 — so by this point `sendToDisplay` is already visible to it.

Replace:

```cpp
void setupDisplay() {
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 display not found.");
    return;
  }

  displayReady = true;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("PromptPay QR");
  display.println("Booting...");
  display.display();
}
```

with:

```cpp
void setupDisplay() {
  Serial2.begin(DISPLAY_UART_BAUD, SERIAL_8N1, DISPLAY_UART_RX_PIN, DISPLAY_UART_TX_PIN);
  displayReady = true;
  sendToDisplay("t0", "PromptPay QR");
  sendToDisplay("t1", "Booting...");
  sendToDisplay("t2", "");
  sendToDisplay("t3", "");
  sendToDisplay("qr0", "");
}
```

- [ ] **Step 9: Rewrite `renderNetworkStatus`**

Replace:

```cpp
void renderNetworkStatus() {
  if (!displayReady) {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (setupMode) {
    display.println("PaymentESP SETUP");
    display.println(SETUP_AP_SSID);
    display.println("PASS: paymentesp");
    display.println("192.168.4.1/setup");
  } else {
    display.println("PaymentESP READY");
    display.println(WiFi.localIP());
    display.println("paymentesp.local");
    display.println("Open browser");
  }
  display.display();
}
```

with:

```cpp
void renderNetworkStatus() {
  if (setupMode) {
    sendToDisplay("t0", "PaymentESP SETUP");
    sendToDisplay("t1", SETUP_AP_SSID);
    sendToDisplay("t2", "PASS: paymentesp");
    sendToDisplay("t3", "192.168.4.1/setup");
  } else {
    sendToDisplay("t0", "PaymentESP READY");
    sendToDisplay("t1", std::string(WiFi.localIP().toString().c_str()));
    sendToDisplay("t2", "paymentesp.local");
    sendToDisplay("t3", "Open browser");
  }
  sendToDisplay("qr0", "");
}
```

- [ ] **Step 10: Remove unused Adafruit libraries from `platformio.ini`**

Replace:

```ini
lib_deps =
  ricmoo/QRCode@0.0.1
  adafruit/Adafruit SSD1306@^2.5.15
  adafruit/Adafruit GFX Library@^1.12.3
```

with:

```ini
lib_deps =
  ricmoo/QRCode@0.0.1
```

- [ ] **Step 11: Build to verify**

Run: `pio run -e wokwi`
Expected: `SUCCESS` — no references to `display`, `Adafruit_SSD1306`, `Adafruit_GFX`, `Wire`, `SCREEN_WIDTH`, `SCREEN_HEIGHT`, or `OLED_RESET` remain (grep the file if the build fails on an undefined symbol to find a missed call site).

Then run: `pio run -e esp32dev`
Expected: `SUCCESS`

- [ ] **Step 12: Commit**

```bash
git add src/main.cpp platformio.ini
git commit -m "feat: drive TJC4832T135 TFT via UART instead of SSD1306 OLED"
```

---

### Task 3: Update `CLAUDE.md` documentation

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update the firmware architecture bullet list**

In the `### Firmware (\`src/\`, \`include/\`)` section of `CLAUDE.md`, replace the bullet
(note the 2-space leading indent, matching the surrounding list):

```
  - **OLED rendering**: `render*()` functions (`renderEventToOled`, `renderPaymentConfirmed`, `renderPpointsDelta`, `renderCustomerQrStatus`, etc.) draw directly via Adafruit GFX; the QR itself is rasterized with the `qrcode` library and blitted as filled rects.
```

with:

```
  - **Display rendering**: `render*()` functions (`renderEventToOled`, `renderPaymentConfirmed`, `renderPpointsDelta`, `renderCustomerQrStatus`, `renderNetworkStatus`, etc.) send Nextion/TJC protocol UART text commands (`include/DisplayHMI.h`/`src/DisplayHMI.cpp`, host-testable like `PromptPayQR.h`/`.cpp`) to a TJC4832T135 touchscreen over `Serial2` (GPIO16 RX2 / GPIO17 TX2, 9600 baud) — the display renders its own QR code and text via a fixed set of components (`qr0`, `t0`-`t3`) that must exist in the `.tft` project uploaded to the display through TJC's USART HMI Editor. `qrcode.h`/`buildQrCode()` is still used separately for `qrSvgFromText()` (the web UI's `/api/qr.svg`).
```

- [ ] **Step 2: Add the DisplayHMI test command**

In the `### Firmware unit tests (host-native, no hardware/PlatformIO needed)` section of
`CLAUDE.md`, replace:

````
```bash
g++ -std=c++17 -Iinclude tests/test_promptpay.cpp src/PromptPayQR.cpp -o /tmp/paymentesp-test
/tmp/paymentesp-test
```

This compiles `PromptPayQR.cpp` (the PromptPay TLV/QR payload logic) standalone against `tests/test_promptpay.cpp`, independent of the Arduino/ESP32 framework in `src/main.cpp`.
````

with:

````
```bash
g++ -std=c++17 -Iinclude tests/test_promptpay.cpp src/PromptPayQR.cpp -o /tmp/paymentesp-test
/tmp/paymentesp-test
```

This compiles `PromptPayQR.cpp` (the PromptPay TLV/QR payload logic) standalone against `tests/test_promptpay.cpp`, independent of the Arduino/ESP32 framework in `src/main.cpp`.

The same pattern applies to `DisplayHMI.cpp` (Nextion/TJC UART command formatting):

```bash
g++ -std=c++17 -Iinclude tests/test_display_hmi.cpp src/DisplayHMI.cpp -o /tmp/paymentesp-test-hmi
/tmp/paymentesp-test-hmi
```
````

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: document DisplayHMI module and TFT display architecture"
```
