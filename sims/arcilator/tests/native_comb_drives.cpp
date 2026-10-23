#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t captured = 0;
  unsigned checks = 0;
  for (unsigned vector = 0; vector < 2048; ++vector) {
    uint64_t selectors = (vector * 1979 + 7) & 4095;
    uint64_t enabled = vector & 15, flags = (vector / 7) & 15;
    bool valid = vector % 7 != 1, reset = vector % 19 == 0;
    auto reference = [&](uint64_t selections, uint64_t active, bool live) {
      uint64_t mask = 0, sfu = 0, any = 0;
      for (unsigned i = 0; i < 4; ++i) {
        if ((active >> i) & 1) {
          any = 1;
          auto index = (selections >> (i * 3)) & 7;
          if (index < 5)
            mask |= uint64_t(1) << index;
          if (index == 2)
            sfu |= uint64_t(1) << ((flags >> i) & 1);
        }
      }
      unsigned bank = live ? 0 : 1;
      return any << (14 + bank) | sfu << (10 + bank * 2) | mask << (bank * 5);
    };
    auto expected = reference(selectors, enabled, valid);
    auto check = [&](uint64_t packed) {
      if (model->signal("mask").get() != (packed & 1023) ||
          model->signal("sfu").get() != ((packed >> 10) & 15) ||
          model->signal("any").get() != (packed >> 14) ||
          model->signal("captured").get() != captured)
        throw std::runtime_error(
            "native combinational drives or edge sampling mismatch");
      ++checks;
    };
    model->advance(vector * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("selectors").set(selectors);
      model->signal("enabled").set(enabled);
      model->signal("flags").set(flags);
      model->signal("valid").set(valid);
    });
    check(expected);
    captured = reset ? 0xad37 : expected;
    model->advance(vector * 1000 + 500, [&] { model->signal("clock").set(1); });
    check(expected);
    auto nextSelectors = selectors ^ 4095, nextEnabled = enabled ^ 15;
    model->advance(vector * 1000 + 750, [&] {
      model->signal("selectors").set(nextSelectors);
      model->signal("enabled").set(nextEnabled);
      model->signal("valid").set(!valid);
    });
    check(reference(nextSelectors, nextEnabled, !valid));
  }
  std::printf("PASS: %u combinational drive checks, nested branches, dynamic "
              "bit writes, input changes, and registered edge sampling\n",
              checks);
}
