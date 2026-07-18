#pragma once

#include <string>

std::string buildHmiTextCommand(const std::string& component, const std::string& value);
std::string buildHmiFillCommand(int x, int y, int width, int height, int color);
