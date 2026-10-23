#include "native/runtime.h"
#include <cstdio>

using namespace chipyard_arc;
static uint64_t calls;
static bool fail_mode;
extern "C" void __real_chipyard_arc_assert_fail();
extern "C" void __wrap_chipyard_arc_assert_fail() {
  ++calls;
  if (fail_mode)
    __real_chipyard_arc_assert_fail();
}
static void check(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  if (model->flag("test-failure")) {
    fail_mode = true;
    model->signal("a").set(1);
    model->signal("b").set(0);
    model->signal("c").set(0);
    model->signal("d").set(1);
    model->signal("enable").set(1);
    return model->run();
  }
  model->initialize();
  // Settle the initially inverted clock while checks are disabled.
  model->advance(0);
  bool old_clock = false, state = false;
  uint64_t expected = 0, time = 0;
  unsigned positive = 0, negative = 0;
  auto step = [&](bool clock, unsigned data, bool reset, bool enable, bool stop,
                  bool constants) {
    bool a = data & 1, b = data & 2, c = data & 4, d = data & 8;
    if (clock != old_clock) {
      if (enable && !reset && stop) {
        if (clock) {
          expected += d && !((!a || b) && !c);
          expected += d && !state;
          expected += constants;
          ++positive;
        } else {
          expected += a && b && c;
          ++negative;
        }
        expected += b && !c;
      }
      if (clock)
        state = !reset && d;
    }
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("a").set(a);
      model->signal("b").set(b);
      model->signal("c").set(c);
      model->signal("d").set(d);
      model->signal("enable").set(enable);
      model->signal("constant_check").set(constants);
      model->signal("arc_stop_enable").set(stop);
    });
    if (calls != expected) {
      std::fprintf(
          stderr,
          "time=%llu clock=%d data=%x state=%d got=%llu expected=%llu\n",
          (unsigned long long)time, clock, data, state,
          (unsigned long long)calls, (unsigned long long)expected);
      throw std::runtime_error(
          "native assertion sampling or callback count changed");
    }
    check(model->signal("state").get() == state,
          "assertion fixture register timing changed");
    old_clock = clock;
  };
  for (unsigned cycle = 0; cycle < 256; ++cycle) {
    bool reset = cycle % 17 == 0, enable = cycle % 7 != 0;
    bool stop = cycle % 11 != 0, constants = cycle % 5 == 0;
    unsigned data = cycle & 15;
    step(false, data, reset, enable, stop, constants);
    step(true, data, reset, enable, stop, constants);
    step(true, data ^ 15, reset, enable, stop, constants);
    step(true, data ^ 15, reset, enable, stop, constants);
    step(false, data ^ 15, reset, enable, stop, constants);
    step(false, data, reset, enable, stop, constants);
  }
  check(positive > 100 && negative > 100 && calls > 100,
        "assertion fixture did not exercise active checks");
  std::printf(
      "PASS: native pointwise LTL, positive/negative/both edges, pre-edge "
      "sampling, reset/enables, constants and exact %llu assertion callbacks\n",
      (unsigned long long)calls);
}
