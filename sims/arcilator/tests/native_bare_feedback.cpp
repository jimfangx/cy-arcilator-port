#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
static uint64_t calls;
extern "C" uint32_t native_bare_feedback_tick(uint32_t data, uint32_t state) {
  ++calls;
  return (state >> 1) | (data & UINT32_C(0x80000000));
}
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  model->advance(0);
  if (calls != 1)
    throw std::runtime_error("bare feedback DPI initialization count changed");
  uint64_t expected = 1, time = 0;
  uint32_t oldData = 0, q = 0;
  bool oldClock = false;
  auto step = [&](uint32_t data, bool clock, bool reset) {
    uint32_t result = (data >> 31) ? UINT32_MAX : 0;
    if (data != oldData)
      expected += ((data ^ oldData) >> 31) ? 33 : 1;
    if (clock && !oldClock)
      q = reset ? 0 : result;
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("data").set(data);
    });
    if (calls != expected || model->signal("value").get() != result ||
        model->signal("q").get() != q) {
      std::fprintf(stderr,
                   "t=%llu calls=%llu expected=%llu data=%x value=%llx q=%llx "
                   "expectedq=%x\n",
                   (unsigned long long)time, (unsigned long long)calls,
                   (unsigned long long)expected, data,
                   (unsigned long long)model->signal("value").get(),
                   (unsigned long long)model->signal("q").get(), q);
      throw std::runtime_error(
          "bare DPI fixed point, callback count or clock sampling changed");
    }
    oldData = data;
    oldClock = clock;
  };
  for (unsigned cycle = 0; cycle < 256; ++cycle) {
    uint32_t data = (((cycle / 3) & 1u) << 31) | (cycle * 71);
    bool reset = cycle % 17 == 0;
    step(data, false, reset);
    step(data, true, reset);
    step(data, true, reset);
    step(data ^ 23, true, reset);
    step(data ^ 23, false, reset);
  }
  std::printf("PASS: %llu bare-DPI callbacks over1280steps, converged values, "
              "unused data bits, stable evaluations and pre-edge sampling\n",
              (unsigned long long)calls);
}
