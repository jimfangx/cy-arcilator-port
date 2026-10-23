#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
static unsigned stop_callbacks = 0;
extern "C" void __real_chipyard_arc_stop_fail();
extern "C" void __wrap_chipyard_arc_stop_fail() {
  ++stop_callbacks;
  __real_chipyard_arc_stop_fail();
}
int main(int argc, char **argv) {
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto model = create_model(argc, argv);
    model->signal("arc_printf_enable").set(repeat);
    model->signal("arc_stop_enable").set(0);
    model->initialize();
    model->advance(0, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(1);
      model->signal("enable").set(0);
      model->signal("halt").set(0);
      model->signal("data").set(0);
    });
    if (repeat)
      std::fprintf(stderr, "EXPECTED_HIER NATIVE_HIER_PLAIN n=5\n");
    if (model->signal("startup").get() != 12)
      throw std::runtime_error("plain function startup result mismatch");
    unsigned expected = 0;
    for (unsigned cycle = 0; cycle < 32; ++cycle) {
      bool reset = cycle % 11 == 0, enable = cycle % 2 == 0,
           printing = cycle % 3 == 1;
      unsigned data = (cycle * 29 + 5) & 255;
      model->advance(cycle * 2, [&] {
        model->signal("clock").set(0);
        model->signal("reset").set(reset);
        model->signal("enable").set(enable);
        model->signal("data").set(data);
        model->signal("arc_printf_enable").set(printing);
      });
      if (reset)
        expected = 0;
      else if (enable) {
        expected = (data + 7) & 255;
        if (printing)
          std::fprintf(stderr, "EXPECTED_HIER NATIVE_HIER_PLAIN n=%u\n", data);
      }
      model->advance(cycle * 2 + 1, [&] { model->signal("clock").set(1); });
      if (model->signal("observed").get() != expected)
        throw std::runtime_error("plain function clocked result mismatch");
    }
    model->advance(64, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(0);
      model->signal("enable").set(0);
      model->signal("halt").set(1);
      model->signal("arc_printf_enable").set(0);
      model->signal("arc_stop_enable").set(repeat);
    });
    unsigned before = stop_callbacks;
    model->advance(65, [&] { model->signal("clock").set(1); });
    if (stop_callbacks != before + repeat)
      throw std::runtime_error("function fatal ignored native stop control");
    if (model->signal("observed").get() != expected)
      throw std::runtime_error("non-returning fatal published a result");
    model->advance(66, [&] { model->signal("clock").set(0); });
    model->advance(67, [&] { model->signal("clock").set(1); });
    if (stop_callbacks != before + repeat)
      throw std::runtime_error("terminated SV process restarted");
  }
  std::fprintf(
      stderr, "PASS: plain SV function prints obey "
              "startup/clock/reset/enable/print controls, fatal callbacks obey "
              "stop controls, no-return ends the process, and68 result checks "
              "across model recreation\n");
}
