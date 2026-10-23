#include "native/runtime.h"
#include <cstdio>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    model->initialize();
    uint64_t time = 0;
    uint32_t calls = 0;
    uint8_t result = 0;
    auto check = [&] {
      if (model->signal("calls").get() != calls ||
          model->signal("result").get() != result)
        throw std::runtime_error(
            "case item side effects or first-match order changed");
    };
    for (unsigned cycle = 0; cycle < 8192; ++cycle) {
      uint8_t value = cycle % 3 == 0 ? 0 : cycle % 3 == 1 ? 1 : cycle * 37;
      model->advance(time++, [&] {
        model->signal("clock").set(0);
        model->signal("value").set(value);
      });
      check();
      model->advance(time++, [&] { model->signal("clock").set(1); });
      calls += value == 0 ? 1 : 3;
      result = value < 2 ? value : 255;
      check();
      model->advance(time++, [&] { model->signal("value").set(value ^ 0x80); });
      check();
    }
    std::puts(
        "PASS: 24576 case-item effect/order checks; first match stops later "
        "calls, duplicate arguments retain separate captured-counter updates, "
        "and stable clocks preserve state");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
