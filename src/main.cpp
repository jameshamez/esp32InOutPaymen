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
constexpr const char* MDNS_HOSTNAME = "paymentesp";
constexpr const char* PREFERENCES_NAMESPACE = "promptpay";
constexpr const char* PREFERENCES_ID_KEY = "id";
constexpr const char* PREFERENCES_WEBHOOK_KEY = "webhook";
constexpr const char* PREFERENCES_WIFI_SSID_KEY = "wifiSsid";
constexpr const char* PREFERENCES_WIFI_PASSWORD_KEY = "wifiPass";
constexpr const char* DEFAULT_PPOINTS_STATION_ID = "S-24001";
constexpr const char* DEFAULT_PPOINTS_BANK_ID = "X-9786";
constexpr const char* DEFAULT_PPOINTS_QR_PAYLOAD =
    "00020101021129390016A000000677010111031500499907526116353037645802TH6304E345";
constexpr const char* PPOINTS_BASE_URL = "https://p-points.com/sms_payin_rd.php";
constexpr size_t MAX_LOGS = 20;
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET = -1;
constexpr int PAYMENT_PULSE_PIN = 26;
constexpr unsigned long PAYMENT_PULSE_DURATION_MS = 500;
constexpr unsigned long PAYMENT_PULSE_GAP_MS = 250;
constexpr unsigned long PPOINTS_QR_TTL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long PPOINTS_POLL_INTERVAL_MS = 5000;
constexpr int MAX_PAYMENT_PULSES = 200;

WebServer server(80);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences preferences;
PromptPayConfig qrConfig{DEFAULT_PROMPTPAY_ID, "PROMPTPAY", "BANGKOK"};
std::string webhookUrl = DEFAULT_WEBHOOK_URL;
std::deque<QrEvent> logs;
String wifiSsid = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
bool displayReady = false;
bool clockSynced = false;
bool setupMode = false;
bool paymentPulseActive = false;
bool paymentPulseHigh = false;
unsigned long paymentPulseNextAt = 0;
int paymentPulsesPending = 0;
int paymentPulsesCompleted = 0;
int paymentPulsesTotal = 0;
std::string lastPaymentId;
std::string lastPaymentAmount;
std::string lastPaymentReference;
String serialCommandBuffer;
bool hasPpointsPreviousTotal = false;
double lastPpointsTotalAmount = 0.0;
std::string lastPpointsCount;
bool ppointsSessionActive = false;
unsigned long ppointsSessionExpiresAt = 0;

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
    const int offsetX = 2;
    const int offsetY = (SCREEN_HEIGHT - qrPixels) / 2;

    for (uint8_t y = 0; y < qrcode.size; ++y) {
      for (uint8_t x = 0; x < qrcode.size; ++x) {
        if (qrcode_getModule(&qrcode, x, y)) {
          display.fillRect(offsetX + x * scale, offsetY + y * scale, scale, scale, SSD1306_WHITE);
        }
      }
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

int pulseCountFromAmount(const std::string& amount) {
  const double value = String(amount.c_str()).toDouble();
  if (value <= 0.0) {
    return 0;
  }
  const int pulses = static_cast<int>(value);
  return std::min(std::max(pulses, 1), MAX_PAYMENT_PULSES);
}

void startPaymentPulses(int count) {
  const int safeCount = std::min(std::max(count, 1), MAX_PAYMENT_PULSES);
  digitalWrite(PAYMENT_PULSE_PIN, LOW);
  paymentPulseActive = true;
  paymentPulseHigh = false;
  paymentPulseNextAt = millis();
  paymentPulsesPending = safeCount;
  paymentPulsesCompleted = 0;
  paymentPulsesTotal = safeCount;

  Serial.print("Pulse count queued: ");
  Serial.println(paymentPulsesTotal);
}

void processPaymentPulses() {
  if (!paymentPulseActive || static_cast<long>(millis() - paymentPulseNextAt) < 0) {
    return;
  }

  if (paymentPulseHigh) {
    digitalWrite(PAYMENT_PULSE_PIN, LOW);
    paymentPulseHigh = false;
    ++paymentPulsesCompleted;

    if (paymentPulsesPending <= 0) {
      paymentPulseActive = false;
      Serial.print("Pulse complete: ");
      Serial.println(paymentPulsesCompleted);
      return;
    }

    paymentPulseNextAt = millis() + PAYMENT_PULSE_GAP_MS;
    return;
  }

  digitalWrite(PAYMENT_PULSE_PIN, HIGH);
  paymentPulseHigh = true;
  --paymentPulsesPending;
  paymentPulseNextAt = millis() + PAYMENT_PULSE_DURATION_MS;
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
  startPaymentPulses(pulseCountFromAmount(amount));

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
  Serial.print("Pulse GPIO: ");
  Serial.println(PAYMENT_PULSE_PIN);
  Serial.print("Pulse count: ");
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

  const double delta = currentTotal - lastPpointsTotalAmount;
  char deltaBuffer[24];
  std::snprintf(deltaBuffer, sizeof(deltaBuffer), "%+.2f", delta);
  deltaText = deltaBuffer;

  const std::string paymentId = "ppoints-delta-" + stationId + "-" + bankId + "-" + count + "-" + deltaText;
  const std::string reference = "PPOINTS-DIFF-" + count;
  if (paymentId == lastPaymentId) {
    duplicate = true;
    return paymentId;
  }

  lastPpointsTotalAmount = currentTotal;
  lastPpointsCount = count;

  if (delta > 0.0) {
    const std::string deltaAmount = formatMoney(delta);
    confirmPayment(paymentId, deltaAmount, reference, source);
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
    const std::string requestedPromptPay = server.arg("promptpay").c_str();
    const std::string requestedWebhook = server.arg("webhook").c_str();
    std::string error;

    if (requestedSsid.isEmpty() || requestedSsid.length() > 32) {
      error = "WiFi SSID is required and must not exceed 32 characters";
    } else if (requestedPassword.length() > 63) {
      error = "WiFi password must not exceed 63 characters";
    } else if (!validatePromptPayId(requestedPromptPay, error)) {
      // Validation error is already populated.
    } else if (!validateWebhookUrl(requestedWebhook, error)) {
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
      qrConfig.promptPayId = requestedPromptPay;
      webhookUrl = requestedWebhook;
      preferences.putString(PREFERENCES_WIFI_SSID_KEY, wifiSsid);
      preferences.putString(PREFERENCES_WIFI_PASSWORD_KEY, wifiPassword);
      preferences.putString(PREFERENCES_ID_KEY, qrConfig.promptPayId.c_str());
      preferences.putString(PREFERENCES_WEBHOOK_KEY, webhookUrl.c_str());

      server.send(200, "text/html; charset=utf-8",
                  "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<style>body{font-family:Arial,sans-serif;max-width:600px;margin:40px auto;padding:20px}</style>"
                  "<h1>Settings saved</h1><p>ESP32 is restarting and will connect to the configured WiFi.</p>"
                  "<p>Reconnect your phone or computer to the same WiFi, then open <b>http://paymentesp.local</b>.</p>");
      delay(900);
      ESP.restart();
      return;
    }
  }

  const std::string currentSsid = htmlEscape(wifiSsid.c_str());
  const std::string currentPromptPay = htmlEscape(qrConfig.promptPayId);
  const std::string currentWebhook = htmlEscape(webhookUrl);
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
          "<label>PromptPay ID</label><input name='promptpay' inputmode='numeric' required value='" + currentPromptPay + "'>"
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
      "input,button{font:inherit;padding:11px;margin:4px 0;width:100%;box-sizing:border-box}"
      "button{cursor:pointer;border:0;border-radius:6px;background:#0f766e;color:white;font-weight:700}"
      "button.secondary{background:#334155}.qrwrap{display:flex;justify-content:center;align-items:center;min-height:330px}"
      "button.active{outline:3px solid #f59e0b;outline-offset:2px}.status{font-weight:700;color:#0f766e}"
      "#qr{width:min(330px,90vw);height:min(330px,90vw);background:white;border:10px solid white;box-shadow:0 4px 20px #0002}"
      "pre{white-space:pre-wrap;word-break:break-all;background:#f1f5f9;padding:12px;border-radius:6px;font-size:12px}"
      ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}@media(max-width:640px){.row{grid-template-columns:1fr}}</style>"
      "</head><body><h1>ESP32 PromptPay QR</h1>"
      "<div class='panel'><b id='network'>Loading network status...</b><br><a href='/setup'>Hardware / WiFi Setup</a></div>"
      "<div class='panel'><form id='form'><label>PromptPay ID</label><input id='promptpay' name='promptpay' value='' inputmode='numeric'>"
      "<label>Webhook URL</label><input id='webhook' name='webhook' value='' placeholder='http://server/api/payment'>"
      "<label>Amount</label><input name='amount' value='99.00' inputmode='decimal'>"
      "<label>Reference</label><input name='ref' value='ORDER-0001'>"
      "<div class='row'><button class='secondary' name='mode' value='static'>Static QR</button>"
      "<button name='mode' value='dynamic'>Dynamic QR</button></div>"
      "<button class='secondary' name='mode' value='config'>Save PromptPay ID</button></form></div>"
      "<div class='panel'><h2>P-Points Direct</h2>"
      "<label>stn_id</label><input id='ppStn' value='S-24001'>"
      "<label>bank_id</label><input id='ppBank' value='X-9786'>"
      "<label>P-Points Amount</label><input id='ppAmount' value='15.00' inputmode='decimal'>"
      "<label>P-Points QR Payload</label><input id='ppPayload' value='00020101021129390016A000000677010111031500499907526116353037645802TH6304E345'>"
      "<div class='row'><button id='ppStatic' type='button' class='secondary'>P-Points Static QR</button>"
      "<button id='ppDynamic' type='button'>P-Points Dynamic QR</button></div>"
      "<div class='row'><button id='ppCheck' type='button'>Check P-Points Pay-in</button>"
      "<button id='ppShow' type='button' class='secondary'>Show QR Mode</button></div>"
      "<p id='ppAuto'>Auto check idle</p>"
      "<p class='note'>P-Points Static/Dynamic QR keeps the QR on OLED while auto checking every 5 seconds for 5 minutes. The OLED changes only when payment is detected or the session expires.</p></div>"
      "<div class='panel'><p id='qrStatus' class='status'>QR idle</p></div>"
      "<div class='panel qrwrap'><img id='qr' alt='PromptPay QR'></div>"
      "<div class='panel'><pre id='out'>Ready</pre></div>"
      "<script>let latestPayload='';let latestQrMode='dynamic';let ppPollTimer=0;let ppCountdownTimer=0;let ppExpiresAt=0;let ppCheckCount=0;let ppMaxChecks=60;function show(j){document.getElementById('out').textContent=JSON.stringify(j,null,2)}"
      "function setQrStatus(t){document.getElementById('qrStatus').textContent=t}"
      "function setPpActive(mode){document.getElementById('ppStatic').classList.toggle('active',mode==='static');document.getElementById('ppDynamic').classList.toggle('active',mode==='dynamic')}"
      "function setQrImage(payload,label){latestPayload=payload;let img=document.getElementById('qr');img.style.opacity='1';img.src='/api/qr.svg?data='+encodeURIComponent(payload)+'&t='+Date.now();setQrStatus(label+' | payload '+payload.length+' chars | crc '+payload.slice(-4))}"
      "async function loadConfig(){let r=await fetch('/api/config');let j=await r.json();document.getElementById('promptpay').value=j.promptPayId||'';document.getElementById('webhook').value=j.webhookUrl||'';document.getElementById('network').textContent=(j.setupMode?'Setup mode: ':'Ready: ')+(j.ip||'no IP')+' | '+(j.hostname||'paymentesp.local')}"
      "async function generateQr(mode){let form=document.getElementById('form');let p=new URLSearchParams(new FormData(form));let r=await fetch('/api/'+mode+'?'+p);let j=await r.json();show(j);if(j.payload){latestQrMode=mode;setQrImage(j.payload,'PromptPay '+mode.toUpperCase())}return j}"
      "async function generatePpointsQr(mode){setPpActive(mode);setQrStatus('Generating P-Points '+mode.toUpperCase()+' QR...');document.getElementById('qr').style.opacity='0.35';let p=new URLSearchParams({mode:mode,payload:document.getElementById('ppPayload').value});if(mode==='dynamic')p.set('amount',document.getElementById('ppAmount').value||'1.00');let r=await fetch('/api/ppoints/qr?'+p);let j=await r.json();show(j);if(j.payload){latestQrMode=mode;setQrImage(j.payload,'P-Points '+mode.toUpperCase()+(mode==='dynamic'?' THB '+(j.amount||document.getElementById('ppAmount').value):' no amount'))}return j}"
      "async function generateDynamic(){return generateQr('dynamic')}"
      "document.getElementById('form').onsubmit=async e=>{e.preventDefault();let mode=e.submitter.value;"
      "let p=new URLSearchParams(new FormData(e.target));let r;"
      "if(mode==='config'){r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p})}"
      "else{let b=mode==='dynamic'?'/api/dynamic':'/api/static';r=await fetch(b+'?'+p)}"
      "let j=await r.json();show(j);"
      "if(j.payload){latestQrMode=mode;setQrImage(j.payload,'PromptPay '+mode.toUpperCase())}};"
      "async function showCustomerQr(){if(!latestPayload)await generateDynamic();let p=new URLSearchParams({stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value,ref:'PPOINTS-'+latestQrMode.toUpperCase(),payload:latestPayload});let r=await fetch('/api/customer-qr?'+p);return await r.json()}"
      "async function setPpointsBaseline(){let p=new URLSearchParams({stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value});let r=await fetch('/api/ppoints/baseline?'+p);return await r.json()}"
      "function stopPpointsAuto(msg){if(ppPollTimer)clearInterval(ppPollTimer);if(ppCountdownTimer)clearInterval(ppCountdownTimer);ppPollTimer=0;ppCountdownTimer=0;ppExpiresAt=0;if(msg)document.getElementById('ppAuto').textContent=msg}"
      "function updatePpointsCountdown(){let ms=ppExpiresAt-Date.now();if(ms<=0){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.');return}document.getElementById('ppAuto').textContent='Auto checking '+latestQrMode.toUpperCase()+' | check '+ppCheckCount+'/'+ppMaxChecks+' | expires in '+Math.ceil(ms/1000)+'s'}"
      "async function checkPpoints(auto=false){let p=new URLSearchParams({stn_id:document.getElementById('ppStn').value,bank_id:document.getElementById('ppBank').value});let r=await fetch('/api/ppoints/check?'+p);let j=await r.json();if(!auto)show(j);return j}"
      "async function pollPpoints(){if(!ppExpiresAt||Date.now()>=ppExpiresAt){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.');show({ok:false,expired:true,note:'QR session expired after 5 minutes.'});return}ppCheckCount++;updatePpointsCountdown();let j=await checkPpoints(true);show(Object.assign({autoCheck:true,checkCount:ppCheckCount,maxChecks:ppMaxChecks,qrMode:latestQrMode},j));if(j.pulseTriggered||Number(j.delta)>0){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('Payment detected. Pulse sent.');show(Object.assign({autoCheck:true,checkCount:ppCheckCount,note:'Payment detected. Pulse sent.'},j))}else if(j.sessionExpired){document.getElementById('qr').style.opacity='0.35';stopPpointsAuto('QR expired. Generate a new P-Points QR.')}}"
      "function startPpointsAuto(baseline){stopPpointsAuto();ppCheckCount=0;ppMaxChecks=Math.max(1,Math.ceil((baseline.expiresInMs||300000)/(baseline.pollIntervalMs||5000)));ppExpiresAt=Date.now()+(baseline.expiresInMs||300000);updatePpointsCountdown();ppCountdownTimer=setInterval(updatePpointsCountdown,1000);ppPollTimer=setInterval(()=>pollPpoints().catch(e=>show({autoCheck:true,error:e.message})),baseline.pollIntervalMs||5000)}"
      "async function preparePpointsQr(mode){let qr=await generatePpointsQr(mode);let oled=await showCustomerQr();let baseline=await setPpointsBaseline();if(baseline.ok)startPpointsAuto(baseline);else stopPpointsAuto('Could not save baseline from P-Points.');show({ok:!!baseline.ok,qr:qr,oled:oled,baseline:baseline,amount:mode==='dynamic'?document.getElementById('ppAmount').value:'static-no-amount',note:baseline.ok?'Baseline saved. Auto checking every 5 seconds for 5 minutes.':'Could not save baseline from P-Points.'})}"
      "document.getElementById('ppStatic').onclick=async()=>preparePpointsQr('static');"
      "document.getElementById('ppDynamic').onclick=async()=>preparePpointsQr('dynamic');"
      "document.getElementById('ppShow').onclick=async()=>show(await showCustomerQr());"
      "document.getElementById('ppCheck').onclick=()=>checkPpoints(false);"
      "loadConfig().then(()=>setQrStatus('Choose P-Points Static or Dynamic QR'));</script>"
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

  std::string body = "{";
  body += "\"mode\":\"ppoints-" + std::string(dynamicQr ? "dynamic" : "static") + "\",";
  body += "\"source\":\"p-points-template\",";
  body += "\"amount\":\"" + jsonEscape(dynamicQr ? formatMoney(amount) : "") + "\",";
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
  body += "\"hostname\":\"http://" + std::string(MDNS_HOSTNAME) + ".local\"";
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
  const std::string stationId = argOrDefault("stn_id", "S-24001").c_str();
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
  HTTPClient http;
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
  const std::string stationId = argOrDefault("stn_id", DEFAULT_PPOINTS_STATION_ID).c_str();
  const std::string bankId = argOrDefault("bank_id", DEFAULT_PPOINTS_BANK_ID).c_str();

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

void handleNotFound() {
  sendJson("{\"error\":\"not found\"}", 404);
}

void connectWifi() {
  setupMode = false;
  if (!wifiSsid.isEmpty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    Serial.print("Connecting to WiFi: ");
    Serial.println(wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
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
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS ready: http://paymentesp.local");
    } else {
      Serial.println("mDNS unavailable; use the IP address shown above.");
    }
    return;
  }

  setupMode = true;
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
  digitalWrite(PAYMENT_PULSE_PIN, LOW);
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
  server.on("/api/payment", HTTP_GET, handlePaymentConfirmation);
  server.on("/api/payment", HTTP_POST, handlePaymentConfirmation);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started on port 80");
  printSimulatorHelp();
  renderNetworkStatus();
}

void loop() {
  server.handleClient();
  processSerialSimulator();
  processPaymentPulses();
}
