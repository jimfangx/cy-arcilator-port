#include "native/runtime.h"
#include <cstdio>
int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  runtime->advance(0);
  auto mode = runtime->option("mode=", "default");
  std::vector<uint8_t> wide(17), sign(9, 255);
  sign.back() = 1;
  unsigned small = 7;
  if (mode == "default" || mode == "formats") {
    wide[0] = 3;
    wide[16] = 1;
  } else if (mode == "hex") {
    wide = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
            0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x01};
    std::fill(sign.begin(), sign.end(), 0);
    sign[8] = 1;
    small = 2;
  } else {
    sign[0] = 254;
    small = 0;
  }
  if (runtime->signal("wide").copy() != wide ||
      runtime->signal("signed").copy() != sign ||
      runtime->signal("small").get() != small)
    throw std::runtime_error("native wide plusarg parsing mismatch");
  std::vector<uint8_t> upper(17), zero(9);
  unsigned literal = 0;
  if (mode == "formats") {
    std::fill(upper.begin(), upper.end(), 255);
    upper.back() = 1;
    zero[0] = 37;
    literal = 511;
  }
  if (runtime->signal("upper").copy() != upper ||
      runtime->signal("zero").copy() != zero ||
      runtime->signal("literal").get() != literal)
    throw std::runtime_error("native plusarg conversion format mismatch");
  std::puts("PASS: native 129-bit plusargs, large defaults, base formats, signed values, width truncation, uppercase and zero-modified conversions and literal percent prefixes");
}
