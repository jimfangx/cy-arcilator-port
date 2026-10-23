#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace chipyard_arc;
static std::array<unsigned, 2> labels, checks, sides;
static std::array<std::string, 2> side_text;
static void scope(int tag) {
  auto name = svGetNameFromScope(svGetScope());
  auto expected = std::string("FormatStringsHarness/original/rtl/rtl/") +
                  (tag ? "right" : "left");
  if (tag < 0 || tag > 1 || !name || name != expected)
    throw std::runtime_error("formatted string DPI physical scope changed");
}
static std::string decimal(unsigned __int128 n) {
  std::string s;
  do {
    s.insert(s.begin(), char('0' + n % 10));
    n /= 10;
  } while (n);
  return s;
}
static std::string binary(unsigned n) {
  std::string s;
  do {
    s.insert(s.begin(), char('0' + (n & 1)));
    n >>= 1;
  } while (n);
  return s;
}
static std::pair<std::string, std::string> expected(int tag, int value) {
  const char *label =
      (unsigned(value) & 3) == 0 ? "" : (tag ? "bEta" : "aLpHa");
  uint64_t low = UINT64_C(0xfedcba9876543210) ^ uint32_t(value);
  auto wide = (static_cast<unsigned __int128>(1) << 64) | low;
  char base[256];
  std::snprintf(base, sizeof(base),
                "n=%d u=%s h=1%016llx q=%.2f s=[%8s]/[%-8s]", value,
                decimal(wide).c_str(), (unsigned long long)low,
                double(value) / 4.0, label, label);
  std::string inner = std::string(value < 0 ? "neg{" : "pos{") + base + "}";
  char tail[128];
  std::snprintf(tail, sizeof(tail), " o=%o c=%c p=ABC",
                unsigned(uint8_t(value)), 65 + tag);
  std::string outer =
      "outer<" + inner + "> sgn=-" +
      decimal((static_cast<unsigned __int128>(1) << 65) - wide) +
      " b=" + binary(uint8_t(value)) + tail;
  return {inner, outer};
}
extern "C" const char *host_label(int tag, int value) {
  scope(tag);
  ++labels[tag];
  return (unsigned(value) & 3) == 0 ? "" : (tag ? "bEta" : "aLpHa");
}
extern "C" int host_check(int tag, int value, const char *inner,
                          const char *outer) {
  scope(tag);
  ++checks[tag];
  auto want = expected(tag, value);
  if (!inner || !outer || want.first != inner || want.second != outer)
    throw std::runtime_error("native formatted return mismatch: got [" +
                             std::string(inner ? inner : "NULL") + "] [" +
                             std::string(outer ? outer : "NULL") +
                             "] expected [" + want.first + "] [" + want.second +
                             "]");
  return value * 3 + tag;
}
extern "C" const char *host_side(int tag, const char *text) {
  scope(tag);
  ++sides[tag];
  side_text[tag] = std::string(text) + "!";
  return side_text[tag].c_str();
}
int main(int argc, char **argv) {
  for (unsigned recreation = 0; recreation < 2; ++recreation) {
    labels = {};
    checks = {};
    sides = {};
    std::array<std::string, 2> files;
    unsigned active = 0;
    {
      auto m = create_model(argc, argv);
      m->signal("arc_stop_enable").set(1);
      m->signal("arc_printf_enable").set(0);
      m->initialize();
      m->advance(0, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(1);
        m->signal("enable").set(0);
        m->signal("value").set(0);
      });
      std::array<int, 2> values{};
      for (unsigned cycle = 0; cycle < 32; ++cycle) {
        bool reset = cycle % 11 == 0, enable = cycle % 3 != 1,
             printing = cycle % 4 == 1;
        int value = int(cycle) * 7 - 80;
        m->advance(cycle * 2, [&] {
          m->signal("clock").set(0);
          m->signal("reset").set(reset);
          m->signal("enable").set(enable);
          m->signal("value").set(uint32_t(value));
          m->signal("arc_printf_enable").set(printing);
        });
        if (reset)
          values = {};
        else if (enable) {
          ++active;
          for (unsigned tag = 0; tag < 2; ++tag) {
            values[tag] = value * 3 + tag;
            auto want = expected(tag, value).second;
            if (printing)
              std::fprintf(
                  stderr,
                  "EXPECTED_NATIVE_FORMAT NATIVE_FORMAT tag=%u msg=[%s!]\n",
                  tag, want.c_str());
            files[tag] +=
                "FILE tag=" + std::to_string(tag) + " value=[" + want + "]\n";
          }
        }
        m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
        if (m->signal("first").get() != uint32_t(values[0]) ||
            m->signal("second").get() != uint32_t(values[1]))
          throw std::runtime_error(
              "formatted return clock/reset/enable output changed");
      }
      for (unsigned tag = 0; tag < 2; ++tag)
        if (labels[tag] != active || checks[tag] != active ||
            sides[tag] != active)
          throw std::runtime_error(
              "formatted output discarded or repeated DPI argument effects");
    }
    for (unsigned tag = 0; tag < 2; ++tag) {
      std::ifstream file("format-native-" + std::to_string(tag) + ".log",
                         std::ios::binary);
      std::ostringstream text;
      text << file.rdbuf();
      if (text.str() != files[tag])
        throw std::runtime_error(
            "formatted file output changed under quiet controls/recreation");
    }
  }
  std::puts(
      "PASS: dynamic/nested formatted string returns, original string DPI ABI, "
      "65-bit decimal/hex/signed values, real/binary/octal/char/packed fields, "
      "padding/empty strings, quiet argument effects, file output, two "
      "physical instances and model recreation");
}
