#include "DisplayHMI.h"

namespace {

std::string escapeHmiValue(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 4);
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::string buildHmiTextCommand(const std::string& component, const std::string& value) {
  std::string command = component + ".txt=\"" + escapeHmiValue(value) + "\"";
  command += '\xFF';
  command += '\xFF';
  command += '\xFF';
  return command;
}
