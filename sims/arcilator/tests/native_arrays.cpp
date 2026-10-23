#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  std::array<uint16_t, 4> expected{};
  uint64_t time = 0;
  for (unsigned cycle = 0; cycle < 12; ++cycle) {
    bool reset = cycle == 0 || cycle == 7;
    uint16_t input = 0xf123 ^ (cycle * 997);
    uint64_t data40 = (0x1122334455ULL ^ (cycle * 0x9e3779b97f4a7c15ULL)) & ((1ULL << 40)-1);
    runtime->advance(time, [&] {
      runtime->signal("clock").set(0);
      runtime->signal("reset").set(reset);
      runtime->signal("data").set(input);
      runtime->signal("data40").set(data40);
    });
    if (reset)
      expected = {0, 1, 2, 3};
    else
      expected[3] = input;
    runtime->advance(time + 500, [&] { runtime->signal("clock").set(1); });
    for (unsigned i = 0; i < expected.size(); ++i) {
      runtime->advance(time + 501 + i, [&] { runtime->signal("index").set(i); });
      if (runtime->signal("selected").get() != expected[i])
        throw std::runtime_error("native array concat/order/reset mismatch");
    }
    std::vector<uint8_t> bytes;
    for (auto value : expected) {
      bytes.push_back(value & 255);
      bytes.push_back(value >> 8);
    }
    if (runtime->signal("state").copy() != bytes)
      throw std::runtime_error("native array boundary layout mismatch");
    uint64_t expected40 = reset ? 0xABCDE00001ULL : data40;
    uint64_t expanded = expected40 | ((expected40 & 1) ? 0xffffff0000000000ULL : 0);
    if (runtime->signal("expanded").get() != expanded)
      throw std::runtime_error("repeated array concatenation layout/reset mismatch");
    time += 1000;
  }
  std::puts("PASS: native array state, singleton concatenation, dynamic reads, and reset");
}
