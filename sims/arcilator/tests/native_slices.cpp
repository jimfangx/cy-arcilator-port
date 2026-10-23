#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t bits = 0, hi = 0, upper = 0, lower = 0;
  std::array<uint64_t, 4> lanes{};
  bool previousA = false, previousB = false;
  for (unsigned step = 0; step < 180; ++step) {
    bool clockA = (step / 2) & 1, clockB = (step / 3) & 1;
    bool reset = step < 8 || (step >= 23 && step < 35);
    bool enable = step % 4 != 2;
    uint64_t data = (step * 71 + 9) & 255, index = step & 3;
    bool risingA = clockA && !previousA, risingB = clockB && !previousB;
    auto oldBits = bits, oldHi = hi, oldUpper = upper;
    auto oldLanes = lanes;
    for (unsigned i = 0; i < 8; ++i)
      if (i % 2 ? risingB : risingA) {
        uint64_t next = reset ? (0x96 >> i) & 1
                              : ((data >> i) ^ (oldBits >> ((i + 1) % 8))) & 1;
        if (reset || enable)
          bits = (bits & ~(uint64_t(1) << i)) | (next << i);
      }
    for (unsigned i = 0; i < 4; ++i)
      if (i % 2 ? risingB : risingA)
        if (reset || enable)
          lanes[i] =
              reset
                  ? i + 17
                  : (oldLanes[i] + oldLanes[(i + 1) % 4] + data + i + 1) & 511;
    if (risingA && (reset || enable)) {
      hi = reset ? 0x123 : (oldHi + data + oldUpper + 1) & 511;
      lower = reset ? 2 : (lower + data + oldHi) & 3;
    }
    if (risingB && (reset || enable))
      upper = reset ? 5 : (upper ^ data) & 7;
    model->advance(step * 1000, [&] {
      model->signal("clock").set(clockA);
      model->signal("clock_b").set(clockB);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("data").set(data);
      model->signal("index").set(index);
    });
    auto check = [&](unsigned selected, uint64_t currentData) {
      uint64_t packedLanes = 0;
      for (unsigned i = 0; i < 4; ++i)
        packedLanes |= lanes[i] << (i * 9);
      std::array<uint64_t, 5> prefix{currentData};
      for (unsigned i = 0; i < 4; ++i)
        prefix[i + 1] = (prefix[i] ^ (prefix[i] << (1 << i))) & 65535;
      if (model->signal("bits_q").get() != bits ||
          model->signal("lanes_q").get() != packedLanes ||
          model->signal("fields_q").get() != (hi << 5 | upper << 2 | lower) ||
          model->signal("selected").get() != lanes[selected] ||
          model->signal("prefix_q").get() != (prefix[4] ^ prefix[selected])) {
        std::fprintf(stderr, "partial write mismatch at step %u\n", step);
        throw std::runtime_error("native partial write/clock mismatch");
      }
    };
    check(index, data);
    // Changing data and read indices between edges must leave all banks alone.
    model->advance(step * 1000 + 500, [&] {
      model->signal("data").set(data ^ 255);
      model->signal("index").set(index ^ 3);
    });
    check(index ^ 3, data ^ 255);
    previousA = clockA;
    previousB = clockB;
  }
  std::puts("PASS: separate bit, packed-array and nested-struct writes, two "
            "clocks, enable, reset, feedback and dynamic reads");
}
