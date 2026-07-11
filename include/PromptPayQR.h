#pragma once

#include <stdint.h>

#include <string>
#include <vector>

enum class PromptPayTargetType {
  Phone,
  NationalId,
  EWallet,
};

struct PromptPayConfig {
  std::string promptPayId;
  std::string merchantName = "PROMPTPAY";
  std::string city = "BANGKOK";
};

struct PromptPayPayload {
  std::string payload;
  std::string normalizedTarget;
  PromptPayTargetType targetType;
};

struct QrEvent {
  std::string mode;
  std::string payload;
  std::string promptPayId;
  std::string amount;
  std::string reference;
  unsigned long createdAtMs = 0;
  uint64_t createdAtEpochMs = 0;
  std::string createdAt;
  int webhookStatus = 0;
};

uint16_t crc16CcittFalse(const std::string& data);
bool hasValidPromptPayCrc(const std::string& payload);
std::string formatMoney(double amount);
PromptPayPayload buildPromptPayPayload(const PromptPayConfig& config,
                                       double amount = 0.0,
                                       const std::string& reference = "",
                                       bool dynamicQr = false);
std::string targetTypeName(PromptPayTargetType type);
std::string jsonEscape(const std::string& value);
std::string eventToJson(const QrEvent& event);
std::string eventsToJson(const std::vector<QrEvent>& events);
