#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto runtime = create_model(argc, argv);
  runtime->initialize();
  runtime->advance(0);
  unsigned previousData = 0, q = 0;
  bool previousEnable = false, previousLiteral = false, previousClock = false;
  unsigned count = 0, literals = 0;
  uint64_t time = 0;
  auto step = [&](unsigned data, bool enable, bool literal, bool clock,
                  bool reset, bool printing) {
    if (clock && !previousClock)
      q = reset ? 0 : (data ^ 0x5a);
    if (printing) {
      if ((data != previousData || enable != previousEnable) && enable) {
        std::fprintf(stderr, "EXPECTED NATIVE_COMB_PRINT %u\n", data);
        ++count;
      }
      if (literal != previousLiteral && literal) {
        std::fprintf(stderr, "EXPECTED NATIVE_COMB_LITERAL\n");
        ++literals;
      }
    }
    runtime->advance(++time, [&] {
      runtime->signal("clock").set(clock);
      runtime->signal("reset").set(reset);
      runtime->signal("data").set(data);
      runtime->signal("enable").set(enable);
      runtime->signal("literal_enable").set(literal);
      runtime->signal("arc_printf_enable").set(printing);
    });
    if (runtime->signal("result").get() != (data ^ 0x5a) ||
        runtime->signal("q").get() != q)
      throw std::runtime_error(
          "print-only combinational result or pre-edge sampling changed");
    previousData = data;
    previousEnable = enable;
    previousLiteral = literal;
    previousClock = clock;
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned data = (cycle * 17) & 255;
    bool enable = cycle % 7 != 0, literal = cycle % 11 == 0;
    bool reset = cycle % 23 == 0;
    step(data, enable, literal, false, reset, true);
    step(data, enable, literal, true, reset, true);
    step(data, enable, literal, true, reset, true);
    step(data ^ 0xff, enable, literal, true, reset, true);
    step(data ^ 0xff, enable, literal, false, reset, true);
  }
  if (count < 500 || literals < 20)
    throw std::runtime_error("insufficient print activations");
  std::fprintf(stderr,
               "PASS: %u dynamic prints, %u literal prints, 2560 steps, stable "
               "settlements and pre-edge register sampling\n",
               count, literals);
}
