#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  for (unsigned round = 0; round < 3; round++) {
    auto model = create_model(argc, argv);
    model->initialize();
    model->advance(0);
    unsigned count = 77;
    for (unsigned cycle = 0; cycle < 4096; cycle++) {
      bool enabled = (cycle * 13 + round) % 5 < 3;
      model->advance(cycle * 10 + 1, [&] {
        model->signal("clock").set(0);
        model->signal("enable").set(enabled);
      });
      model->advance(cycle * 10 + 5, [&] { model->signal("clock").set(1); });
      count += enabled;
      if (model->signal("observed").get() != count)
        throw std::runtime_error(
            "compilation-unit static did not retain its state");
    }
  }
  std::puts("PASS: 12288 compilation-unit static cycles, retained "
            "initialization, disabled calls and independent model recreation");
}
