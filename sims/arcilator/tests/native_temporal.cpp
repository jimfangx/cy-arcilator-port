#include "native/runtime.h"
#include <cstdio>
#include <deque>
using namespace chipyard_arc;
static uint64_t calls;
static bool failMode;
extern "C" void __real_chipyard_arc_assert_fail();
extern "C" void __wrap_chipyard_arc_assert_fail() {
  ++calls;
  if (failMode)
    __real_chipyard_arc_assert_fail();
}
struct Tick {
  bool a, b, c;
  unsigned data;
};
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  if (model->flag("test-failure")) {
    failMode = true;
    model->signal("enable").set(1);
    model->signal("a").set(1);
    model->signal("b").set(1);
    model->signal("c").set(0);
    return model->run();
  }
  model->initialize();
  model->advance(0);
  uint64_t time = 0, expected = 0;
  bool old = false;
  unsigned positive = 0, negative = 0;
  std::deque<Tick> pos, neg;
  unsigned pastData = 0;
  auto step = [&](bool clock, unsigned bits, bool reset, bool enable,
                  bool stop) {
    Tick now{bool(bits & 1), bool(bits & 2), bool(bits & 4), (bits >> 3) & 255};
    bool active = enable && !reset;
    if (!active) {
      pos.clear();
      neg.clear();
    }
    if (clock != old) {
      if (active && stop) {
        if (clock) {
          ++positive;
          bool prev = !pos.empty();
          expected += prev && pos[0].a && !now.b;
          expected += now.a && !now.b;
          expected += pos.size() >= 3 && pos[2].a && pos[0].b && !now.c;
          bool antecedent = false;
          for (unsigned d = 0; d < 3 && d < pos.size(); ++d)
            antecedent |= pos[d].a && now.b;
          expected += antecedent && !now.c;
          expected += prev && pos[0].a && now.data != pastData;
          expected += prev && pos[0].a && !(now.b || now.c);
          expected += prev && pos[0].a && pos[0].b && !now.c;
          expected += prev && pos[0].a && now.a && !now.b;
          expected += prev && pos[0].a && now.b && pos[0].b && now.c && now.a;
          expected += pos.size() >= 2 && pos[1].a && pos[0].b && pos[1].c &&
                      now.a && now.b;
          expected += now.c && ((prev && pos[0].a && now.b) ||
                                (pos.size() >= 2 && pos[1].c && now.a));
        } else {
          ++negative;
          expected += !neg.empty() && neg[0].a && !now.c;
        }
      }
      if (clock)
        pastData = now.data;
      if (active) {
        auto &history = clock ? pos : neg;
        history.push_front(now);
        if (history.size() > 4)
          history.pop_back();
      }
    }
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("arc_stop_enable").set(stop);
      model->signal("a").set(now.a);
      model->signal("b").set(now.b);
      model->signal("c").set(now.c);
      model->signal("data").set(now.data);
    });
    if (calls != expected) {
      std::fprintf(stderr,
                   "t=%llu clk=%d data=%x enabled=%d reset=%d calls=%llu "
                   "expected=%llu\n",
                   (unsigned long long)time, clock, bits, enable, reset,
                   (unsigned long long)calls, (unsigned long long)expected);
      throw std::runtime_error("temporal deadline or assertion count changed");
    }
    old = clock;
  };
  unsigned random = 0x83c6e9ab;
  for (unsigned cycle = 0; cycle < 2048; ++cycle) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    bool reset = cycle % 37 == 0, enable = cycle % 17 != 0,
         stop = cycle % 11 != 0;
    step(false, random, reset, enable, stop);
    step(true, random, reset, enable, stop);
    step(true, random ^ 0x3ff, reset, enable, stop);
    // An asynchronous disable pulse must abort pending obligations even if no
    // sampled clock edge occurs during the pulse.
    if (cycle % 13 == 0) {
      step(true, random, false, false, stop);
      step(true, random, false, true, stop);
    }
    step(false, random ^ 0x2ff, reset, enable, stop);
  }
  if (positive < 1000 || negative < 1000 || calls < 1000)
    throw std::runtime_error("insufficient temporal monitor coverage");
  std::printf(
      "PASS: temporal antecedents, exact and ranged delays, repetitions, "
      "nested implications, $past, independent edges, asynchronous "
      "cancellation, stop gating; %llu callbacks over %llu steps\n",
      (unsigned long long)calls, (unsigned long long)time);
}
