#include <cassert>
#include <iostream>

#include "DisplayHMI.h"

int main() {
  const std::string simple = buildHmiTextCommand("t0", "PAID");
  assert(simple == std::string("t0.txt=\"PAID\"") + "\xFF\xFF\xFF");

  const std::string withQuoteAndBackslash = buildHmiTextCommand("t3", "say \"hi\\bye\"");
  assert(withQuoteAndBackslash ==
         std::string("t3.txt=\"say \\\"hi\\\\bye\\\"\"") + "\xFF\xFF\xFF");

  const std::string empty = buildHmiTextCommand("qr0", "");
  assert(empty == std::string("qr0.txt=\"\"") + "\xFF\xFF\xFF");

  assert(DISPLAY_QR_MAX_LEN == 80);

  std::cout << "DisplayHMI tests passed\n";
  return 0;
}
