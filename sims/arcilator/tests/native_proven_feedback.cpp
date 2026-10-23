#include "native/runtime.h"
#include <cstdio>
int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t time = 0, sampled = 0;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    unsigned a = (cycle * 367 + 5) & 8191, b = (cycle * 997 + 31) & 8191;
    unsigned mode = cycle % 4, value = mode == 0 ? a : mode == 1 ? b : 0;
    auto check = [&] {
      if (model->signal("x").get() != value ||
          model->signal("y").get() != value ||
          model->signal("sampled").get() != sampled)
        throw std::runtime_error(
            "solver-proved feedback changed values or edge sampling");
    };
    model->advance(time++, [&] {
      model->signal("clock").set(0);
      model->signal("mode").set(mode);
      model->signal("a").set(a);
      model->signal("b").set(b);
    });
    check();
    sampled = value;
    model->advance(time++, [&] { model->signal("clock").set(1); });
    check();
    model->advance(time++);
    check();
  }
  std::puts(
      "PASS: 4096 solver-proved 13-bit feedback cycles, mutually exclusive "
      "decode, input changes, stable values and sequential pre-edge sampling");
}
