#include "native/runtime.h"
#include <cstdint>
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  model->signal("arc_printf_enable").set(1);
  uint16_t q = 0, count = 0;
  auto expect = [&] {
    if (model->signal("q").get() != q ||
        model->signal("counter").get() != count)
      throw std::runtime_error(
          "SV task outputs, automatic locals or inout copy-back changed");
  };
  uint64_t time = 0;
  auto leaf = [](uint8_t value, bool select) -> uint16_t {
    uint8_t high = select ? value : uint8_t(~value);
    return (uint16_t(high) << 8) | (select ? 0x5a : 0xa5);
  };
  for (unsigned c = 0; c < 512; ++c) {
    uint8_t data = uint8_t(17 * (c / 2) + 9);
    bool reset = c % 17 == 0, enable = c % 3 != 1;
    model->advance(time++, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("data").set(data);
    });
    expect();
    if (reset)
      q = count = 0;
    else if (enable) {
      q = leaf(data, data & 1) ^ leaf(data ^ 0x33, !(data & 1)) ^ count;
      count += 0x137;
      std::fprintf(stderr, "EXPECTED_TASK %u %u\n", unsigned(q),
                   unsigned(count));
    }
    model->advance(time++, [&] { model->signal("clock").set(1); });
    expect();
    model->advance(time++, [&] { model->signal("data").set(uint8_t(~data)); });
    expect();
    model->advance(time++);
    expect();
  }
  std::fprintf(
      stderr,
      "PASS: 512 imported SV task cycles, nested automatic locals, "
      "input/output/inout, branches, reset, enable and pre-edge sampling\n");
}
