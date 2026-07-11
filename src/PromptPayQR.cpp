#include "PromptPayQR.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

std::string digitsOnly(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

std::string tlv(const char* id, const std::string& value) {
  char len[3];
  std::snprintf(len, sizeof(len), "%02u", static_cast<unsigned>(value.size()));
  return std::string(id) + len + value;
}

struct NormalizedTarget {
  std::string value;
  PromptPayTargetType type;
  const char* subTag;
};

NormalizedTarget normalizePromptPayTarget(const std::string& input) {
  const std::string digits = digitsOnly(input);

  if (digits.size() == 10 && digits[0] == '0') {
    return {"0066" + digits.substr(1), PromptPayTargetType::Phone, "01"};
  }

  if (digits.size() == 11 && digits.rfind("66", 0) == 0) {
    return {"00" + digits, PromptPayTargetType::Phone, "01"};
  }

  if (digits.size() == 13) {
    return {digits, PromptPayTargetType::NationalId, "02"};
  }

  if (digits.size() == 15) {
    return {digits, PromptPayTargetType::EWallet, "03"};
  }

  throw std::invalid_argument(
      "PromptPay ID must be a Thai phone number, 13-digit national/tax ID, or 15-digit e-wallet ID");
}

std::string maybeTruncate(const std::string& value, size_t maxLen) {
  if (value.size() <= maxLen) {
    return value;
  }
  return value.substr(0, maxLen);
}

}  // namespace

uint16_t crc16CcittFalse(const std::string& data) {
  uint16_t crc = 0xFFFF;
  for (unsigned char byte : data) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int i = 0; i < 8; ++i) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

bool hasValidPromptPayCrc(const std::string& payload) {
  if (payload.size() < 8) {
    return false;
  }
  const size_t marker = payload.rfind("6304");
  if (marker == std::string::npos || marker + 8 != payload.size()) {
    return false;
  }

  const std::string body = payload.substr(0, marker + 4);
  const std::string crcText = payload.substr(marker + 4);
  char expected[5];
  std::snprintf(expected, sizeof(expected), "%04X", crc16CcittFalse(body));
  return crcText == expected;
}

std::string formatMoney(double amount) {
  if (amount <= 0.0 || std::isnan(amount) || std::isinf(amount)) {
    return "";
  }

  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%.2f", amount);
  return buffer;
}

PromptPayPayload buildPromptPayPayload(const PromptPayConfig& config,
                                       double amount,
                                       const std::string& reference,
                                       bool dynamicQr) {
  const NormalizedTarget target = normalizePromptPayTarget(config.promptPayId);

  std::string merchantAccount;
  merchantAccount += tlv("00", "A000000677010111");
  merchantAccount += tlv(target.subTag, target.value);

  std::string payload;
  payload += tlv("00", "01");
  payload += tlv("01", dynamicQr ? "12" : "11");
  payload += tlv("29", merchantAccount);
  payload += tlv("58", "TH");
  payload += tlv("53", "764");

  const std::string amountText = formatMoney(amount);
  if (!amountText.empty()) {
    payload += tlv("54", amountText);
  }

  if (!reference.empty()) {
    payload += tlv("62", tlv("05", maybeTruncate(reference, 25)));
  }

  payload += "6304";
  char crc[5];
  std::snprintf(crc, sizeof(crc), "%04X", crc16CcittFalse(payload));
  payload += crc;

  return {payload, target.value, target.type};
}

std::string targetTypeName(PromptPayTargetType type) {
  switch (type) {
    case PromptPayTargetType::Phone:
      return "phone";
    case PromptPayTargetType::NationalId:
      return "national_id";
    case PromptPayTargetType::EWallet:
      return "e_wallet";
  }
  return "unknown";
}

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string eventToJson(const QrEvent& event) {
  std::string out = "{";
  out += "\"mode\":\"" + jsonEscape(event.mode) + "\",";
  out += "\"promptPayId\":\"" + jsonEscape(event.promptPayId) + "\",";
  out += "\"amount\":\"" + jsonEscape(event.amount) + "\",";
  out += "\"reference\":\"" + jsonEscape(event.reference) + "\",";
  out += "\"payload\":\"" + jsonEscape(event.payload) + "\",";
  out += "\"createdAtMs\":" + std::to_string(event.createdAtMs) + ",";
  out += "\"createdAtEpochMs\":" + std::to_string(event.createdAtEpochMs) + ",";
  out += "\"createdAt\":\"" + jsonEscape(event.createdAt) + "\",";
  out += "\"webhookStatus\":" + std::to_string(event.webhookStatus);
  out += "}";
  return out;
}

std::string eventsToJson(const std::vector<QrEvent>& events) {
  std::string out = "[";
  for (size_t i = 0; i < events.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += eventToJson(events[i]);
  }
  out += "]";
  return out;
}
