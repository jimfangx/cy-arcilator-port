#include "native/runtime.h"
#include <cstdio>

static std::vector<uint8_t> bits(uint64_t low, uint64_t high, bool top) {
  std::vector<uint8_t> result(17);
  for (unsigned i = 0; i < 8; ++i) {
    result[i] = low >> (i * 8);
    result[i + 8] = high >> (i * 8);
  }
  result[16] = top;
  return result;
}

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  auto value =
      bits(UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210), true);
  auto resetValue =
      bits(UINT64_C(0xaa55aa55aa55aa55), UINT64_C(0x55aa55aa55aa55aa), false);
  std::vector<uint8_t> sampled(17);
  auto check = [&] {
    if (model->signal("q").copy() != value ||
        model->signal("sampled").copy() != sampled)
      throw std::runtime_error(
          "native signal retention or pre-edge sampling mismatch");
  };
  model->advance(0);
  check();
  for (uint64_t cycle = 0; cycle < 80; ++cycle) {
    auto low = cycle * UINT64_C(0xdeadbeef9abcdef) + 7;
    auto data = bits(low, ~low ^ (cycle << 32), cycle & 1);
    bool reset = cycle == 0 || cycle == 31, write = cycle % 5 != 2;
    model->advance(cycle * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("write").set(write);
      model->signal("data").assign(data);
    });
    check();
    sampled = value;
    if (reset || write)
      value = reset ? resetValue : data;
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check();
    for (auto &byte : data)
      byte ^= 255;
    model->advance(cycle * 1000 + 750,
                   [&] { model->signal("data").assign(data); });
    check();
  }
  std::puts("PASS: native conditional signal state, nonzero 129-bit initial "
            "value, reset, enable, timed suspension and pre-edge sampling");
}
