# TJC TFT HMI display support — design

## Context

PaymentESP firmware (`src/main.cpp`) currently drives a 128x64 SSD1306 OLED over I2C
(`Wire.begin(21, 22)`, Adafruit_GFX/Adafruit_SSD1306) to show QR codes and status text.
The target hardware is changing to a **TJC4832T135** (T1 series, 3.5", 480x320,
resistive touch, 8MB flash, 3584 bytes SRAM) — a Nextion-protocol-compatible "smart"
HMI display with its own onboard GPU/MCU. It is controlled over UART with short ASCII
text commands; it renders everything itself, including a built-in QR code component.

The SSD1306 OLED is being **replaced**, not supplemented. This spec covers the ESP32
firmware side only. Screen layout inside the TJC display project (component placement)
must be created by the user in TJC's official "USART HMI" editor
(download: https://tjc1688.com) — that GUI step is outside what this repo/agent can do.

## Confirmed hardware wiring

Customer-wired already, verified from photos:
- Display TX → ESP32 **GPIO16 (RX2)**
- Display RX → ESP32 **GPIO17 (TX2)**
- Display 5V → 5V, Display GND → GND (shared ground with ESP32)

These are the ESP32's standard hardware `Serial2` pins — no custom pin remap needed,
`Serial2.begin(...)` uses them by default. Confirmed clear of conflicts:
- `GPIO26` (payment pulse output) — untouched
- `GPIO0/1` a.k.a. `TX0`/`RX0` (USB serial, used for flashing/Serial Monitor) — untouched

Baud rate: **9600**, matching TJC/Nextion factory default. The USART HMI Editor project
must be configured for 9600 baud to match (documented as a setup step, not enforced by
firmware).

## Display-side component contract

The user creates a **single page** (page 0) in the USART HMI Editor with exactly these
component names:

| Component | Type | Purpose |
|---|---|---|
| `qr0` | QRcode | Shows the current PromptPay/P-Points QR payload |
| `t0` | Text | Title / status line (e.g. "PromptPay QR", "PAID", "P-Points") |
| `t1` | Text | Line 2 (mode / amount / IP) |
| `t2` | Text | Line 3 (amount / reference / count) |
| `t3` | Text | Line 4 (reference / hostname / extra status) |

No other pages or components are required. Firmware never sends a page-switch command
(`page N`) — everything happens by updating text on page 0.

## Architecture

### New module: `include/DisplayHMI.h` / `src/DisplayHMI.cpp`

Framework-independent (no Arduino/ESP32 headers), following the same pattern as
`PromptPayQR.h`/`.cpp` so it's host-testable via g++.

```cpp
// Builds a Nextion/TJC protocol command as raw bytes ready to write to the UART:
//   <component>.txt="<escaped value>"<0xFF><0xFF><0xFF>
// Escapes backslash and double-quote in value. No trailing newline.
std::string buildHmiTextCommand(const std::string& component, const std::string& value);

// Safety cap applied by callers before building a qr0 command.
constexpr size_t DISPLAY_QR_MAX_LEN = 80;
```

`main.cpp` owns the actual `Serial2.write()` call; `DisplayHMI.cpp` only builds byte
strings, no I/O, so it can be unit tested the same way `PromptPayQR.cpp` is.

### `src/main.cpp` changes

- Remove: `#include <Adafruit_GFX.h>`, `#include <Adafruit_SSD1306.h>`, `Wire.begin(21, 22)`,
  the `display` global, `buildQrCode()`'s use inside the render functions listed below,
  and the fillRect-based QR blit loop.
- Keep `qrcode.h` / `buildQrCode()` for `qrSvgFromText()` — that endpoint (`/api/qr.svg`,
  used by the web UI) is unrelated to the physical display and is unchanged.
- Add: `#include "DisplayHMI.h"`, a `HardwareSerial hmiDisplay(2)` (or equivalent) set up
  in `setup()` via `hmiDisplay.begin(9600, SERIAL_8N1, 16, 17)`, and a small
  `sendToDisplay(component, value)` helper that calls `buildHmiTextCommand` and writes
  the result to `hmiDisplay`.
- `displayReady` becomes unconditionally `true` once `Serial2.begin()` runs (no probe —
  the protocol has no reliable handshake to check for at boot).
- Rewrite all 7 render functions to send text updates instead of drawing pixels:

  | Function | `t0` | `t1` | `t2` | `t3` | `qr0` |
  |---|---|---|---|---|---|
  | `renderEventToOled` | "PromptPay" | mode (Dynamic/Static) | amount | reference | payload (guarded by `DISPLAY_QR_MAX_LEN`, else `t0="QR too long"` and skip) |
  | `renderPaymentConfirmed` | "PAID" | "THB " + amount | reference | "" | "" (clear — original OLED redraw never shows a QR here) |
  | `renderPpointsDelta` | "P-Points" | "Total " + total | "Diff " + delta | count + baseline/pulse status | "" (clear — original OLED redraw never shows a QR here) |
  | `renderPpointsExpired` | "P-Points" | "QR expired" | "Total " + total | "Count " + count | "" (clear) |
  | `renderCustomerQrStatus` | "P-Points" | bankId | stationId | reference | payload if non-empty (same length guard), else "" (clear) — matches original's conditional `buildQrCode` call |
  | `renderNetworkStatus` (setupMode) | "PaymentESP SETUP" | AP SSID | "PASS: " + password | setup URL | "" |
  | `renderNetworkStatus` (connected) | "PaymentESP READY" | local IP | "paymentesp.local" | "Open browser" | "" |

  Every render function explicitly sets `qr0` (either to a payload or to `""`) rather
  than leaving it untouched — unlike the OLED's implicit full-screen `clearDisplay()`,
  the TFT's components persist independently, so a stale QR would stay on screen if a
  later render call forgot to clear it.

  Boot message in `setupDisplay()` becomes a single `sendToDisplay("t0", "Booting...")` (or similar), no OLED-specific init.

### Tests

- `tests/test_display_hmi.cpp` (host-native, same style as `tests/test_promptpay.cpp`):
  asserts `buildHmiTextCommand` produces the correct `component.txt="value"` + 3×`0xFF`
  byte sequence, and that `"` / `\` in the value are escaped.
- Existing `tests/test_promptpay.cpp` is unaffected (PromptPayQR.cpp untouched).
- No hardware-in-the-loop test — visual verification on the real TJC display is manual,
  same as the current OLED workflow.

## Explicitly out of scope

- Touch input handling (no on-screen buttons wired to firmware actions).
- Multi-page navigation on the display.
- Any change to PromptPay payload generation, P-Points logic, or web/dashboard code —
  this is a display-driver swap only.

## Manual steps required from the user (not doable by this agent)

1. Download/install `USART HMI.exe` from https://tjc1688.com.
2. Create a new project targeting TJC4832T135, set UART baud to 9600.
3. Add components named exactly `qr0`, `t0`, `t1`, `t2`, `t3` on page 0.
4. Compile and upload the resulting `.tft` file to the display over serial.
