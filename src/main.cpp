#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
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

namespace {

#ifdef PAYMENTESP_WOKWI
constexpr const char* DEFAULT_WIFI_SSID = "Wokwi-GUEST";
constexpr const char* DEFAULT_WIFI_PASSWORD = "";
constexpr const char* DEFAULT_WEBHOOK_URL = "http://host.wokwi.internal:3001/api/webhook";
#else
constexpr const char* DEFAULT_WIFI_SSID = "";
constexpr const char* DEFAULT_WIFI_PASSWORD = "";
constexpr const char* DEFAULT_WEBHOOK_URL = "";
#endif
constexpr const char* DEFAULT_PROMPTPAY_ID = "0812345678";
constexpr const char* SETUP_AP_SSID = "PaymentESP-Setup";
constexpr const char* SETUP_AP_PASSWORD = "paymentesp";
constexpr const char* DEFAULT_MDNS_HOSTNAME = "paymentesp";
constexpr const char* PREFERENCES_NAMESPACE = "promptpay";
constexpr const char* PREFERENCES_ID_KEY = "id";
constexpr const char* PREFERENCES_WEBHOOK_KEY = "webhook";
constexpr const char* PREFERENCES_HOSTNAME_KEY = "hostname";
constexpr const char* PREFERENCES_WIFI_SSID_KEY = "wifiSsid";
constexpr const char* PREFERENCES_WIFI_PASSWORD_KEY = "wifiPass";
constexpr const char* DEFAULT_PPOINTS_STATION_ID = "P-24001";
constexpr const char* DEFAULT_PPOINTS_BANK_ID = "X-9786";
constexpr const char* DEFAULT_PPOINTS_QR_PAYLOAD =
    "00020101021129390016A000000677010111031500499907526116353037645802TH6304E345";
constexpr const char* PPOINTS_BASE_URL = "https://p-points.com/sms_payin_rd.php";
constexpr size_t MAX_LOGS = 20;
constexpr int DISPLAY_UART_RX_PIN = 16;
constexpr int DISPLAY_UART_TX_PIN = 17;
constexpr unsigned long DISPLAY_UART_BAUD = 9600;
constexpr int QR_AREA_X = 320;
constexpr int QR_AREA_Y = 160;
constexpr int QR_AREA_SIZE = 150;
constexpr int PAYMENT_PULSE_PIN = 15;
constexpr int PAYMENT_PULSE_TENS_PIN = 2;
constexpr unsigned long DEFAULT_PULSE_WIDTH_MS = 25;
constexpr unsigned long DEFAULT_PULSE_SPACE_WIDTH_MS = 25;
constexpr int DEFAULT_PULSE_DIVISOR_ONES = 1;
constexpr int DEFAULT_PULSE_DIVISOR_TENS = 10;
constexpr unsigned long MAX_PULSE_WIDTH_MS = 200;
constexpr int MAX_PULSE_DIVISOR_ONES = 100;
constexpr int MAX_PULSE_DIVISOR_TENS = 10000;
constexpr unsigned long PPOINTS_QR_TTL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PPOINTS_POLL_INTERVAL_MS = 5000;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;
constexpr unsigned long MDNS_REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PPOINTS_MONITOR_INTERVAL_MS = 5000;
constexpr int MAX_PAYMENT_PULSES = 200;

WebServer server(80);
Preferences preferences;
PromptPayConfig qrConfig{DEFAULT_PROMPTPAY_ID, "PROMPTPAY", "BANGKOK"};
std::string webhookUrl = DEFAULT_WEBHOOK_URL;
std::string mdnsHostname = DEFAULT_MDNS_HOSTNAME;
std::deque<QrEvent> logs;
String wifiSsid = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
bool displayReady = false;
bool clockSynced = false;
bool setupMode = false;
bool paymentPulseActive = false;
bool paymentPulseHigh = false;
unsigned long paymentPulseNextAt = 0;
int paymentPulseCurrentPin = PAYMENT_PULSE_PIN;
int paymentPulsesPending = 0;
int paymentPulsesCompleted = 0;
int paymentPulsesTotal = 0;
int paymentPulsesOnes = 0;
int paymentPulsesTens = 0;
std::deque<int> paymentPulseQueue;
std::string lastPaymentId;
std::string lastPaymentAmount;
std::string lastPaymentReference;
String serialCommandBuffer;
bool hasPpointsPreviousTotal = false;
double lastPpointsTotalAmount = 0.0;
std::string lastPpointsCount;
bool ppointsSessionActive = false;
unsigned long ppointsSessionExpiresAt = 0;
bool twoPinPulseMode = false;
unsigned long pulseWidthMs = DEFAULT_PULSE_WIDTH_MS;
unsigned long pulseSpaceWidthMs = DEFAULT_PULSE_SPACE_WIDTH_MS;
int pulseDivisorOnes = DEFAULT_PULSE_DIVISOR_ONES;
int pulseDivisorTens = DEFAULT_PULSE_DIVISOR_TENS;
bool ppointsMonitorEnabled = true;
bool ppointsMonitorContinuous = true;
unsigned long ppointsMonitorIntervalMs = PPOINTS_MONITOR_INTERVAL_MS;
unsigned long ppointsMonitorTimeoutMs = PPOINTS_QR_TTL_MS;
unsigned long ppointsMonitorStartedAt = 0;
unsigned long ppointsMonitorNextAt = 0;
unsigned long ppointsMonitorChecks = 0;
std::string ppointsMonitorStationId = DEFAULT_PPOINTS_STATION_ID;
std::string ppointsMonitorBankId = DEFAULT_PPOINTS_BANK_ID;
std::string ppointsMonitorLastError;

std::string shortenForDisplay(const std::string& value, size_t maxLength = 10) {
  if (value.size() <= maxLength) {
    return value;
  }
  if (maxLength <= 3) {
    return value.substr(0, maxLength);
  }
  return value.substr(0, maxLength - 3) + "...";
}

String argOrDefault(const char* name, const String& fallback) {
  return server.hasArg(name) ? server.arg(name) : fallback;
}

std::string tlvField(const char* id, const std::string& value) {
  char len[3];
  std::snprintf(len, sizeof(len), "%02u", static_cast<unsigned>(value.size()));
  return std::string(id) + len + value;
}

std::string stripQrCrc(const std::string& payload) {
  const size_t marker = payload.rfind("6304");
  if (marker != std::string::npos && marker + 8 == payload.size()) {
    return payload.substr(0, marker);
  }
  return payload;
}

std::string withQrCrc(const std::string& payloadWithoutCrc) {
  std::string out = stripQrCrc(payloadWithoutCrc) + "6304";
  char crc[5];
  std::snprintf(crc, sizeof(crc), "%04X", crc16CcittFalse(out));
  out += crc;
  return out;
}

bool buildPpointsPayloadFromTemplate(const std::string& qrTemplate,
                                     double amount,
                                     bool dynamicQr,
                                     std::string& out,
                                     std::string& error) {
  const std::string source = stripQrCrc(qrTemplate);
  if (source.size() < 8) {
    error = "P-Points QR payload is too short";
    return false;
  }

  const std::string amountText = formatMoney(amount);
  if (dynamicQr && amountText.empty()) {
    error = "P-Points Dynamic QR amount must be greater than 0";
    return false;
  }

  std::string rebuilt;
  bool amountInserted = false;
  bool sawAmount = false;
  size_t pos = 0;
  while (pos + 4 <= source.size()) {
    const std::string id = source.substr(pos, 2);
    const int length = String(source.substr(pos + 2, 2).c_str()).toInt();
    if (length < 0 || pos + 4 + static_cast<size_t>(length) > source.size()) {
      error = "P-Points QR payload has invalid TLV length";
      return false;
    }

    std::string value = source.substr(pos + 4, static_cast<size_t>(length));
    pos += 4 + static_cast<size_t>(length);

    if (id == "01") {
      value = dynamicQr ? "12" : "11";
      rebuilt += tlvField("01", value);
      continue;
    }

    if (id == "54") {
      sawAmount = true;
      if (dynamicQr) {
        rebuilt += tlvField("54", amountText);
      }
      continue;
    }

    if (dynamicQr && !sawAmount && !amountInserted && (id == "58" || id == "59" || id == "60" || id == "62")) {
      rebuilt += tlvField("54", amountText);
      amountInserted = true;
    }
    rebuilt += tlvField(id.c_str(), value);
  }

  if (pos != source.size()) {
    error = "P-Points QR payload has trailing invalid data";
    return false;
  }

  if (dynamicQr && !sawAmount && !amountInserted) {
    rebuilt += tlvField("54", amountText);
  }

  out = withQrCrc(rebuilt);
  return true;
}

bool validatePromptPayId(const std::string& promptpay, std::string& error) {
  try {
    PromptPayConfig testConfig = qrConfig;
    testConfig.promptPayId = promptpay;
    buildPromptPayPayload(testConfig, 1.0, "VALIDATE", true);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool applyPromptPayFromRequest(std::string& error, bool persist = false) {
  if (!server.hasArg("promptpay")) {
    return true;
  }

  const std::string promptpay = server.arg("promptpay").c_str();
  if (promptpay.empty()) {
    error = "promptpay is empty";
    return false;
  }

  if (!validatePromptPayId(promptpay, error)) {
    return false;
  }

  qrConfig.promptPayId = promptpay;
  if (persist) {
    preferences.putString(PREFERENCES_ID_KEY, qrConfig.promptPayId.c_str());
  }
  return true;
}

bool validateWebhookUrl(const std::string& url, std::string& error) {
  if (url.empty()) {
    return true;
  }
  if (url.size() > 240 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
    error = "webhook must be empty or start with http:// or https://";
    return false;
  }
  return true;
}

bool validateHostname(const std::string& hostname, std::string& error) {
  if (hostname.empty() || hostname.size() > 32) {
    error = "hostname must be 1-32 characters";
    return false;
  }
  for (char ch : hostname) {
    const bool isAlnum = std::isalnum(static_cast<unsigned char>(ch)) != 0;
    if (!isAlnum && ch != '-') {
      error = "hostname may only contain letters, numbers, and hyphens";
      return false;
    }
  }
  if (hostname.front() == '-' || hostname.back() == '-') {
    error = "hostname must not start or end with a hyphen";
    return false;
  }
  return true;
}

bool applyWebhookFromRequest(std::string& error, bool persist = false) {
  if (!server.hasArg("webhook")) {
    return true;
  }

  const std::string requested = server.arg("webhook").c_str();
  if (!validateWebhookUrl(requested, error)) {
    return false;
  }

  webhookUrl = requested;
  if (persist) {
    preferences.putString(PREFERENCES_WEBHOOK_KEY, webhookUrl.c_str());
  }
  return true;
}

void loadPersistedConfig() {
  preferences.begin(PREFERENCES_NAMESPACE, false);
  wifiSsid = preferences.getString(PREFERENCES_WIFI_SSID_KEY, DEFAULT_WIFI_SSID);
  wifiPassword = preferences.getString(PREFERENCES_WIFI_PASSWORD_KEY, DEFAULT_WIFI_PASSWORD);
#ifdef PAYMENTESP_WOKWI
  wifiSsid = DEFAULT_WIFI_SSID;
  wifiPassword = DEFAULT_WIFI_PASSWORD;
#endif

  const String saved = preferences.getString(PREFERENCES_ID_KEY, DEFAULT_PROMPTPAY_ID);
  std::string error;
  if (validatePromptPayId(saved.c_str(), error)) {
    qrConfig.promptPayId = saved.c_str();
  } else {
    Serial.print("Saved PromptPay ID is invalid, using default: ");
    Serial.println(error.c_str());
    qrConfig.promptPayId = DEFAULT_PROMPTPAY_ID;
  }

  const String savedWebhook = preferences.getString(PREFERENCES_WEBHOOK_KEY, DEFAULT_WEBHOOK_URL);
  error.clear();
  if (validateWebhookUrl(savedWebhook.c_str(), error)) {
    webhookUrl = savedWebhook.c_str();
  } else {
    Serial.print("Saved webhook URL is invalid, using default: ");
    Serial.println(error.c_str());
    webhookUrl = DEFAULT_WEBHOOK_URL;
  }

  const String savedHostname = preferences.getString(PREFERENCES_HOSTNAME_KEY, DEFAULT_MDNS_HOSTNAME);
  error.clear();
  if (validateHostname(savedHostname.c_str(), error)) {
    mdnsHostname = savedHostname.c_str();
  } else {
    Serial.print("Saved hostname is invalid, using default: ");
    Serial.println(error.c_str());
    mdnsHostname = DEFAULT_MDNS_HOSTNAME;
  }
}

void syncClock() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
  struct tm localTime;
  clockSynced = getLocalTime(&localTime, 5000);
  Serial.println(clockSynced ? "NTP time synchronized." : "NTP unavailable; using uptime fallback.");
}

void setEventTime(QrEvent& event) {
  event.createdAtMs = millis();
  const std::time_t now = std::time(nullptr);
  if (!clockSynced || now < 1700000000) {
    event.createdAt = "uptime:" + std::to_string(event.createdAtMs) + "ms";
    return;
  }

  struct tm localTime;
  localtime_r(&now, &localTime);
  char timestamp[32];
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S+07:00", &localTime);
  event.createdAt = timestamp;
  event.createdAtEpochMs = static_cast<uint64_t>(now) * 1000ULL;
}

void printEventToSerial(const QrEvent& event) {
  Serial.println();
  Serial.println("=== QR Payment Event ===");
  Serial.print("Mode: ");
  Serial.println(event.mode.c_str());
  Serial.print("PromptPay ID: ");
  Serial.println(event.promptPayId.c_str());
  Serial.print("Amount: ");
  Serial.println(event.amount.c_str());
  Serial.print("Reference: ");
  Serial.println(event.reference.c_str());
  Serial.print("Created at: ");
  Serial.println(event.createdAt.c_str());
  Serial.print("Webhook status: ");
  Serial.println(event.webhookStatus);
  Serial.print("Payload: ");
  Serial.println(event.payload.c_str());
  Serial.println("========================");
}

int postEvent(const QrEvent& event) {
  if (webhookUrl.empty() || WiFi.status() != WL_CONNECTED) {
    Serial.println("REST POST skipped: webhook URL is empty or WiFi is not connected.");
    return 0;
  }

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(webhookUrl.c_str());
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(eventToJson(event).c_str());
  Serial.print("REST POST status: ");
  Serial.println(status);
  http.end();
  return status;
}

void saveLog(const QrEvent& event) {
  if (logs.size() >= MAX_LOGS) {
    logs.pop_front();
  }
  logs.push_back(event);
}

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

void sendFillToDisplay(int x, int y, int width, int height, int color) {
  if (!displayReady) {
    return;
  }
  const std::string command = buildHmiFillCommand(x, y, width, height, color);
  Serial2.write(reinterpret_cast<const uint8_t*>(command.data()), command.size());
}

void clearQrArea() {
  sendFillToDisplay(QR_AREA_X, QR_AREA_Y, QR_AREA_SIZE, QR_AREA_SIZE, 65535);
}

void sendQrToDisplay(const std::string& payload) {
  if (payload.empty()) {
    clearQrArea();
    return;
  }

  QRCode qrcode;
  std::vector<uint8_t> buffer;
  if (!buildQrCode(payload, qrcode, buffer)) {
    sendToDisplay("t0", "QR too large");
    clearQrArea();
    return;
  }

  clearQrArea();
  const int scale = std::max(1, QR_AREA_SIZE / qrcode.size);
  for (uint8_t y = 0; y < qrcode.size; ++y) {
    uint8_t x = 0;
    while (x < qrcode.size) {
      if (!qrcode_getModule(&qrcode, x, y)) {
        ++x;
        continue;
      }
      const uint8_t runStart = x;
      while (x < qrcode.size && qrcode_getModule(&qrcode, x, y)) {
        ++x;
      }
      const int runLength = x - runStart;
      sendFillToDisplay(QR_AREA_X + runStart * scale, QR_AREA_Y + y * scale,
                         runLength * scale, scale, 0);
    }
  }
}

void renderEventToOled(const QrEvent& event) {
  sendToDisplay("t0", "PromptPay");
  sendToDisplay("t1", event.mode == "dynamic" ? "Dynamic" : "Static");
  sendToDisplay("t2", event.amount.empty() ? "No amount" : event.amount);
  sendToDisplay("t3", event.reference);
  sendQrToDisplay(event.payload);
}

void renderPaymentConfirmed(const std::string& amount, const std::string& reference) {
  sendToDisplay("t0", "PAID");
  sendToDisplay("t1", "THB " + amount);
  sendToDisplay("t2", reference);
  sendToDisplay("t3", "");
  clearQrArea();
}

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
  clearQrArea();
}

void renderPpointsExpired(const std::string& total, const std::string& count) {
  sendToDisplay("t0", "P-Points");
  sendToDisplay("t1", "QR expired");
  sendToDisplay("t2", "Total " + total);
  sendToDisplay("t3", "Count " + count);
  clearQrArea();
}

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

double extractAmountFromText(const String& text) {
  String candidate;
  bool hasDigit = false;
  bool hasDecimal = false;

  for (int i = 0; i < text.length(); ++i) {
    const char ch = text.charAt(i);
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      candidate += ch;
      hasDigit = true;
    } else if ((ch == '.' || ch == ',') && hasDigit && !hasDecimal) {
      candidate += '.';
      hasDecimal = true;
    } else if (hasDigit) {
      break;
    }
  }

  if (!hasDigit) {
    return 0.0;
  }
  return candidate.toDouble();
}

int wholeBahtFromAmount(const std::string& amount) {
  const double value = String(amount.c_str()).toDouble();
  if (value <= 0.0) {
    return 0;
  }
  return std::min(static_cast<int>(value), MAX_PAYMENT_PULSES);
}

void enqueuePulsePin(int pin, int count) {
  const int safeCount = std::min(std::max(count, 0), MAX_PAYMENT_PULSES);
  for (int i = 0; i < safeCount; ++i) {
    paymentPulseQueue.push_back(pin);
  }
}

void startPaymentPulsesForAmount(const std::string& amount) {
  paymentPulseQueue.clear();
  digitalWrite(PAYMENT_PULSE_PIN, LOW);
  digitalWrite(PAYMENT_PULSE_TENS_PIN, LOW);
  paymentPulseHigh = false;
  paymentPulsesCompleted = 0;
  paymentPulsesOnes = 0;
  paymentPulsesTens = 0;

  const int wholeBaht = wholeBahtFromAmount(amount);
  if (twoPinPulseMode) {
    int remainder = wholeBaht;
    if (pulseDivisorTens > 0) {
      paymentPulsesTens = remainder / pulseDivisorTens;
      remainder -= paymentPulsesTens * pulseDivisorTens;
    }
    paymentPulsesOnes = pulseDivisorOnes > 0 ? remainder / pulseDivisorOnes : 0;
    enqueuePulsePin(PAYMENT_PULSE_TENS_PIN, paymentPulsesTens);
    enqueuePulsePin(PAYMENT_PULSE_PIN, paymentPulsesOnes);
  } else {
    paymentPulsesOnes = pulseDivisorOnes > 0 ? wholeBaht / pulseDivisorOnes : 0;
    enqueuePulsePin(PAYMENT_PULSE_PIN, paymentPulsesOnes);
  }

  paymentPulsesTotal = static_cast<int>(paymentPulseQueue.size());
  paymentPulsesPending = paymentPulsesTotal;
  if (paymentPulsesPending <= 0) {
    paymentPulseActive = false;
    return;
  }

  digitalWrite(PAYMENT_PULSE_PIN, LOW);
  digitalWrite(PAYMENT_PULSE_TENS_PIN, LOW);
  paymentPulseActive = true;
  paymentPulseNextAt = millis();

  Serial.print("Pulse mode: ");
  Serial.println(twoPinPulseMode ? "2-pin (ones + tens)" : "1-pin (1 baht/pulse)");
  Serial.print("Pulse queued total: ");
  Serial.println(paymentPulsesTotal);
  Serial.print("Pulse ones GPIO ");
  Serial.print(PAYMENT_PULSE_PIN);
  Serial.print(": ");
  Serial.println(paymentPulsesOnes);
  Serial.print("Pulse tens GPIO ");
  Serial.print(PAYMENT_PULSE_TENS_PIN);
  Serial.print(": ");
  Serial.println(paymentPulsesTens);
}

void processPaymentPulses() {
  if (!paymentPulseActive || static_cast<long>(millis() - paymentPulseNextAt) < 0) {
    return;
  }

  if (paymentPulseHigh) {
    digitalWrite(paymentPulseCurrentPin, LOW);
    paymentPulseHigh = false;
    ++paymentPulsesCompleted;

    if (paymentPulseQueue.empty()) {
      paymentPulseActive = false;
      Serial.print("Pulse complete: ");
      Serial.println(paymentPulsesCompleted);
      return;
    }

    paymentPulseNextAt = millis() + pulseSpaceWidthMs;
    return;
  }

  paymentPulseCurrentPin = paymentPulseQueue.front();
  paymentPulseQueue.pop_front();
  digitalWrite(paymentPulseCurrentPin, HIGH);
  paymentPulseHigh = true;
  --paymentPulsesPending;
  paymentPulseNextAt = millis() + pulseWidthMs;
}

bool confirmPayment(const std::string& paymentId,
                    const std::string& amount,
                    const std::string& reference,
                    const std::string& source) {
  if (paymentId == lastPaymentId) {
    return false;
  }

  lastPaymentId = paymentId;
  lastPaymentAmount = amount;
  lastPaymentReference = reference;
  startPaymentPulsesForAmount(amount);

  Serial.println();
  Serial.println("=== PAYMENT CONFIRMED ===");
  Serial.print("Source: ");
  Serial.println(source.c_str());
  Serial.print("Payment ID: ");
  Serial.println(lastPaymentId.c_str());
  Serial.print("Amount: ");
  Serial.println(lastPaymentAmount.c_str());
  Serial.print("Reference: ");
  Serial.println(lastPaymentReference.c_str());
  Serial.print("Pulse GPIO ones: ");
  Serial.println(PAYMENT_PULSE_PIN);
  Serial.print("Pulse GPIO tens: ");
  Serial.println(PAYMENT_PULSE_TENS_PIN);
  Serial.print("Pulse count total: ");
  Serial.println(paymentPulsesTotal);
  Serial.println("=========================");
  renderPaymentConfirmed(lastPaymentAmount, lastPaymentReference);
  return true;
}

struct PpointsResult {
  bool ok = false;
  std::string status;
  std::string stationId;
  std::string bankId;
  std::string amount;
  std::string count;
  std::string raw;
};

std::string extractJsonStringValue(const std::string& text, const std::string& key) {
  const std::string quotedKey = "\"" + key + "\"";
  size_t pos = text.find(quotedKey);
  if (pos == std::string::npos) {
    pos = text.find(key);
  }
  if (pos == std::string::npos) {
    return "";
  }
  pos = text.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  ++pos;
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '"' || text[pos] == '\'')) {
    ++pos;
  }
  size_t end = pos;
  while (end < text.size() && text[end] != '"' && text[end] != '\'' && text[end] != ',' && text[end] != '}' &&
         text[end] != ']') {
    ++end;
  }
  return text.substr(pos, end - pos);
}

PpointsResult parsePpointsResponse(const std::string& raw) {
  PpointsResult result;
  result.raw = raw;
  result.status = extractJsonStringValue(raw, "RES");
  if (result.status.empty()) {
    result.status = extractJsonStringValue(raw, "res");
  }
  std::transform(result.status.begin(), result.status.end(), result.status.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });

  result.stationId = extractJsonStringValue(raw, "stn_id");
  result.bankId = extractJsonStringValue(raw, "bank_id");

  const size_t amtPos = raw.find("amt");
  if (amtPos != std::string::npos) {
    const size_t bracketStart = raw.find('[', amtPos);
    const size_t bracketEnd = bracketStart == std::string::npos ? std::string::npos : raw.find(']', bracketStart);
    if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
      const std::string amtBlock = raw.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
      result.amount = formatMoney(extractAmountFromText(amtBlock.c_str()));
      const size_t comma = amtBlock.find(',');
      if (comma != std::string::npos) {
        result.count = extractJsonStringValue("{\"count\":" + amtBlock.substr(comma + 1) + "}", "count");
      }
    }
  }

  result.ok = result.status == "OK" && !result.amount.empty();
  return result;
}

std::string ppointsResultToJson(const PpointsResult& result) {
  std::string body = "{";
  body += "\"ok\":" + std::string(result.ok ? "true" : "false") + ",";
  body += "\"status\":\"" + jsonEscape(result.status) + "\",";
  body += "\"stationId\":\"" + jsonEscape(result.stationId) + "\",";
  body += "\"bankId\":\"" + jsonEscape(result.bankId) + "\",";
  body += "\"amount\":\"" + jsonEscape(result.amount) + "\",";
  body += "\"count\":\"" + jsonEscape(result.count) + "\",";
  body += "\"raw\":\"" + jsonEscape(result.raw) + "\"";
  body += "}";
  return body;
}

bool isPpointsSessionExpired() {
  return ppointsSessionActive && static_cast<long>(millis() - ppointsSessionExpiresAt) >= 0;
}

unsigned long ppointsSessionRemainingMs() {
  if (!ppointsSessionActive || isPpointsSessionExpired()) {
    return 0;
  }
  return ppointsSessionExpiresAt - millis();
}

void startPpointsSession() {
  ppointsSessionActive = true;
  ppointsSessionExpiresAt = millis() + PPOINTS_QR_TTL_MS;
}

std::string processPpointsResult(const PpointsResult& result,
                                 const std::string& stationId,
                                 const std::string& bankId,
                                 const std::string& source,
                                 bool& duplicate,
                                 bool& baseline,
                                 bool& pulseTriggered,
                                 std::string& deltaText) {
  duplicate = false;
  baseline = false;
  pulseTriggered = false;
  deltaText = "0.00";
  if (!result.ok) {
    return "";
  }

  const double currentTotal = String(result.amount.c_str()).toDouble();
  const std::string count = result.count.empty() ? std::to_string(millis()) : result.count;
  if (isPpointsSessionExpired()) {
    renderPpointsExpired(result.amount, count);
    return "ppoints-expired-" + stationId + "-" + bankId + "-" + count;
  }

  if (!hasPpointsPreviousTotal) {
    hasPpointsPreviousTotal = true;
    lastPpointsTotalAmount = currentTotal;
    lastPpointsCount = count;
    startPpointsSession();
    baseline = true;
    return "ppoints-baseline-" + stationId + "-" + bankId + "-" + count;
  }

  const bool isNewRecord = count != lastPpointsCount || result.amount != formatMoney(lastPpointsTotalAmount);
  char deltaBuffer[24];
  std::snprintf(deltaBuffer, sizeof(deltaBuffer), "%+.2f", currentTotal - lastPpointsTotalAmount);
  deltaText = deltaBuffer;

  const std::string paymentId = "ppoints-record-" + stationId + "-" + bankId + "-" + count + "-" + result.amount;
  const std::string reference = "PPOINTS-DIFF-" + count;
  if (paymentId == lastPaymentId) {
    duplicate = true;
    return paymentId;
  }

  lastPpointsTotalAmount = currentTotal;
  lastPpointsCount = count;

  if (isNewRecord && currentTotal > 0.0) {
    confirmPayment(paymentId, result.amount, reference, source);
    ppointsSessionActive = false;
    pulseTriggered = true;
  }
  return paymentId;
}

QrEvent createEvent(bool dynamicQr, double amount, const std::string& reference) {
  PromptPayPayload generated = buildPromptPayPayload(qrConfig, amount, reference, dynamicQr);
  QrEvent event;
  event.mode = dynamicQr ? "dynamic" : "static";
  event.payload = generated.payload;
  event.promptPayId = generated.normalizedTarget;
  event.amount = formatMoney(amount);
  event.reference = reference;
  setEventTime(event);
  event.webhookStatus = postEvent(event);
  saveLog(event);
  printEventToSerial(event);
  renderEventToOled(event);
  return event;
}

void sendJson(const std::string& body, int status = 200) {
  server.send(status, "application/json", body.c_str());
}

std::string qrSvgFromText(const std::string& text) {
  QRCode qrcode;
  std::vector<uint8_t> buffer;

  if (!buildQrCode(text, qrcode, buffer)) {
    return "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 300 300'>"
           "<rect width='300' height='300' fill='white'/>"
           "<text x='150' y='150' text-anchor='middle' font-size='16' fill='red'>QR too large</text>"
           "</svg>";
  }

  constexpr int quiet = 4;
  const int size = qrcode.size + quiet * 2;
  std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + std::to_string(size) +
                    " " + std::to_string(size) + "' shape-rendering='crispEdges'>";
  svg += "<rect width='100%' height='100%' fill='white'/>";

  for (uint8_t y = 0; y < qrcode.size; ++y) {
    for (uint8_t x = 0; x < qrcode.size; ++x) {
      if (qrcode_getModule(&qrcode, x, y)) {
        svg += "<rect x='" + std::to_string(x + quiet) + "' y='" + std::to_string(y + quiet) +
               "' width='1' height='1' fill='black'/>";
      }
    }
  }

  svg += "</svg>";
  return svg;
}

std::string htmlEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

void handleSetup() {
  std::string message;
  int status = 200;

  if (server.method() == HTTP_POST) {
    const String requestedSsid = server.arg("ssid");
    const String requestedPassword = server.arg("password");
    const std::string requestedWebhook = server.arg("webhook").c_str();
    const std::string requestedHostname = server.arg("hostname").c_str();
    std::string error;

    if (requestedSsid.isEmpty() || requestedSsid.length() > 32) {
      error = "WiFi SSID is required and must not exceed 32 characters";
    } else if (requestedPassword.length() > 63) {
      error = "WiFi password must not exceed 63 characters";
    } else if (!validateWebhookUrl(requestedWebhook, error)) {
      // Validation error is already populated.
    } else if (!validateHostname(requestedHostname, error)) {
      // Validation error is already populated.
    }

    if (!error.empty()) {
      message = "<p class='error'>" + htmlEscape(error) + "</p>";
      status = 400;
    } else {
      if (requestedSsid != wifiSsid || !requestedPassword.isEmpty()) {
        wifiPassword = requestedPassword;
      }
      wifiSsid = requestedSsid;
      webhookUrl = requestedWebhook;
      mdnsHostname = requestedHostname;
      preferences.putString(PREFERENCES_WIFI_SSID_KEY, wifiSsid);
      preferences.putString(PREFERENCES_WIFI_PASSWORD_KEY, wifiPassword);
      preferences.putString(PREFERENCES_WEBHOOK_KEY, webhookUrl.c_str());
      preferences.putString(PREFERENCES_HOSTNAME_KEY, mdnsHostname.c_str());

      const std::string savedPage =
          "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<style>body{font-family:Arial,sans-serif;max-width:600px;margin:40px auto;padding:20px}</style>"
          "<h1>Settings saved</h1><p>ESP32 is restarting and will connect to the configured WiFi.</p>"
          "<p>Reconnect your phone or computer to the same WiFi, then open <b>http://" +
          mdnsHostname + ".local</b>.</p>";
      server.send(200, "text/html; charset=utf-8", savedPage.c_str());
      delay(900);
      ESP.restart();
      return;
    }
  }

  const std::string currentSsid = htmlEscape(wifiSsid.c_str());
  const std::string currentWebhook = htmlEscape(webhookUrl);
  const std::string currentHostname = htmlEscape(mdnsHostname);
  std::string html =
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>PaymentESP Setup</title><style>body{font-family:Arial,sans-serif;max-width:620px;margin:24px auto;padding:0 16px;line-height:1.45;background:#f5f7f8;color:#17202a}"
      ".panel{background:white;border:1px solid #d8dee4;padding:18px;border-radius:8px}label{display:block;font-weight:700;margin-top:12px}"
      "input,button{font:inherit;width:100%;box-sizing:border-box;padding:11px;margin-top:5px}button{margin-top:18px;background:#08766b;color:white;border:0;border-radius:6px;font-weight:700}"
      ".note{color:#52606d}.error{color:#a61b1b;font-weight:700}</style></head><body><h1>PaymentESP Hardware Setup</h1>";
  html += message;
  html += "<div class='panel'><form method='post' action='/setup'>"
          "<label>WiFi SSID</label><input name='ssid' maxlength='32' required value='" + currentSsid + "'>"
          "<label>WiFi Password</label><input name='password' type='password' maxlength='63' placeholder='Leave blank to keep the current password'>"
          "<label>Device Hostname</label><input name='hostname' maxlength='32' required value='" + currentHostname + "' placeholder='paymentesp'>"
          "<p class='note' style='margin-top:2px'>Access URL will be: http://" + currentHostname + ".local</p>"
          "<label>REST POST Webhook URL</label><input name='webhook' value='" + currentWebhook + "' placeholder='Optional: http://192.168.1.10:3001/api/webhook'>"
          "<button type='submit'>Save and restart ESP32</button></form></div>"
          "<p class='note'>Setup AP: PaymentESP-Setup | Password: paymentesp | Setup URL: http://192.168.4.1/setup</p>"
          "<p><a href='/'>Back to QR page</a></p></body></html>";
  server.send(status, "text/html; charset=utf-8", html.c_str());
}

void handleRoot() {
  const char* html =
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ESP32 PromptPay QR</title>"
      "<style>body{font-family:Arial,sans-serif;max-width:820px;margin:20px auto;padding:0 16px;line-height:1.45;background:#f7f7f8;color:#111}"
      "h1{margin:0 0 12px}.panel{background:white;border:1px solid #ddd;border-radius:8px;padding:16px;margin-bottom:14px}"
      "input,button,select{font:inherit;padding:11px;margin:4px 0;width:100%;box-sizing:border-box}"
      "button{cursor:pointer;border:0;border-radius:6px;background:#0f766e;color:white;font-weight:700}"
      "button.secondary{background:#334155}.qrwrap{display:flex;justify-content:center;align-items:center;min-height:330px}"
      "button.active{outline:3px solid #f59e0b;outline-offset:2px}.status{font-weight:700;color:#0f766e}"
      "#qr{width:min(330px,90vw);height:min(330px,90vw);background:white;border:10px solid white;box-shadow:0 4px 20px #0002}"
      "pre{white-space:pre-wrap;word-break:break-all;background:#f1f5f9;padding:12px;border-radius:6px;font-size:12px}"
      ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}@media(max-width:640px){.row{grid-template-columns:1fr}}</style>"
      "</head><body><h1>ESP32 PromptPay QR</h1>"
      "<div class='panel'><b id='network'>Loading network status...</b><br><a href='/setup'>Hardware / WiFi Setup</a></div>"
      "<div class='panel'><h2>GenQR</h2>"
      "<label>P-Points QR Payload</label><input id='ppPayload' value='00020101021129390016A000000677010111031500499907526116353037645802TH6304E345'>"
      "<label>Amount</label><input id='ppAmount' value='15.00' inputmode='decimal'>"
      "<div class='row'><button id='ppGen' type='button'>GenQR</button>"
      "<button id='ppStatic' type='button' class='secondary'>Static QR</button></div>"
      "<div class='row'><button id='ppApply' type='button' class='secondary'>Apply QR Changes</button>"
      "<button id='ppDynamic' type='button'>P-Points Dynamic QR</button></div>"
      "<button id='ppShow' type='button' class='secondary'>Show Current QR on OLED</button>"
      "<p id='ppAuto'>GenQR uses only P-Points QR Payload + Amount.</p>"
      "<p class='note'>GenQR only creates and displays QR. It does not change stn_id, bank_id, baseline, monitor, or pulse checking.</p></div>"
      "<div class='panel'><h2>P-Points Check / Monitor</h2>"
      "<label>stn_id</label><input id='ppStn' value='P-24001'>"
      "<label>bank_id</label><input id='ppBank' value='X-9786'>"
      "<label>Pulse Mode</label><select id='pulseMode'><option value='1pin'>1 pin: 1 baht / pulse</option><option value='2pin'>2 pins: A=ones, B=10 baht/pulse</option></select>"
      "<div class='row'><div><label>O/P#1 divisor (baht/pulse, 0-100)</label><input id='pulseDivisor1' type='number' min='0' max='100' value='1'></div>"
      "<div><label>O/P#2 divisor (baht/pulse, 0-10000)</label><input id='pulseDivisor2' type='number' min='0' max='10000' value='10'></div></div>"
      "<div class='row'><div><label>PulseWidth (ms, 1-200)</label><input id='pulseWidth' type='number' min='1' max='200' value='25'></div>"
      "<div><label>PulseSpaceWidth (ms, 1-200)</label><input id='pulseGap' type='number' min='1' max='200' value='25'></div></div>"
      "<button id='ppCheck' type='button'>Check P-Points Pay-in</button>"
      "<label>Polling Mode</label><select id='pollingMode'><option value='continuous'>1A: Poll continuously</option><option value='session'>1B: Poll until Start, stop on Timeout</option></select>"
      "<div class='row'><div><label>Polling Period (sec)</label><input id='pollingPeriod' type='number' min='5' value='5'></div>"
      "<div><label>Timeout (sec, mode 1B only)</label><input id='pollingTimeout' type='number' min='1' value='300'></div></div>"
      "<div class='row'><button id='monitorStart' type='button'>Start Monitor</button>"
      "<button id='monitorStop' type='button' class='secondary'>Stop Monitor</button></div>"
      "<p id='monitorStatus'>Monitor auto starting...</p>"
      "<p class='note'>Monitor checks P-Points by stn_id + bank_id every 5 seconds and sends pulse only when a new pay-in amount is detected.</p></div>"
      "<div class='panel'><p id='qrStatus' class='status'>QR idle</p></div>"
      "<div class='panel qrwrap'><img id='qr' alt='PromptPay QR'></div>"
      "<div class='panel'><pre id='out'>Ready</pre></div>"
      "<script>let latestPayload='';let latestQrMode='dynamic';let qrSeq=0;let ppPollTimer=0;let ppCountdownTimer=0;let ppExpiresAt=0;let ppCheckCount=0;let ppMaxChecks=60;function show(j){document.getElementById('out').textContent=JSON.stringify(j,null,2)}"
      "function setQrStatus(t){document.getElementById('qrStatus').textContent=t}"
      "function setPpActive(mode){document.getElementById('ppStatic').classList.toggle('active',mode==='static');document.getElementById('ppDynamic').classList.toggle('active',mode==='dynamic');document.getElementById('ppGen').classList.toggle('active',mode==='dynamic')}"
      "function placeholderQr(label){qrSeq++;let svg='<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"330\" height=\"330\"><rect width=\"330\" height=\"330\" fill=\"white\"/><rect x=\"10\" y=\"10\" width=\"310\" height=\"310\" fill=\"#f8fafc\" stroke=\"#0f766e\" stroke-width=\"6\"/><text x=\"165\" y=\"145\" font-family=\"Arial\" font-size=\"34\" font-weight=\"700\" text-anchor=\"middle\" fill=\"#0f172a\">'+label+'</text><text x=\"165\" y=\"188\" font-family=\"Arial\" font-size=\"18\" text-anchor=\"middle\" fill=\"#475569\">loading QR #'+qrSeq+'</text></svg>';let img=document.getElementById('qr');img.style.opacity='1';img.src='data:image/svg+xml;charset=utf-8,'+encodeURIComponent(svg);setQrStatus(label+' selected | loading #'+qrSeq)}"
      "function setQrImage(payload,label){latestPayload=payload;let img=document.getElementById('qr');img.style.opacity='1';img.src='/api/qr.svg?data='+encodeURIComponent(payload)+'&t='+Date.now()+'&seq='+qrSeq;setQrStatus(label+' | payload '+payload.length+' chars | crc '+payload.slice(-4)+' | #'+qrSeq)}"
      "async function loadConfig(){let r=await fetch('/api/config');let j=await r.json();document.getElementById('network').textContent=(j.setupMode?'Setup mode: ':'Ready: ')+(j.ip||'no IP')+' | '+(j.hostname||'paymentesp.local')}"
      "async function generatePpointsQr(mode){latestQrMode=mode;setPpActive(mode);placeholderQr('P-Points '+mode.toUpperCase());let p=new URLSearchParams({mode:mode,payload:document.getElementById('ppPayload').value});if(mode==='dynamic')p.set('amount',document.getElementById('ppAmount').value||'1.00');let r=await fetch('/api/ppoints/qr?'+p);let j=await r.json();show(j);if(j.payload){setQrImage(j.payload,'P-Points '+mode.toUpperCase()+(mode==='dynamic'?' THB '+(j.amount||document.getElementById('ppAmount').value):' no amount'))}return j}"
      "async function showCustomerQr(){if(!latestPayload)await generatePpointsQr('dynamic');let p=new URLSearchParams({stn_id:latestQrMode.toUpperCase(),bank_id:'GENQR',ref:'PPOINTS-'+latestQrMode.toUpperCase(),payload:latestPayload});let r=await fetch('/api/customer-qr?'+p);return await r.json()}"
      "async function setPpointsBaseline(){let p=new URLSearchParams({stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value});let r=await fetch('/api/ppoints/baseline?'+p);return await r.json()}"
      "async function savePulseMode(){let p=new URLSearchParams({mode:document.getElementById('pulseMode').value,divisor1:document.getElementById('pulseDivisor1').value,divisor2:document.getElementById('pulseDivisor2').value,width:document.getElementById('pulseWidth').value,gap:document.getElementById('pulseGap').value});let r=await fetch('/api/pulse/config?'+p);return await r.json()}"
      "function showMonitorStatus(j){document.getElementById('monitorStatus').textContent='Monitor '+(j.enabled?'ON':'OFF')+' | '+j.pollingMode+' | '+(j.intervalMs/1000)+'s | next '+Math.ceil((j.nextInMs||0)/1000)+'s'+(j.pollingMode==='session'?' | timeout in '+Math.ceil((j.timeoutRemainingMs||0)/1000)+'s':'')+' | checks '+j.checks+' | pulse '+j.pulseMode+' O/P#1='+j.divisor1+' O/P#2='+j.divisor2+' width='+j.pulseWidthMs+'ms gap='+j.pulseSpaceWidthMs+'ms | last '+(j.lastTotal||'-')+(j.lastError?' | error '+j.lastError:'')}"
      "async function monitor(action){let p=new URLSearchParams({action:action,stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value,interval_ms:String((Number(document.getElementById('pollingPeriod').value)||5)*1000),polling_mode:document.getElementById('pollingMode').value,timeout_ms:String((Number(document.getElementById('pollingTimeout').value)||300)*1000)});let pulse=await savePulseMode();let r=await fetch('/api/ppoints/monitor?'+p);let j=await r.json();showMonitorStatus(j);show({pulse:pulse,monitor:j});return j}"
      "async function refreshMonitor(){let r=await fetch('/api/ppoints/monitor?action=status');let j=await r.json();showMonitorStatus(j);return j}"
      "async function applyQrChanges(){let qr=await generatePpointsQr(latestQrMode==='static'?'static':'dynamic');let oled=await showCustomerQr();show({qr:qr,oled:oled,note:'P-Points QR updated after pressing Apply QR Changes.'})}"
      "let ppFieldTimer=0;function schedulePpointsApply(kind){clearTimeout(ppFieldTimer);ppFieldTimer=setTimeout(async()=>{try{if(kind==='pulse'){show({pulse:await savePulseMode(),note:'Pulse mode updated immediately.'});return}if(kind==='monitor'){let j=await monitor('start');show({monitor:j,note:'P-Points station/bank updated. Baseline reset.'});return}setQrStatus('QR settings changed. Press Apply QR Changes.');show({pending:true,note:'Amount/Payload changed. Press Apply QR Changes to regenerate QR.'})}catch(e){show({error:e.message})}},500)}"
      "function stopPpointsAuto(msg){if(ppPollTimer)clearInterval(ppPollTimer);if(ppCountdownTimer)clearInterval(ppCountdownTimer);ppPollTimer=0;ppCountdownTimer=0;ppExpiresAt=0;if(msg)document.getElementById('ppAuto').textContent=msg}"
      "function updatePpointsCountdown(){let ms=ppExpiresAt-Date.now();if(ms<=0){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.');return}document.getElementById('ppAuto').textContent='Auto checking '+latestQrMode.toUpperCase()+' | check '+ppCheckCount+'/'+ppMaxChecks+' | expires in '+Math.ceil(ms/1000)+'s'}"
      "async function checkPpoints(auto=false){let p=new URLSearchParams({stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value});let r=await fetch('/api/ppoints/check?'+p);let j=await r.json();if(!auto)show(j);return j}"
      "async function pollPpoints(){if(!ppExpiresAt||Date.now()>=ppExpiresAt){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.');show({ok:false,expired:true,note:'QR session expired after 5 minutes.'});return}ppCheckCount++;updatePpointsCountdown();let j=await checkPpoints(true);show(Object.assign({autoCheck:true,checkCount:ppCheckCount,maxChecks:ppMaxChecks,qrMode:latestQrMode},j));if(j.pulseTriggered||Number(j.delta)>0){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('Payment detected. Pulse sent.');show(Object.assign({autoCheck:true,checkCount:ppCheckCount,note:'Payment detected. Pulse sent.'},j))}else if(j.sessionExpired){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.')}}"
      "function startPpointsAuto(baseline){stopPpointsAuto();ppCheckCount=0;ppMaxChecks=Math.max(1,Math.ceil((baseline.expiresInMs||300000)/(baseline.pollIntervalMs||5000)));ppExpiresAt=Date.now()+(baseline.expiresInMs||300000);updatePpointsCountdown();ppCountdownTimer=setInterval(updatePpointsCountdown,1000);ppPollTimer=setInterval(()=>pollPpoints().catch(e=>show({autoCheck:true,error:e.message})),baseline.pollIntervalMs||5000)}"
      "async function preparePpointsQr(mode){let qr=await generatePpointsQr(mode);let oled=await showCustomerQr();stopPpointsAuto('QR displayed. Monitor keeps checking separately.');show({ok:!!qr.payload,qr:qr,oled:oled,amount:mode==='dynamic'?document.getElementById('ppAmount').value:'static-no-amount',note:'GenQR uses only payload and amount. Monitor/pulse checking is separate.'})}"
      "document.getElementById('ppGen').onclick=async()=>preparePpointsQr('dynamic');"
      "document.getElementById('ppStatic').onclick=async()=>preparePpointsQr('static');"
      "document.getElementById('ppDynamic').onclick=async()=>preparePpointsQr('dynamic');"
      "document.getElementById('ppShow').onclick=async()=>show(await showCustomerQr());"
      "document.getElementById('ppApply').onclick=()=>applyQrChanges();"
      "document.getElementById('ppCheck').onclick=()=>checkPpoints(false);"
      "document.getElementById('monitorStart').onclick=()=>monitor('start');"
      "document.getElementById('monitorStop').onclick=()=>monitor('stop');"
      "document.getElementById('ppAmount').oninput=()=>schedulePpointsApply('qr');"
      "document.getElementById('ppPayload').oninput=()=>schedulePpointsApply('qr');"
      "document.getElementById('ppStn').oninput=()=>schedulePpointsApply('monitor');"
      "document.getElementById('ppBank').oninput=()=>schedulePpointsApply('monitor');"
      "document.getElementById('pollingMode').onchange=()=>schedulePpointsApply('monitor');"
      "document.getElementById('pollingPeriod').onchange=()=>schedulePpointsApply('monitor');"
      "document.getElementById('pollingTimeout').onchange=()=>schedulePpointsApply('monitor');"
      "document.getElementById('pulseMode').onchange=()=>schedulePpointsApply('pulse');"
      "document.getElementById('pulseDivisor1').onchange=()=>schedulePpointsApply('pulse');"
      "document.getElementById('pulseDivisor2').onchange=()=>schedulePpointsApply('pulse');"
      "document.getElementById('pulseWidth').onchange=()=>schedulePpointsApply('pulse');"
      "document.getElementById('pulseGap').onchange=()=>schedulePpointsApply('pulse');"
      "loadConfig().then(()=>{setQrStatus('Choose P-Points Static or Dynamic QR');refreshMonitor();setInterval(()=>refreshMonitor().catch(()=>{}),5000)});</script>"
      "</body></html>";
  server.send(200, "text/html", html);
}

void handleStaticQr() {
  std::string error;
  if (!applyPromptPayFromRequest(error)) {
    sendJson("{\"error\":\"" + jsonEscape(error) + "\"}", 400);
    return;
  }

  const std::string reference = argOrDefault("ref", "STATIC").c_str();
  try {
    QrEvent event = createEvent(false, 0.0, reference);
    sendJson(eventToJson(event));
  } catch (const std::exception& ex) {
    sendJson("{\"error\":\"" + jsonEscape(ex.what()) + "\"}", 400);
  }
}

void handleDynamicQr() {
  std::string error;
  if (!applyPromptPayFromRequest(error)) {
    sendJson("{\"error\":\"" + jsonEscape(error) + "\"}", 400);
    return;
  }

  const double amount = argOrDefault("amount", "1.00").toDouble();
  const std::string reference = argOrDefault("ref", "DYNAMIC").c_str();
  if (amount <= 0.0) {
    sendJson("{\"error\":\"amount must be greater than 0\"}", 400);
    return;
  }
  try {
    QrEvent event = createEvent(true, amount, reference);
    sendJson(eventToJson(event));
  } catch (const std::exception& ex) {
    sendJson("{\"error\":\"" + jsonEscape(ex.what()) + "\"}", 400);
  }
}

void handlePpointsQr() {
  const std::string mode = argOrDefault("mode", "static").c_str();
  const bool dynamicQr = mode == "dynamic";
  const bool staticQr = mode == "static";
  if (!dynamicQr && !staticQr) {
    sendJson("{\"error\":\"mode must be static or dynamic\"}", 400);
    return;
  }

  const std::string templatePayload = argOrDefault("payload", DEFAULT_PPOINTS_QR_PAYLOAD).c_str();
  const double amount = argOrDefault("amount", "0").toDouble();
  std::string payload;
  std::string error;
  if (!buildPpointsPayloadFromTemplate(templatePayload, amount, dynamicQr, payload, error)) {
    sendJson("{\"error\":\"" + jsonEscape(error) + "\"}", 400);
    return;
  }

  const std::string stationId = argOrDefault("stn_id", dynamicQr ? "DYNAMIC" : "STATIC").c_str();
  const std::string bankId = argOrDefault("bank_id", "GENQR").c_str();
  renderCustomerQrStatus(bankId, stationId, dynamicQr ? "PPOINTS-DYNAMIC" : "PPOINTS-STATIC", payload);

  std::string body = "{";
  body += "\"mode\":\"ppoints-" + std::string(dynamicQr ? "dynamic" : "static") + "\",";
  body += "\"source\":\"p-points-template\",";
  body += "\"amount\":\"" + jsonEscape(dynamicQr ? formatMoney(amount) : "") + "\",";
  body += "\"oledRendered\":true,";
  body += "\"templateCrcValid\":" + std::string(hasValidPromptPayCrc(templatePayload) ? "true" : "false") + ",";
  body += "\"payload\":\"" + jsonEscape(payload) + "\"";
  body += "}";
  sendJson(body);
}

void handleLogs() {
  std::vector<QrEvent> copy(logs.begin(), logs.end());
  sendJson(eventsToJson(copy));
}

void handleQrSvg() {
  std::string data = argOrDefault("data", "").c_str();
  if (data.empty()) {
    const PromptPayPayload payload = buildPromptPayPayload(qrConfig, 99.0, "ORDER-0001", true);
    data = payload.payload;
  }
  server.send(200, "image/svg+xml", qrSvgFromText(data).c_str());
}

void handleConfig() {
  if (server.method() == HTTP_POST) {
    std::string error;
    if (!applyPromptPayFromRequest(error, true) || !applyWebhookFromRequest(error, true)) {
      sendJson("{\"error\":\"" + jsonEscape(error) + "\"}", 400);
      return;
    }
  }

  std::string body = "{";
  body += "\"promptPayId\":\"" + jsonEscape(qrConfig.promptPayId) + "\",";
  body += "\"merchantName\":\"" + jsonEscape(qrConfig.merchantName) + "\",";
  body += "\"city\":\"" + jsonEscape(qrConfig.city) + "\",";
  body += "\"webhookUrl\":\"" + jsonEscape(webhookUrl) + "\",";
  body += "\"clockSynced\":" + std::string(clockSynced ? "true" : "false") + ",";
  body += "\"wifiConnected\":" + std::string(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  body += "\"setupMode\":" + std::string(setupMode ? "true" : "false") + ",";
  body += "\"ssid\":\"" + jsonEscape(wifiSsid.c_str()) + "\",";
  body += "\"ip\":\"" + std::string((setupMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str()) + "\",";
  body += "\"hostname\":\"http://" + mdnsHostname + ".local\"";
  body += "}";
  sendJson(body);
}

void handlePaymentConfirmation() {
  if (server.method() == HTTP_GET) {
    std::string body = "{";
    body += "\"paymentId\":\"" + jsonEscape(lastPaymentId) + "\",";
    body += "\"amount\":\"" + jsonEscape(lastPaymentAmount) + "\",";
    body += "\"reference\":\"" + jsonEscape(lastPaymentReference) + "\",";
    body += "\"pulseActive\":" + std::string(paymentPulseActive ? "true" : "false") + ",";
    body += "\"pulseTotal\":" + std::to_string(paymentPulsesTotal) + ",";
    body += "\"pulseCompleted\":" + std::to_string(paymentPulsesCompleted);
    body += "}";
    sendJson(body);
    return;
  }

  const std::string paymentId = argOrDefault("paymentId", "").c_str();
  const std::string status = argOrDefault("status", "").c_str();
  const std::string amount = argOrDefault("amount", "").c_str();
  const std::string reference = argOrDefault("ref", "").c_str();
  if (paymentId.empty() || status != "paid" || amount.empty()) {
    sendJson("{\"error\":\"paymentId, status=paid and amount are required\"}", 400);
    return;
  }
  if (paymentId == lastPaymentId) {
    sendJson("{\"ok\":true,\"duplicate\":true,\"pulseTriggered\":false}");
    return;
  }

  confirmPayment(paymentId, amount, reference, "REST API");
  sendJson("{\"ok\":true,\"duplicate\":false,\"pulseTriggered\":true}");
}

void handleCustomerQr() {
  const std::string bankId = argOrDefault("bank_id", "X-9786").c_str();
  const std::string stationId = argOrDefault("stn_id", "P-24001").c_str();
  const std::string reference = argOrDefault("ref", "CUSTOMER-QR").c_str();
  const std::string payload = argOrDefault("payload", "").c_str();

  renderCustomerQrStatus(bankId, stationId, reference, payload);

  Serial.println();
  Serial.println("=== CUSTOMER QR DISPLAY ===");
  Serial.print("Bank ID: ");
  Serial.println(bankId.c_str());
  Serial.print("Station ID: ");
  Serial.println(stationId.c_str());
  Serial.print("Reference: ");
  Serial.println(reference.c_str());
  Serial.print("Payload source: ");
  Serial.println(payload.empty() ? "external image/static customer QR" : "payload");
  Serial.println("===========================");

  std::string body = "{";
  body += "\"ok\":true,";
  body += "\"mode\":\"customer-qr\",";
  body += "\"bankId\":\"" + jsonEscape(bankId) + "\",";
  body += "\"stationId\":\"" + jsonEscape(stationId) + "\",";
  body += "\"reference\":\"" + jsonEscape(reference) + "\"";
  body += "}";
  sendJson(body);
}

void sendPpointsPaymentResponse(const PpointsResult& parsed,
                                const std::string& stationId,
                                const std::string& bankId,
                                const std::string& source) {
  bool duplicate = false;
  bool baseline = false;
  bool pulseTriggered = false;
  std::string deltaText;
  const std::string paymentId =
      processPpointsResult(parsed, stationId, bankId, source, duplicate, baseline, pulseTriggered, deltaText);

  std::string body = "{";
  body += "\"ok\":" + std::string(parsed.ok ? "true" : "false") + ",";
  body += "\"source\":\"" + jsonEscape(source) + "\",";
  body += "\"paymentId\":\"" + jsonEscape(paymentId) + "\",";
  body += "\"duplicate\":" + std::string(duplicate ? "true" : "false") + ",";
  body += "\"baseline\":" + std::string(baseline ? "true" : "false") + ",";
  body += "\"delta\":\"" + jsonEscape(deltaText) + "\",";
  body += "\"previousTotal\":\"" + jsonEscape(formatMoney(lastPpointsTotalAmount)) + "\",";
  body += "\"pulseTriggered\":" + std::string(pulseTriggered ? "true" : "false") + ",";
  body += "\"pulseTotal\":" + std::to_string(paymentPulsesTotal) + ",";
  body += "\"sessionActive\":" + std::string(ppointsSessionActive ? "true" : "false") + ",";
  body += "\"sessionExpired\":" + std::string(isPpointsSessionExpired() ? "true" : "false") + ",";
  body += "\"expiresInMs\":" + std::to_string(ppointsSessionRemainingMs()) + ",";
  body += "\"parsed\":" + ppointsResultToJson(parsed);
  body += "}";
  sendJson(body, parsed.ok ? 200 : 202);
}

bool fetchPpointsResult(const std::string& stationId,
                        const std::string& bankId,
                        PpointsResult& parsed,
                        int& status,
                        std::string& error) {
  if (WiFi.status() != WL_CONNECTED) {
    error = "WiFi is not connected; cannot call P-Points";
    return false;
  }

  std::string url = std::string(PPOINTS_BASE_URL) + "?stn_id=" + stationId + "&bank_id=" + bankId + "&flg=W";
  WiFiClientSecure client;
  client.setInsecure();
  // WiFiClientSecure defaults to a 120s TLS handshake timeout; left unset, a single
  // stalled handshake to p-points.com freezes the whole loop() (web server, WiFi
  // reconnect, everything) for up to 2 minutes at a time. Bound it to match the
  // HTTPClient timeout below so a bad network blip can't lock up the board.
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url.c_str())) {
    error = "failed to start P-Points request";
    status = 0;
    return false;
  }

  status = http.GET();
  const String bodyText = http.getString();
  http.end();

  if (status <= 0 || status >= 400) {
    error = bodyText.c_str();
    return false;
  }

  parsed = parsePpointsResponse(bodyText.c_str());
  if (parsed.stationId.empty()) parsed.stationId = stationId;
  if (parsed.bankId.empty()) parsed.bankId = bankId;
  return true;
}

void handlePpointsBaseline() {
  const std::string stationId = argOrDefault("stn_id", DEFAULT_PPOINTS_STATION_ID).c_str();
  const std::string bankId = argOrDefault("bank_id", DEFAULT_PPOINTS_BANK_ID).c_str();

  PpointsResult parsed;
  int status = 0;
  std::string error;
  if (!fetchPpointsResult(stationId, bankId, parsed, status, error)) {
    std::string body = "{";
    body += "\"ok\":false,";
    body += "\"error\":\"" + jsonEscape(error) + "\",";
    body += "\"httpStatus\":" + std::to_string(status);
    body += "}";
    sendJson(body, WiFi.status() == WL_CONNECTED ? 502 : 503);
    return;
  }

  if (!parsed.ok) {
    std::string body = "{";
    body += "\"ok\":false,";
    body += "\"baseline\":false,";
    body += "\"note\":\"P-Points did not return a payable amount\",";
    body += "\"parsed\":" + ppointsResultToJson(parsed);
    body += "}";
    sendJson(body, 202);
    return;
  }

  const std::string count = parsed.count.empty() ? std::to_string(millis()) : parsed.count;
  lastPpointsTotalAmount = String(parsed.amount.c_str()).toDouble();
  lastPpointsCount = count;
  hasPpointsPreviousTotal = true;
  startPpointsSession();

  Serial.println();
  Serial.println("=== P-POINTS BASELINE ===");
  Serial.print("Station ID: ");
  Serial.println(stationId.c_str());
  Serial.print("Bank ID: ");
  Serial.println(bankId.c_str());
  Serial.print("Baseline total: ");
  Serial.println(parsed.amount.c_str());
  Serial.print("Count: ");
  Serial.println(count.c_str());
  Serial.println("=========================");

  std::string body = "{";
  body += "\"ok\":true,";
  body += "\"source\":\"ESP32 P-Points API\",";
  body += "\"baseline\":true,";
  body += "\"total\":\"" + jsonEscape(parsed.amount) + "\",";
  body += "\"count\":\"" + jsonEscape(count) + "\",";
  body += "\"ttlMs\":" + std::to_string(PPOINTS_QR_TTL_MS) + ",";
  body += "\"pollIntervalMs\":" + std::to_string(PPOINTS_POLL_INTERVAL_MS) + ",";
  body += "\"expiresInMs\":" + std::to_string(ppointsSessionRemainingMs()) + ",";
  body += "\"note\":\"Baseline saved. Scan QR, then call /api/ppoints/check to compare the new total.\",";
  body += "\"parsed\":" + ppointsResultToJson(parsed);
  body += "}";
  sendJson(body);
}

void handlePpointsMock() {
  const std::string stationId = argOrDefault("stn_id", DEFAULT_PPOINTS_STATION_ID).c_str();
  const std::string bankId = argOrDefault("bank_id", DEFAULT_PPOINTS_BANK_ID).c_str();
  const std::string amount = argOrDefault("amount", "13.03").c_str();
  const std::string count = argOrDefault("count", String(millis())).c_str();

  const std::string raw = "{\"RES\":\"OK\",\"stn_id\":\"" + jsonEscape(stationId) + "\",\"bank_id\":\"" +
                          jsonEscape(bankId) + "\",\"amt\":[\"" + jsonEscape(amount) + "\",\"" +
                          jsonEscape(count) + "\"]}";
  PpointsResult parsed = parsePpointsResponse(raw);
  if (parsed.stationId.empty()) parsed.stationId = stationId;
  if (parsed.bankId.empty()) parsed.bankId = bankId;
  sendPpointsPaymentResponse(parsed, stationId, bankId, "ESP32 P-Points Mock");
}

void handlePpointsCheck() {
  const std::string stationId = argOrDefault("stn_id", String(ppointsMonitorStationId.c_str())).c_str();
  const std::string bankId = argOrDefault("bank_id", String(ppointsMonitorBankId.c_str())).c_str();

  PpointsResult parsed;
  int status = 0;
  std::string error;
  if (!fetchPpointsResult(stationId, bankId, parsed, status, error)) {
    std::string body = "{";
    body += "\"error\":\"P-Points request failed\",";
    body += "\"httpStatus\":" + std::to_string(status) + ",";
    body += "\"raw\":\"" + jsonEscape(error) + "\"";
    body += "}";
    sendJson(body, WiFi.status() == WL_CONNECTED ? 502 : 503);
    return;
  }

  sendPpointsPaymentResponse(parsed, stationId, bankId, "ESP32 P-Points API");
}

std::string processPpointsMonitorResult(const PpointsResult& result,
                                        const std::string& stationId,
                                        const std::string& bankId,
                                        bool& baseline,
                                        bool& pulseTriggered,
                                        std::string& deltaText) {
  baseline = false;
  pulseTriggered = false;
  deltaText = "0.00";
  if (!result.ok) {
    return "";
  }

  const double currentTotal = String(result.amount.c_str()).toDouble();
  const std::string count = result.count.empty() ? std::to_string(millis()) : result.count;
  if (!hasPpointsPreviousTotal) {
    hasPpointsPreviousTotal = true;
    lastPpointsTotalAmount = currentTotal;
    lastPpointsCount = count;
    baseline = true;
    renderPpointsDelta(result.amount, "+0.00", count, true, false);
    return "ppoints-monitor-baseline-" + stationId + "-" + bankId + "-" + count;
  }

  const bool isNewRecord = count != lastPpointsCount || result.amount != formatMoney(lastPpointsTotalAmount);
  char deltaBuffer[24];
  std::snprintf(deltaBuffer, sizeof(deltaBuffer), "%+.2f", currentTotal - lastPpointsTotalAmount);
  deltaText = deltaBuffer;

  lastPpointsTotalAmount = currentTotal;
  lastPpointsCount = count;

  const std::string paymentId = "ppoints-monitor-record-" + stationId + "-" + bankId + "-" + count + "-" + result.amount;
  if (isNewRecord && currentTotal > 0.0 && paymentId != lastPaymentId) {
    confirmPayment(paymentId, result.amount, "PPOINTS-MONITOR-" + count, "ESP32 P-Points 5min Monitor");
    pulseTriggered = true;
  }
  return paymentId;
}

std::string ppointsMonitorStatusJson() {
  const unsigned long nextInMs =
      ppointsMonitorEnabled && static_cast<long>(ppointsMonitorNextAt - millis()) > 0 ? ppointsMonitorNextAt - millis()
                                                                                       : 0;
  const unsigned long timeoutAt = ppointsMonitorStartedAt + ppointsMonitorTimeoutMs;
  const unsigned long timeoutRemainingMs = (!ppointsMonitorContinuous && ppointsMonitorEnabled &&
                                             static_cast<long>(timeoutAt - millis()) > 0)
                                                ? (timeoutAt - millis())
                                                : 0;
  std::string body = "{";
  body += "\"ok\":true,";
  body += "\"enabled\":" + std::string(ppointsMonitorEnabled ? "true" : "false") + ",";
  body += "\"pollingMode\":\"" + std::string(ppointsMonitorContinuous ? "continuous" : "session") + "\",";
  body += "\"timeoutMs\":" + std::to_string(ppointsMonitorTimeoutMs) + ",";
  body += "\"timeoutRemainingMs\":" + std::to_string(timeoutRemainingMs) + ",";
  body += "\"stationId\":\"" + jsonEscape(ppointsMonitorStationId) + "\",";
  body += "\"bankId\":\"" + jsonEscape(ppointsMonitorBankId) + "\",";
  body += "\"intervalMs\":" + std::to_string(ppointsMonitorIntervalMs) + ",";
  body += "\"nextInMs\":" + std::to_string(nextInMs) + ",";
  body += "\"checks\":" + std::to_string(ppointsMonitorChecks) + ",";
  body += "\"lastTotal\":\"" + jsonEscape(hasPpointsPreviousTotal ? formatMoney(lastPpointsTotalAmount) : "") + "\",";
  body += "\"lastCount\":\"" + jsonEscape(lastPpointsCount) + "\",";
  body += "\"lastError\":\"" + jsonEscape(ppointsMonitorLastError) + "\",";
  body += "\"pulseMode\":\"" + std::string(twoPinPulseMode ? "two-pin" : "one-pin") + "\",";
  body += "\"pulseOnesPin\":" + std::to_string(PAYMENT_PULSE_PIN) + ",";
  body += "\"pulseTensPin\":" + std::to_string(PAYMENT_PULSE_TENS_PIN) + ",";
  body += "\"divisor1\":" + std::to_string(pulseDivisorOnes) + ",";
  body += "\"divisor2\":" + std::to_string(pulseDivisorTens) + ",";
  body += "\"pulseWidthMs\":" + std::to_string(pulseWidthMs) + ",";
  body += "\"pulseSpaceWidthMs\":" + std::to_string(pulseSpaceWidthMs);
  body += "}";
  return body;
}

void handlePulseConfig() {
  if (server.hasArg("mode")) {
    const std::string mode = server.arg("mode").c_str();
    if (mode == "2pin" || mode == "two-pin") {
      twoPinPulseMode = true;
    } else if (mode == "1pin" || mode == "one-pin") {
      twoPinPulseMode = false;
    } else {
      sendJson("{\"error\":\"mode must be 1pin or 2pin\"}", 400);
      return;
    }
  }

  if (server.hasArg("divisor1")) {
    const int requested = server.arg("divisor1").toInt();
    if (requested < 0 || requested > MAX_PULSE_DIVISOR_ONES) {
      sendJson("{\"error\":\"divisor1 must be 0-" + std::to_string(MAX_PULSE_DIVISOR_ONES) + "\"}", 400);
      return;
    }
    pulseDivisorOnes = requested;
  }

  if (server.hasArg("divisor2")) {
    const int requested = server.arg("divisor2").toInt();
    if (requested < 0 || requested > MAX_PULSE_DIVISOR_TENS) {
      sendJson("{\"error\":\"divisor2 must be 0-" + std::to_string(MAX_PULSE_DIVISOR_TENS) + "\"}", 400);
      return;
    }
    pulseDivisorTens = requested;
  }

  if (server.hasArg("width")) {
    const long requested = server.arg("width").toInt();
    if (requested < 1 || requested > static_cast<long>(MAX_PULSE_WIDTH_MS)) {
      sendJson("{\"error\":\"width must be 1-" + std::to_string(MAX_PULSE_WIDTH_MS) + "\"}", 400);
      return;
    }
    pulseWidthMs = static_cast<unsigned long>(requested);
  }

  if (server.hasArg("gap")) {
    const long requested = server.arg("gap").toInt();
    if (requested < 1 || requested > static_cast<long>(MAX_PULSE_WIDTH_MS)) {
      sendJson("{\"error\":\"gap must be 1-" + std::to_string(MAX_PULSE_WIDTH_MS) + "\"}", 400);
      return;
    }
    pulseSpaceWidthMs = static_cast<unsigned long>(requested);
  }

  std::string body = "{";
  body += "\"ok\":true,";
  body += "\"pulseMode\":\"" + std::string(twoPinPulseMode ? "two-pin" : "one-pin") + "\",";
  body += "\"pulseOnesPin\":" + std::to_string(PAYMENT_PULSE_PIN) + ",";
  body += "\"pulseTensPin\":" + std::to_string(PAYMENT_PULSE_TENS_PIN) + ",";
  body += "\"divisor1\":" + std::to_string(pulseDivisorOnes) + ",";
  body += "\"divisor2\":" + std::to_string(pulseDivisorTens) + ",";
  body += "\"pulseWidthMs\":" + std::to_string(pulseWidthMs) + ",";
  body += "\"pulseSpaceWidthMs\":" + std::to_string(pulseSpaceWidthMs);
  body += "}";
  sendJson(body);
}

void handlePpointsMonitor() {
  const std::string action = argOrDefault("action", "status").c_str();
  if (action == "start") {
    ppointsMonitorStationId = argOrDefault("stn_id", DEFAULT_PPOINTS_STATION_ID).c_str();
    ppointsMonitorBankId = argOrDefault("bank_id", DEFAULT_PPOINTS_BANK_ID).c_str();
    const long requestedIntervalMs = argOrDefault("interval_ms", String(PPOINTS_MONITOR_INTERVAL_MS)).toInt();
    ppointsMonitorIntervalMs = static_cast<unsigned long>(std::max(5000L, requestedIntervalMs));

    const std::string pollingMode = argOrDefault("polling_mode", "continuous").c_str();
    ppointsMonitorContinuous = pollingMode != "session" && pollingMode != "1b";

    const long requestedTimeoutMs = argOrDefault("timeout_ms", String(PPOINTS_QR_TTL_MS)).toInt();
    ppointsMonitorTimeoutMs = static_cast<unsigned long>(std::max(1000L, requestedTimeoutMs));

    ppointsMonitorEnabled = true;
    ppointsMonitorStartedAt = millis();
    ppointsMonitorNextAt = millis();
    ppointsMonitorChecks = 0;
    ppointsMonitorLastError.clear();
    hasPpointsPreviousTotal = false;
    lastPpointsTotalAmount = 0.0;
    lastPpointsCount.clear();
    ppointsSessionActive = false;
    sendJson(ppointsMonitorStatusJson());
    return;
  }

  if (action == "stop") {
    ppointsMonitorEnabled = false;
    sendJson(ppointsMonitorStatusJson());
    return;
  }

  sendJson(ppointsMonitorStatusJson());
}

void processPpointsMonitor() {
  if (!ppointsMonitorEnabled) {
    return;
  }

  if (!ppointsMonitorContinuous &&
      static_cast<long>(millis() - (ppointsMonitorStartedAt + ppointsMonitorTimeoutMs)) >= 0) {
    ppointsMonitorEnabled = false;
    Serial.println("P-Points monitor stopped: polling timeout reached.");
    return;
  }

  if (static_cast<long>(millis() - ppointsMonitorNextAt) < 0) {
    return;
  }

  ppointsMonitorNextAt = millis() + ppointsMonitorIntervalMs;
  ++ppointsMonitorChecks;

  PpointsResult parsed;
  int status = 0;
  std::string error;
  if (!fetchPpointsResult(ppointsMonitorStationId, ppointsMonitorBankId, parsed, status, error)) {
    ppointsMonitorLastError = error.empty() ? "P-Points request failed" : error;
    Serial.print("P-Points monitor error: ");
    Serial.println(ppointsMonitorLastError.c_str());
    return;
  }

  bool baseline = false;
  bool pulseTriggered = false;
  std::string deltaText;
  const std::string paymentId = processPpointsMonitorResult(parsed, ppointsMonitorStationId, ppointsMonitorBankId,
                                                            baseline, pulseTriggered, deltaText);
  ppointsMonitorLastError.clear();

  Serial.println();
  Serial.println("=== P-POINTS MONITOR ===");
  Serial.print("Check: ");
  Serial.println(ppointsMonitorChecks);
  Serial.print("Payment ID: ");
  Serial.println(paymentId.c_str());
  Serial.print("Total: ");
  Serial.println(parsed.amount.c_str());
  Serial.print("Delta: ");
  Serial.println(deltaText.c_str());
  Serial.print("Baseline: ");
  Serial.println(baseline ? "yes" : "no");
  Serial.print("Pulse: ");
  Serial.println(pulseTriggered ? "yes" : "no");
  Serial.println("========================");
}

void handleNotFound() {
  sendJson("{\"error\":\"not found\"}", 404);
}

unsigned long mdnsRefreshNextAt = 0;

void connectWifi() {
  setupMode = false;
  if (!wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(mdnsHostname.c_str());
    Serial.print("Connecting to WiFi: ");
    Serial.println(wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    WiFi.setSleep(false);
    const unsigned long deadline = millis() + 20000;
    while (WiFi.status() != WL_CONNECTED && static_cast<long>(millis() - deadline) < 0) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(mdnsHostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      Serial.println(("mDNS ready: http://" + mdnsHostname + ".local").c_str());
    } else {
      Serial.println("mDNS unavailable; use the IP address shown above.");
    }
    // Windows doesn't resolve .local (mDNS) names without Bonjour installed, but it
    // does resolve NetBIOS names natively, so also answer to the plain hostname.
    NBNS.begin(mdnsHostname.c_str());
    mdnsRefreshNextAt = millis() + MDNS_REFRESH_INTERVAL_MS;
    return;
  }

  setupMode = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD)) {
    Serial.println("Failed to start setup access point.");
    return;
  }
  Serial.println("WiFi is not configured or could not connect.");
  Serial.print("Setup AP: ");
  Serial.println(SETUP_AP_SSID);
  Serial.print("Setup URL: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/setup");
}

unsigned long wifiReconnectNextAt = 0;

void processWifiReconnect() {
  if (setupMode || wifiSsid.isEmpty()) {
    return;
  }

  static bool wasConnected = true;
  const bool nowConnected = WiFi.status() == WL_CONNECTED;

  if (nowConnected) {
    if (!wasConnected) {
      Serial.print("WiFi reconnected. IP: ");
      Serial.println(WiFi.localIP());
      MDNS.end();
      if (MDNS.begin(mdnsHostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.println(("mDNS ready: http://" + mdnsHostname + ".local").c_str());
      } else {
        Serial.println("mDNS unavailable; use the IP address shown above.");
      }
      NBNS.begin(mdnsHostname.c_str());
      mdnsRefreshNextAt = millis() + MDNS_REFRESH_INTERVAL_MS;
    }
    wasConnected = true;
    return;
  }

  wasConnected = false;
  if (static_cast<long>(millis() - wifiReconnectNextAt) < 0) {
    return;
  }
  wifiReconnectNextAt = millis() + WIFI_RECONNECT_INTERVAL_MS;
  Serial.println("WiFi disconnected; attempting reconnect...");
  WiFi.reconnect();
}

// ESP32 Arduino's mDNS responder is known to silently stop answering
// paymentesp.local queries after running for a while even though WiFi
// itself stays connected; periodically restarting it works around that.
void processMdnsRefresh() {
  if (setupMode || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (static_cast<long>(millis() - mdnsRefreshNextAt) < 0) {
    return;
  }
  mdnsRefreshNextAt = millis() + MDNS_REFRESH_INTERVAL_MS;
  MDNS.end();
  if (MDNS.begin(mdnsHostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println(("mDNS refreshed: http://" + mdnsHostname + ".local").c_str());
  } else {
    Serial.println("mDNS refresh failed; use the IP address instead.");
  }
}

void setupDisplay() {
  Serial2.begin(DISPLAY_UART_BAUD, SERIAL_8N1, DISPLAY_UART_RX_PIN, DISPLAY_UART_TX_PIN);
  displayReady = true;
  sendToDisplay("t0", "PromptPay QR");
  sendToDisplay("t1", "Booting...");
  sendToDisplay("t2", "");
  sendToDisplay("t3", "");
  clearQrArea();
}

void renderNetworkStatus() {
  if (setupMode) {
    sendToDisplay("t0", "PaymentESP SETUP");
    sendToDisplay("t1", SETUP_AP_SSID);
    sendToDisplay("t2", "PASS: paymentesp");
    sendToDisplay("t3", "192.168.4.1/setup");
  } else {
    sendToDisplay("t0", "PaymentESP READY");
    sendToDisplay("t1", std::string(WiFi.localIP().toString().c_str()));
    sendToDisplay("t2", mdnsHostname + ".local");
    sendToDisplay("t3", "Open browser");
  }
  clearQrArea();
}

void printSimulatorHelp() {
  Serial.println();
  Serial.println("=== SMS Simulator Commands ===");
  Serial.println("PAY 15");
  Serial.println("SMS: KBank money in 15.00 THB");
  Serial.println("SMS: เงินเข้า 15.00 บาท");
  Serial.println("The simulator extracts the first amount and queues 1 pulse per whole THB.");
  Serial.println("==============================");
}

void processSerialCommand(String line) {
  line.trim();
  if (line.isEmpty()) {
    return;
  }

  String upper = line;
  upper.toUpperCase();
  if (upper == "HELP" || upper == "?") {
    printSimulatorHelp();
    return;
  }

  bool isSimulatorPayment = false;
  String paymentText = line;
  if (upper.startsWith("SMS:")) {
    isSimulatorPayment = true;
    paymentText = line.substring(4);
  } else if (upper.startsWith("SMS ")) {
    isSimulatorPayment = true;
    paymentText = line.substring(4);
  } else if (upper.startsWith("PAY ")) {
    isSimulatorPayment = true;
    paymentText = line.substring(4);
  }

  if (!isSimulatorPayment) {
    Serial.print("Unknown serial command: ");
    Serial.println(line);
    Serial.println("Type HELP for simulator commands.");
    return;
  }

  const double amount = extractAmountFromText(paymentText);
  const std::string formattedAmount = formatMoney(amount);
  if (formattedAmount.empty()) {
    Serial.print("SMS simulator could not extract a valid amount from: ");
    Serial.println(paymentText);
    return;
  }

  const std::string paymentId = "sms-sim-" + std::to_string(millis()) + "-" + formattedAmount;
  const std::string reference = "SMS-SIM";
  const bool triggered = confirmPayment(paymentId, formattedAmount, reference, "Serial SMS Simulator");
  if (triggered) {
    Serial.print("SMS simulator accepted amount THB ");
    Serial.println(formattedAmount.c_str());
  } else {
    Serial.println("SMS simulator ignored duplicate payment.");
  }
}

void processSerialSimulator() {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n' || ch == '\r') {
      processSerialCommand(serialCommandBuffer);
      serialCommandBuffer = "";
    } else if (serialCommandBuffer.length() < 240) {
      serialCommandBuffer += ch;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("ESP32 PromptPay QR Payment");
  loadPersistedConfig();
  pinMode(PAYMENT_PULSE_PIN, OUTPUT);
  pinMode(PAYMENT_PULSE_TENS_PIN, OUTPUT);
  digitalWrite(PAYMENT_PULSE_PIN, LOW);
  digitalWrite(PAYMENT_PULSE_TENS_PIN, LOW);
  setupDisplay();

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    syncClock();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/setup", HTTP_GET, handleSetup);
  server.on("/setup", HTTP_POST, handleSetup);
  server.on("/api/static", HTTP_GET, handleStaticQr);
  server.on("/api/dynamic", HTTP_GET, handleDynamicQr);
  server.on("/api/ppoints/qr", HTTP_GET, handlePpointsQr);
  server.on("/api/qr.svg", HTTP_GET, handleQrSvg);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/customer-qr", HTTP_GET, handleCustomerQr);
  server.on("/api/ppoints/mock", HTTP_GET, handlePpointsMock);
  server.on("/api/ppoints/baseline", HTTP_GET, handlePpointsBaseline);
  server.on("/api/ppoints/check", HTTP_GET, handlePpointsCheck);
  server.on("/api/ppoints/monitor", HTTP_GET, handlePpointsMonitor);
  server.on("/api/pulse/config", HTTP_GET, handlePulseConfig);
  server.on("/api/payment", HTTP_GET, handlePaymentConfirmation);
  server.on("/api/payment", HTTP_POST, handlePaymentConfirmation);
  server.on("/api/payment/trigger", HTTP_GET, handlePpointsCheck);
  server.on("/api/payment/trigger", HTTP_POST, handlePpointsCheck);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started on port 80");
  printSimulatorHelp();
  renderNetworkStatus();
}

void loop() {
  server.handleClient();
  processSerialSimulator();
  processPpointsMonitor();
  processPaymentPulses();
  processWifiReconnect();
  processMdnsRefresh();
}
