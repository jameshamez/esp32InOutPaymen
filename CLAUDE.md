# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

PaymentESP generates Thai PromptPay QR codes on real ESP32 DevKit V1 hardware with a TJC4832T135 touchscreen display. It supports static QR, dynamic QR with amount, a web setup page, serial output, REST POST webhooks, and device logs. A Node.js "local dashboard" runs on a computer on the same LAN to configure the device and optionally receive webhook events and process Omise PromptPay charges. README.md is in Thai and is the source of truth for hardware wiring and operational behavior — consult it for details beyond what's summarized here.

## Commands

### Firmware (PlatformIO / ESP32)

```bash
pio run -e esp32dev                    # build firmware
pio run -e esp32dev -t upload          # build and flash to a connected ESP32
pio device monitor -b 115200           # serial monitor
```

### Firmware unit tests (host-native, no hardware/PlatformIO needed)

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

### Local dashboard (Node.js, no dependencies beyond `qrcode`)

```bash
npm ci
npm run dashboard          # starts local-dashboard/server.js on :3001 (node local-dashboard/server.js)
npm run test:dashboard     # tests/test_dashboard.js — spawns the dashboard server as a child process
npm run test:omise         # tests/test_omise.js — tests the Omise client wrapper
```

### Wokwi simulator (secondary — for testing without hardware)

```bash
pio run -e wokwi           # separate build env, defines PAYMENTESP_WOKWI
```

Then open `diagram.json` in VS Code and run "Wokwi: Start Simulator" (`wokwi.toml` points at `.pio/build/wokwi/`). The simulated device UI is reachable at `http://localhost:8180`. Wokwi is not part of the primary hardware delivery workflow — the real target is physical ESP32 hardware.

## Architecture

### Firmware (`src/`, `include/`)

- `include/PromptPayQR.h` / `src/PromptPayQR.cpp` — pure, framework-independent logic: builds EMV/PromptPay TLV QR payloads (`buildPromptPayPayload`), CRC16-CCITT checksums, money formatting, and JSON serialization of `QrEvent`. This file has no Arduino/ESP32 dependencies, which is what makes it host-testable via g++ (see test command above). Any change to PromptPay payload structure or event JSON shape belongs here.
- `src/main.cpp` — the entire Arduino firmware in one file (~1500 lines), all in an anonymous namespace plus `setup()`/`loop()`. Structure to know:
  - **Config & persistence**: WiFi SSID/password, PromptPay ID, and webhook URL are stored in NVS via `Preferences` (namespace `promptpay`) and loaded in `loadPersistedConfig()`.
  - **WiFi bootstrap**: `connectWifi()` tries the saved STA credentials; on failure it falls back to a `PaymentESP-Setup` AP (open at `192.168.4.1/setup`) served by the same `WebServer` instance, gated by `setupMode`.
  - **HTTP routes**: registered in `setup()` — `/`, `/setup`, and the `/api/*` endpoints listed in README.md. Handlers are `handleXxx()` functions in the same file; there's no router abstraction.
  - **Display rendering**: `render*()` functions (`renderEventToOled`, `renderPaymentConfirmed`, `renderPpointsDelta`, `renderCustomerQrStatus`, `renderNetworkStatus`, etc.) send Nextion/TJC protocol UART commands (`include/DisplayHMI.h`/`src/DisplayHMI.cpp`, host-testable like `PromptPayQR.h`/`.cpp`) to a TJC4832T135 touchscreen over `Serial2` (GPIO16 RX2 / GPIO17 TX2, 115200 baud). Status text goes to four Text components (`t0`-`t3`); the QR itself is drawn as filled-rectangle blocks directly on the page background (`sendQrToDisplay`/`clearQrArea` in `main.cpp`) rather than through a native QR component, because TJC's T1-series QRcode component hard-caps text input at 84 bytes — shorter than this firmware's real PromptPay payloads. Both `t0`-`t3` and the reserved QR drawing area (x=10,y=10,280x280) must exist in the `.tft` project uploaded to the display through TJC's USART HMI Editor. `qrcode.h`/`buildQrCode()` is shared with `qrSvgFromText()` (the web UI's `/api/qr.svg`).
  - **Payment confirmation / GPIO pulse**: `confirmPayment()` dedupes by `paymentId` and triggers `startPaymentPulsesForAmount()`, which drives a non-blocking pulse-queue state machine (`processPaymentPulses()`, called every `loop()`). Two modes, selected via `twoPinPulseMode` (`/api/pulse/config?mode=1pin|2pin`): 1-pin mode pulses GPIO 26 once per whole THB; 2-pin mode splits the amount into tens (GPIO 27) and ones (GPIO 26) digit pulses. Total pulses capped at `MAX_PAYMENT_PULSES`.
  - **P-Points integration**: two parallel flows against `https://p-points.com/sms_payin_rd.php` (`fetchPpointsResult` → `parsePpointsResponse`). The QR-session flow (`processPpointsResult`) records a baseline total, then a 5-minute session (`ppointsSessionActive`/`ppointsSessionExpiresAt`, `PPOINTS_QR_TTL_MS`) during which the display holds the QR while the frontend polls `/api/ppoints/check` every `PPOINTS_POLL_INTERVAL_MS`; the session expires without pulsing if no increase is seen in time. Separately, `processPpointsMonitor()` runs an always-on 5-second auto-poll (`/api/ppoints/monitor?action=start|stop`, `ppointsMonitorEnabled`/`ppointsMonitorIntervalMs`) that pulses on any detected balance increase without requiring a QR session at all. Both flows call `confirmPayment` and mirror what the Node dashboard does for local-only testing.
  - **Serial SMS simulator**: `processSerialSimulator()`/`processSerialCommand()` reads lines from Serial (`PAY 15`, `SMS: ...`) for testing payment confirmation without real P-Points calls — useful in Wokwi.
  - The `/` page embeds a full inline HTML/CSS/JS single-page UI as a C string literal (no separate frontend build step) — edit it in place inside `handleRoot()`.

### Local dashboard (`local-dashboard/`)

- `server.js` — a dependency-light Node `http` server (no Express) that mirrors much of the firmware's PromptPay/P-Points logic in JS (`normalizePromptPayTarget`, `crc16CcittFalse`, `tlv`) so it can run and be tested on a dev machine without hardware. Routes are dispatched via sequential `if (requestUrl.pathname === "...")` checks starting around line 1032. Key routes: `/local/*` (dashboard-side QR generation/config), `/api/omise/*` (Omise PromptPay charge creation/status/webhook), `/api/ppoints/*` (P-Points check/mock, same 5-minute-session semantics as firmware), `/api/webhook` (receives REST POST events forwarded from the ESP32, `isAllowedTarget()` restricts forwarding to LAN/localhost/mDNS hosts as an SSRF guard).
  - Dashboard config (`promptPayId`, `webhookUrl`, P-Points station/bank IDs) persists to `local-dashboard/data/config.json`; payment/event history persists to `local-dashboard/data/payments.json`. Both are gitignored and written atomically (write to `.tmp`, then rename).
- `omise.js` — thin wrapper (`OmiseClient`) around the Omise REST API for creating/retrieving PromptPay charges. Configured via `OMISE_SECRET_KEY` in `.env` (see `.env.example`); the dashboard falls back to firmware-generated QR when Omise isn't configured (`omise.configured`).

### Tests

- `tests/test_promptpay.cpp` — host-native assertions against `PromptPayQR.cpp` (TLV structure, CRC validity, JSON event shape). No test framework, just `assert()`.
- `tests/test_dashboard.js` / `tests/test_omise.js` — spawn `local-dashboard/server.js` as a real child process (random port derived from PID) and hit its HTTP routes, including a fake P-Points server for `test_dashboard.js`. No mocking framework.
- There are no ESP32-hardware-in-the-loop tests; `src/main.cpp` is exercised manually via `pio run -e esp32dev`/`upload`/`monitor` or the Wokwi simulator, and via the serial SMS simulator commands.

### Documentation & delivery artifacts

- `docs/` holds the Thai-language install/test-report/requirements source docs; `scripts/build_*_docx.py` (Python, uses `python-docx`/`Pillow`) render these into `.docx` deliverables under `deliverables/<version>/documents/`. These scripts are one-off document generators for client delivery packages, not part of the app build — only touch them if asked to regenerate delivery documents.
- `deliverables/` contains prebuilt, versioned delivery packages (source + firmware + documents + evidence) — treat as generated/archival output, not a place to make source edits.

## Key conventions

- PromptPay payload construction and P-Points delta/session logic are duplicated between `src/main.cpp` (C++/Arduino) and `local-dashboard/server.js` (JS) by design — the dashboard is a standalone dev/testing tool, not a proxy for the firmware. If you change payload/CRC/session behavior in one, check whether the other needs the same change and whether `docs/`/README.md need updating.
- The firmware has two physical GPIO outputs for payment confirmation pulses (GPIO 26 = ones/whole-THB, GPIO 27 = tens digit in 2-pin mode), selected via `twoPinPulseMode`; total pulse count capped at `MAX_PAYMENT_PULSES` = 200, driven non-blocking from `loop()`.
- QR/event JSON is hand-serialized (no JSON library) in both C++ (`eventToJson`/`jsonEscape` in `PromptPayQR.cpp`) and JS — keep escaping consistent with existing helpers rather than introducing a dependency.
- Secrets (`OMISE_PUBLIC_KEY`, `OMISE_SECRET_KEY`, WiFi passwords) belong only in `.env` (gitignored) or ESP32 NVS (`Preferences`), never in delivery packages (see `DELIVERY.md`).
