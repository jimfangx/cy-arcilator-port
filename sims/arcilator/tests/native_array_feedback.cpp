#include "native/runtime.h"
#include <cstdio>

using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  uint64_t time = 0;
  unsigned q = 0;
  bool old_clock = false;
  auto step = [&](bool clock, bool reset, unsigned data, unsigned fallback,
                  bool choose, bool choose_second) {
    unsigned a = data & 3, b = (data >> 2) & 3, c = (data >> 4) & 3;
    unsigned first = choose ? fallback & 63 : data;
    unsigned second = choose_second ? (a ^ c) | ((b ^ a) << 2) | ((c ^ b) << 4)
                                    : ((a + b) & 3) | (((b + c) & 3) << 2) |
                                          (((c + a) & 3) << 4);
    if (clock && !old_clock)
      q = reset ? 0 : first ^ second;
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("data").set(data);
      model->signal("fallback").set(fallback);
      model->signal("choose").set(choose);
      model->signal("choose_second").set(choose_second);
    });
    if (model->signal("first").get() != first ||
        model->signal("second").get() != second ||
        model->signal("q").get() != q)
      throw std::runtime_error(
          "aggregate feedback changed an observed array lane");
    old_clock = clock;
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned data = cycle & 63, fallback = (cycle * 29 + 7) & 255;
    bool reset = cycle % 13 == 0, choose = cycle & 64, second = cycle & 128;
    step(false, reset, data, fallback, choose, second);
    step(true, reset, data, fallback, choose, second);
    step(true, !reset, data ^ 63, fallback ^ 255, !choose, !second);
    step(true, !reset, data ^ 63, fallback ^ 255, !choose, !second);
    step(false, reset, data, fallback, choose, second);
  }
  std::puts("PASS: observed lanes of cyclic array mux/injection graphs, opaque "
            "array input, independent branches, write order, reset and clock "
            "sampling (512 cycles)");
}
