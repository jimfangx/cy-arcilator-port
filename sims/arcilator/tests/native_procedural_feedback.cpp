#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  model->advance(0);
  if (model->flag("test-oscillation")) {
    try {
      model->advance(1, [&] { model->signal("oscillate").set(1); });
    } catch (const std::runtime_error &error) {
      if (std::string(error.what()).find("did not converge") ==
          std::string::npos)
        throw;
      std::puts("PASS: nonconvergent procedural feedback is diagnosed");
      return 0;
    }
    throw std::runtime_error(
        "oscillating procedural feedback was silently accepted");
  }
  unsigned oldData = 0, oldMask = 0, first = 1, q = 0, count = 0;
  bool oldEnable = false, oldClock = false;
  uint64_t time = 0;
  auto step = [&](unsigned data, bool enable, bool clock, bool reset) {
    unsigned mask = 0x5a, next = (data + 1) & 255;
    if (enable && (data != oldData || enable != oldEnable || mask != oldMask)) {
      std::fprintf(stderr, "EXPECTED NATIVE_LOOP_PRINT %u %u\n", data,
                   first ^ mask);
      ++count;
      if (next != first) {
        std::fprintf(stderr, "EXPECTED NATIVE_LOOP_PRINT %u %u\n", data,
                     next ^ mask);
        ++count;
      }
    }
    if (clock && !oldClock)
      q = reset ? 0 : ((next ^ mask) ^ 0xa5);
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("arc_printf_enable").set(1);
      model->signal("reset").set(reset);
      model->signal("data").set(data);
      model->signal("mask").set(mask);
      model->signal("enable").set(enable);
    });
    if (model->signal("first").get() != next ||
        model->signal("second").get() != ((next ^ mask) ^ 0xa5) ||
        model->signal("q").get() != q)
      throw std::runtime_error(
          "procedural feedback final value or pre-edge sample changed");
    oldData = data;
    oldMask = mask;
    oldEnable = enable;
    oldClock = clock;
    first = next;
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned data = (cycle * 17) & 255;
    bool enable = cycle % 7 != 0, reset = cycle % 23 == 0;
    step(data, enable, false, reset);
    step(data, enable, true, reset);
    step(data, enable, true, reset);
    step(data ^ 0xff, enable, true, reset);
    step(data ^ 0xff, enable, false, reset);
  }
  if (count < 1000)
    throw std::runtime_error("insufficient feedback activations");
  std::fprintf(stderr,
               "PASS: %u procedural feedback prints, 2560 steps, converged "
               "values and pre-edge register sampling\n",
               count);
}
