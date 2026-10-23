#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  runtime->signal("arc_printf_enable").set(1);
  runtime->signal("arc_stop_enable").set(1);
  auto expect = [&](uint64_t state, uint64_t ticks, uint64_t stamp,
                    uint64_t lanes, uint64_t clockStamp, uint64_t heldStamp) {
    if (runtime->signal("q").get() != state ||
        runtime->signal("ticks").get() != ticks ||
        runtime->signal("stamp").get() != stamp ||
        runtime->signal("clock_stamp").get() != clockStamp ||
        runtime->signal("held_stamp").get() != heldStamp ||
        runtime->signal("payload").get() != ((state << 16) | lanes) ||
        runtime->signal("inverted").get() != (lanes ^ 0x5aa5))
      throw std::runtime_error(
          "imported procedural RTL or aggregate port mismatch");
  };
  runtime->advance(0, [&] {
    runtime->signal("reset").set(1);
    runtime->signal("lanes").set(0xabcd);
  });
  expect(0, 0, 0, 0xabcd, 0, 0);
  runtime->advance(1, [&] { runtime->signal("clock").set(1); });
  expect(0, 0, 0, 0xabcd, 0, 0);
  runtime->advance(2, [&] {
    runtime->signal("clock").set(0);
    runtime->signal("reset").set(0);
    runtime->signal("enable").set(1);
    runtime->signal("data").set(7);
  });
  expect(0, 1, 2, 0xabcd, 0, 0);
  runtime->advance(3, [&] { runtime->signal("clock").set(1); });
  expect(10, 2, 3, 0xabcd, 3, 0);
  runtime->advance(4, [&] {
    runtime->signal("data").set(5);
    runtime->signal("lanes").set(0x1837);
  });
  expect(10, 2, 3, 0x1837, 3, 0);
  runtime->advance(5, [&] { runtime->signal("clock").set(0); });
  expect(10, 3, 5, 0x1837, 3, 2);
  runtime->advance(6, [&] { runtime->signal("clock").set(1); });
  expect(18, 4, 6, 0x1837, 6, 2);
  runtime->advance(20);
  expect(18, 4, 6, 0x1837, 6, 2);
  std::puts("PASS: imported SV state, one print per edge, constant strings, "
            "1500 fs delays, context time, and aggregate boundaries");
}
