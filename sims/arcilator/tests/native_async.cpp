#include "native/runtime.h"
#include <cstdio>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    auto clock = model->signal("clock"), reset = model->signal("reset");
    clock.set(0); reset.set(0); model->initialize(); model->settle();
    std::vector<uint8_t> expected(257, 0);
    auto reset_value = [&] { expected.assign(257, 0); expected[0] = 5; expected[256] = 4; };
    auto increment = [&] {
      unsigned carry = 3;
      for (unsigned i = 0; i < expected.size(); ++i) {
        unsigned value = expected[i] + carry + (i == 128 ? 2 : 0);
        expected[i] = value; carry = value >> 8;
      }
      expected.back() &= 7;
    };
    unsigned count = 0;
    auto check = [&] {
      if (model->signal("wide").copy() != expected ||
          model->signal("array_wide").copy() != expected ||
          model->signal("struct_wide").copy() != expected || model->signal("count").get() != count)
        throw std::runtime_error("native asynchronous bank or synchronous state mismatch");
    };
    auto drive = [&](uint64_t time, bool c, bool r) {
      model->advance(time, [&] { clock.set(c); reset.set(r); });
    };
    drive(10, 0, 1); reset_value(); check();
    drive(20, 0, 0); check();
    drive(30, 1, 0); increment(); ++count; check();
    // Assert reset between edges while the clock remains high. All
    // foreign banks must reset immediately; native synchronous state must hold.
    drive(35, 1, 1); reset_value(); check();
    drive(36, 1, 0); check();
    drive(40, 0, 0); check();
    drive(50, 1, 0); increment(); ++count; check();
    drive(51, 1, 1); reset_value(); check();
    drive(55, 0, 1); check();
    drive(60, 1, 1); count = 0; check();
    drive(65, 1, 0); check();
    drive(70, 0, 0); check();
    drive(80, 1, 0); increment(); ++count; check();
    bool previous_clock = true, previous_reset = false;
    uint32_t random = 209;
    for (unsigned i = 0; i < 1024; ++i) {
      random = random * 1664525u + 1013904223u;
      bool c = (random >> 20) & 1, r = (random >> 21) & 1;
      bool rise = c && !previous_clock;
      if (rise) { if (r) count = 0; else ++count; }
      if (rise || (r && !previous_reset)) {
        if (r) reset_value(); else increment();
      }
      drive(81 + i, c, r); check();
      previous_clock = c; previous_reset = r;
    }
    std::puts("PASS: Verilator-free 2051-bit integer, array and nested-structure asynchronous banks and synchronous native state");
    return 0;
  } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
