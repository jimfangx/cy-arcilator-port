#include "native/runtime.h"
#include <array>
#include <cstdio>

using namespace chipyard_arc;
static constexpr std::array<unsigned, 7> widths{1, 2, 3, 4, 5, 8, 17};
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  std::array<unsigned, 14> last{}, captured{};
  for (unsigned i = 0; i < last.size(); ++i) {
    auto width = widths[i / 2];
    last[i] = width == 2 || width == 4 || width == 8 ? 0 : width - 1;
  }
  auto winner = [](unsigned requests, unsigned width, unsigned last) {
    for (unsigned step = 1; step <= width; ++step) {
      auto index = (last + step) % width;
      if ((requests >> index) & 1)
        return index;
    }
    return width - 1;
  };
  uint64_t time = 0;
  unsigned checks = 0;
  bool old_clock = false;
  auto evaluate = [&](bool clock, bool reset, unsigned requests, bool unlock) {
    if (clock && !old_clock) {
      for (unsigned i = 0; i < last.size(); ++i) {
        unsigned width = widths[i / 2], mask = (1u << width) - 1;
        unsigned active = requests & mask,
                 selected = winner(active, width, last[i]);
        unsigned hot = active || width == 2 || width == 4 || width == 8
                           ? 1u << selected
                           : 0;
        captured[i] = reset ? 0 : hot;
        if (reset)
          last[i] = width == 2 || width == 4 || width == 8 ? 0 : width - 1;
        else if (!(i & 1) || unlock) {
          if (active || width == 2 || width == 4 || width == 8)
            last[i] = selected;
        }
      }
    }
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("requests").set(requests);
      model->signal("unlock").set(unlock);
    });
    for (unsigned i = 0; i < last.size(); ++i) {
      unsigned width = widths[i / 2], active = requests & ((1u << width) - 1);
      unsigned selected = winner(active, width, last[i]);
      unsigned hot =
          active || width == 2 || width == 4 || width == 8 ? 1u << selected : 0;
      auto name = "rr_" + std::to_string(width) + "_" + std::to_string(i & 1);
      if (model->signal(name + ".valid").get() != bool(active) ||
          model->signal(name + ".hot").get() != hot ||
          (active && model->signal(name + ".index").get() != selected) ||
          model->signal(name + ".captured").get() != captured[i]) {
        std::fprintf(
            stderr,
            "rr mismatch %s time=%llu requests=%x last=%u hot=%llu/%u "
            "capture=%llu/%u\n",
            name.c_str(), (unsigned long long)time, requests, last[i],
            (unsigned long long)model->signal(name + ".hot").get(), hot,
            (unsigned long long)model->signal(name + ".captured").get(),
            captured[i]);
        throw std::runtime_error(
            "original Vortex round-robin arbiter mismatch");
      }
      ++checks;
    }
    old_clock = clock;
  };
  evaluate(false, true, 0, false);
  evaluate(true, true, 0, false);
  uint64_t random = 1979;
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    random = random * 1664525 + 1013904223;
    unsigned requests = cycle < 256 ? cycle : random & 131071;
    bool reset = cycle % 113 == 0, unlock = cycle % 3 != 0;
    evaluate(false, reset, requests, unlock);
    evaluate(true, reset, requests, unlock);
    evaluate(true, reset, requests ^ 131071, !unlock);
    evaluate(true, reset, requests ^ 131071, !unlock);
  }
  std::printf(
      "PASS: %u original Vortex round-robin checks, 14 specializations, "
      "widths 1/2/3/4/5/8/17, lock/unlock, idle, reset and pre-edge "
      "sampling\n",
      checks);
}
