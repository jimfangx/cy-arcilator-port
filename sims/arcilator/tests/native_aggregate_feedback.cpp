#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  model->advance(0);
  unsigned oldData = 0, q = 0, count = 0;
  bool oldEnable = false, oldClock = false;
  uint64_t time = 0;
  auto step = [&](unsigned data, bool enable, bool clock, bool reset) {
    if (enable && (data != oldData || enable != oldEnable)) {
      std::fprintf(stderr, "EXPECTED NATIVE_AGG_FEEDBACK %u\n", oldData);
      ++count;
      if (data != oldData) {
        std::fprintf(stderr, "EXPECTED NATIVE_AGG_FEEDBACK %u\n", data);
        ++count;
      }
    }
    if (clock && !oldClock)
      q = reset ? 0 : data;
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("data").set(data);
      model->signal("enable").set(enable);
      model->signal("arc_printf_enable").set(1);
    });
    unsigned __int128 wide = 0;
    for (unsigned i = 0; i < 5; ++i)
      wide = (wide << 13) | data;
    if (model->signal("lo").get() != data ||
        model->signal("a0").get() != (data ^ 291) ||
        model->signal("a1").get() != ((data + 7) & 8191) ||
        model->signal("a2").get() != data ||
        model->signal("wide_lo").get() != (uint64_t)wide ||
        model->signal("wide_hi").get() != (unsigned)(wide >> 64) ||
        model->signal("q").get() != q)
      throw std::runtime_error(
          "117-bit aggregate feedback packing or pre-edge sampling changed");
    oldData = data;
    oldEnable = enable;
    oldClock = clock;
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned data = (cycle * 137) & 8191;
    bool enable = cycle % 7 != 0, reset = cycle % 23 == 0;
    step(data, enable, false, reset);
    step(data, enable, true, reset);
    step(data, enable, true, reset);
    step(data ^ 8191, enable, true, reset);
    step(data ^ 8191, enable, false, reset);
  }
  std::fprintf(stderr,
               "PASS: 117-bit struct/array feedback, 65-bit field, %u exact "
               "print events, 2560 steps, and pre-edge register samples\n",
               count);
}
