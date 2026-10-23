#include "native/runtime.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  const uint64_t values[] = {0,
                             UINT64_C(0x8000000000000000),
                             UINT64_C(0x3ff4000000000000),
                             UINT64_C(0xbff8000000000000),
                             UINT64_C(0x7ff8000000000001),
                             UINT64_C(0x7ff0000000000000),
                             1,
                             UINT64_C(0x7fefffffffffffff)};
  unsigned count = 0;
  auto expect = [&] {
    if (model->signal("count").get() != count)
      throw std::runtime_error("real trace process changed register sampling");
  };
  uint64_t time = 0;
  for (unsigned c = 0; c < 512; ++c) {
    uint64_t bits = values[(c / 2) % 8];
    bool reset = c % 17 == 0, enable = c % 3 != 1, printing = c % 5 != 0;
    model->advance(time++, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("bits").set(bits);
      model->signal("arc_printf_enable").set(printing);
    });
    expect();
    if (reset)
      count = 0;
    else if (enable) {
      if (printing) {
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        std::fprintf(stderr, "EXPECTED_REAL %u [%12.3f] [%-20.4e] [%.9g]\n",
                     count, value, value, value);
      }
      count = (count + 1) & 255;
    }
    model->advance(time++, [&] { model->signal("clock").set(1); });
    expect();
    model->advance(time++, [&] { model->signal("bits").set(~bits); });
    expect();
    model->advance(time++);
    expect();
  }
  std::fprintf(
      stderr,
      "PASS: 512 imported real trace cycles, fixed/scientific/general formats, "
      "pre-edge counters, reset, enable and print suppression\n");
}
