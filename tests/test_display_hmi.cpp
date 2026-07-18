#include <cassert>
#include <iostream>

#include "DisplayHMI.h"

int main() {
  const std::string simple = buildHmiTextCommand("t0", "PAID");
  assert(simple == std::string("t0.txt=\"PAID\"") + "\xFF\xFF\xFF");

  const std::string withQuoteAndBackslash = buildHmiTextCommand("t3", "say \"hi\\bye\"");
  assert(withQuoteAndBackslash ==
         std::string("t3.txt=\"say \\\"hi\\\\bye\\\"\"") + "\xFF\xFF\xFF");

  const std::string empty = buildHmiTextCommand("t1", "");
  assert(empty == std::string("t1.txt=\"\"") + "\xFF\xFF\xFF");

  const std::string fillBlack = buildHmiFillCommand(10, 20, 30, 40, 0);
  assert(fillBlack == std::string("fill 10,20,30,40,0") + "\xFF\xFF\xFF");

  const std::string fillWhite = buildHmiFillCommand(0, 0, 280, 280, 65535);
  assert(fillWhite == std::string("fill 0,0,280,280,65535") + "\xFF\xFF\xFF");

  std::cout << "DisplayHMI tests passed\n";
  return 0;
}
