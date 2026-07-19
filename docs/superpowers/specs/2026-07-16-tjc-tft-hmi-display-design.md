# TJC TFT HMI display support — design

## Revision (post-implementation): QR rendering changed from native component to pixel-fill drawing

The original design (below) sent the QR payload as text to the display's built-in `qr0`
QRcode component. The final whole-branch code review measured real PromptPay payloads at
92–105 characters (dynamic/static QR with a reference) and found that TJC's "T1 series"
(the tier `TJC4832T135` belongs to) caps the QRcode component's text input at **84 bytes
of hardware limit** — confirmed against TJC/Nextion's own documented Basic-tier limit, not
just this firmware's `DISPLAY_QR_MAX_LEN` guard. Real payloads exceed that hardware ceiling
in the two most common cases (dynamic QR, static QR with a reference); only "static, no
reference" (74 chars) reliably fit.

Decision (user-approved): stop using the native `qr0` component entirely. Draw the QR as
filled-rectangle blocks directly onto the page background instead, using Nextion's native
`fill x,y,width,height,color` instruction — the same approach the original SSD1306 OLED
code used (`display.fillRect` per module), just sent as UART draw commands instead of I2C
pixel writes. This has no text-length ceiling at all (capacity is governed by
`buildQrCode()`'s existing QR-version-6-to-15 range, same as the OLED always had), so the
84-byte limit no longer applies. Baud rate raised from 9600 to 115200 to keep draw time
reasonable (~40-60 fill commands after row run-length-encoding, vs. up to hundreds of
individual module commands unencoded).

**Consequence:** the display-side component contract below no longer needs a `qr0`
QRcode component at all — only `t0`-`t3` Text components. `DISPLAY_QR_MAX_LEN` is removed.
See "Architecture" for the updated `sendQrToDisplay` design.

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

Baud rate: **115200** (revised — see revision note above; was 9600 in the original design).
The USART HMI Editor project must be configured for 115200 baud to match (documented as a
setup step, not enforced by firmware).

## Display-side component contract

The user creates a **single page** (page 0) in the USART HMI Editor with exactly these
component names:

| Component | Type | Purpose |
|---|---|---|
| `t0` | Text | Title / status line (e.g. "PromptPay QR", "PAID", "P-Points") |
| `t1` | Text | Line 2 (mode / amount / IP) |
| `t2` | Text | Line 3 (amount / reference / count) |
| `t3` | Text | Line 4 (reference / hostname / extra status) |

No `qr0` QRcode component is needed (see revision note) — the QR is drawn as filled
rectangles directly on the page background within a reserved area:
**x=190, y=30, 280x280 pixels** (bottom-right of the 480x320 screen — moved from the
original top-left x=10,y=10 placement at user request). The user's `t0`-`t3` placement in
the editor must not overlap that rectangle (e.g. place text in the remaining top-left
area, x=0-180 or y=0-20).

No other pages or components are required. Firmware never sends a page-switch command
(`page N`) — everything happens by updating text/drawing on page 0.

## Architecture

### New module: `include/DisplayHMI.h` / `src/DisplayHMI.cpp`

Framework-independent (no Arduino/ESP32 headers), following the same pattern as
`PromptPayQR.h`/`.cpp` so it's host-testable via g++.

```cpp
// Builds a Nextion/TJC protocol command as raw bytes ready to write to the UART:
//   <component>.txt="<escaped value>"<0xFF><0xFF><0xFF>
// Escapes backslash and double-quote in value. No trailing newline.
std::string buildHmiTextCommand(const std::string& component, const std::string& value);

// Builds a Nextion/TJC "fill" draw command as raw bytes ready to write to the UART:
//   fill <x>,<y>,<width>,<height>,<color><0xFF><0xFF><0xFF>
// color is a 16-bit RGB565 value (0 = black, 65535 = white). No validation of
// coordinate ranges — callers are responsible for staying on-screen.
std::string buildHmiFillCommand(int x, int y, int width, int height, int color);
```

(`DISPLAY_QR_MAX_LEN` from the original design is removed — see revision note.)

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

  | Function | `t0` | `t1` | `t2` | `t3` | QR area |
  |---|---|---|---|---|---|
  | `renderEventToOled` | "PromptPay" | mode (Dynamic/Static) | amount | reference | draw payload (see below), or `t0="QR too large"` if no QR version 6-15 fits |
  | `renderPaymentConfirmed` | "PAID" | "THB " + amount | reference | "" | clear (white fill) — original OLED redraw never shows a QR here |
  | `renderPpointsDelta` | "P-Points" | "Total " + total | "Diff " + delta | count + baseline/pulse status | clear (white fill) |
  | `renderPpointsExpired` | "P-Points" | "QR expired" | "Total " + total | "Count " + count | clear (white fill) |
  | `renderCustomerQrStatus` | "P-Points" | bankId | stationId | reference | draw payload if non-empty, else clear — matches original's conditional `buildQrCode` call |
  | `renderNetworkStatus` (setupMode) | "PaymentESP SETUP" | AP SSID | "PASS: " + password | setup URL | clear (white fill) |
  | `renderNetworkStatus` (connected) | "PaymentESP READY" | local IP | "paymentesp.local" | "Open browser" | clear (white fill) |

  Every render function explicitly clears or redraws the QR area rather than leaving it
  untouched — unlike the OLED's implicit full-screen `clearDisplay()`, the TFT's drawn
  pixels persist independently of text-component updates, so a stale QR would stay on
  screen if a later render call forgot to clear it.

  Boot message in `setupDisplay()` becomes `sendToDisplay("t0", "Booting...")` plus a
  clear of the QR area, no OLED-specific init.

  **`sendQrToDisplay(payload)` algorithm** (replaces the text-based version):
  1. Call the existing `buildQrCode(payload, qrcode, buffer)` (unchanged, still shared
     with `qrSvgFromText()`). If it returns false (no version 6-15 fits), send
     `t0="QR too large"` and return — same fallback message the OLED used.
  2. Clear the reserved area: `fill 10,10,280,280,65535` (white).
  3. Compute `scale = max(1, 280 / qrcode.size)`.
  4. For each module row, scan left to right and run-length-encode contiguous dark
     modules into a single `fill` command per run (`fill x,y,runLength*scale,scale,0`)
     instead of one command per module — cuts command count roughly in half to a
     quarter for typical QR patterns, keeping draw time reasonable at 115200 baud.

### Tests

- `tests/test_display_hmi.cpp` (host-native, same style as `tests/test_promptpay.cpp`):
  asserts `buildHmiTextCommand` produces the correct `component.txt="value"` + 3×`0xFF`
  byte sequence, that `"` / `\` in the value are escaped, and that `buildHmiFillCommand`
  produces the correct `fill x,y,width,height,color` + 3×`0xFF` byte sequence.
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
2. Create a new project targeting TJC4832T135, set UART baud to **115200**.
3. Add Text components named exactly `t0`, `t1`, `t2`, `t3` on page 0, positioned so
   none overlap the reserved QR drawing area (x=190, y=30, 280x280 pixels, bottom-right
   of the screen) — e.g. place text in the top-left area.
4. Compile and upload the resulting `.tft` file to the display over serial.
