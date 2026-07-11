#include <cassert>
#include <iostream>

#include "PromptPayQR.h"

int main() {
  assert(crc16CcittFalse("123456789") == 0x29B1);

  PromptPayConfig config;
  config.promptPayId = "081-234-5678";

  PromptPayPayload staticQr = buildPromptPayPayload(config, 0.0, "ORDER-STATIC", false);
  assert(staticQr.normalizedTarget == "0066812345678");
  assert(targetTypeName(staticQr.targetType) == "phone");
  assert(staticQr.payload.find("010211") != std::string::npos);
  assert(staticQr.payload.find("540") == std::string::npos);
  assert(staticQr.payload.find("ORDER-STATIC") != std::string::npos);
  assert(hasValidPromptPayCrc(staticQr.payload));

  PromptPayPayload dynamicQr = buildPromptPayPayload(config, 123.45, "ORDER-DYNAMIC", true);
  assert(dynamicQr.payload.find("010212") != std::string::npos);
  assert(dynamicQr.payload.find("5406123.45") != std::string::npos);
  assert(dynamicQr.payload.find("ORDER-DYNAMIC") != std::string::npos);
  assert(hasValidPromptPayCrc(dynamicQr.payload));

  PromptPayConfig nationalId;
  nationalId.promptPayId = "1234567890123";
  PromptPayPayload nationalQr = buildPromptPayPayload(nationalId, 1.0, "", true);
  assert(targetTypeName(nationalQr.targetType) == "national_id");
  assert(nationalQr.payload.find("02131234567890123") != std::string::npos);
  assert(hasValidPromptPayCrc(nationalQr.payload));

  QrEvent event;
  event.mode = "dynamic";
  event.promptPayId = dynamicQr.normalizedTarget;
  event.amount = "123.45";
  event.reference = "ORDER-DYNAMIC";
  event.payload = dynamicQr.payload;
  event.createdAtMs = 1234;
  event.createdAtEpochMs = 1782522000000ULL;
  event.createdAt = "2026-06-27T08:00:00+07:00";
  event.webhookStatus = 200;
  const std::string json = eventToJson(event);
  assert(json.find("\"createdAt\":\"2026-06-27T08:00:00+07:00\"") != std::string::npos);
  assert(json.find("\"createdAtEpochMs\":1782522000000") != std::string::npos);
  assert(json.find("\"webhookStatus\":200") != std::string::npos);

  std::cout << "PromptPay QR tests passed\n";
  std::cout << dynamicQr.payload << "\n";
  return 0;
}
