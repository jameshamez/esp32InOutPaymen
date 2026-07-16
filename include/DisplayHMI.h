#pragma once

#include <cstddef>
#include <string>

constexpr size_t DISPLAY_QR_MAX_LEN = 80;

std::string buildHmiTextCommand(const std::string& component, const std::string& value);
