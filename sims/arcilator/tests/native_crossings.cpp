#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t first = 0, second = 0, leaf1 = 0, leaf2 = 0, sampled = 0;
  for (unsigned cycle = 0; cycle < 40; ++cycle) {
    uint64_t data = (cycle * 19 + 7) & 255;
    bool reset = cycle == 0 || cycle == 17;
    auto check = [&] {
      if (model->signal("first").get() != first ||
          model->signal("second").get() != second ||
          model->signal("sampled").get() != sampled)
        throw std::runtime_error(
            "native-to-RTL-to-native state or edge mismatch");
    };
    model->advance(cycle * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("data").set(data);
    });
    check();
    sampled = first;
    first = reset ? 0x5a : leaf1 ^ 0x5a;
    second = reset ? 0x3c : leaf2 ^ 0x3c;
    leaf1 = reset ? 5 : (data + 3) & 255;
    leaf2 = reset ? 5 : ((data ^ 128) + 3) & 255;
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check();
    model->advance(cycle * 1000 + 750,
                   [&] { model->signal("data").set(data ^ 255); });
    check();
  }
  std::puts("PASS: native-to-RTL-to-native hierarchy, independent instances, "
            "binding-name collision, hierarchical $bits, clock conversions, "
            "and pre-edge sampling");
}
