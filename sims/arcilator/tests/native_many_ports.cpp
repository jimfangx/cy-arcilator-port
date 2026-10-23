#include "native/runtime.h"
#include <cstdio>

namespace {
constexpr unsigned count = 16500;
uint8_t a(unsigned i, unsigned mode) { return (i * 17) ^ (i >> 8) ^ (mode * 93); }
uint8_t b(unsigned i, unsigned mode) { return (i * 83) ^ (i >> 6) ^ 0x59 ^ (mode * 37); }
void inputs(chipyard_arc::Runtime &runtime, unsigned mode) {
  for (unsigned i = 0; i < count; ++i) {
    runtime.signal("a" + std::to_string(i)).set(a(i, mode));
    runtime.signal("b" + std::to_string(i)).set(b(i, mode));
  }
}
void outputs(chipyard_arc::Runtime &runtime, unsigned mode) {
  for (unsigned i = 0; i < count; ++i)
    if (runtime.signal("o" + std::to_string(i)).get() != unsigned(a(i, mode) ^ b(i, mode)))
      throw std::runtime_error("incorrect native high-arity output " + std::to_string(i));
}
}

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  runtime->signal("arc_printf_enable").set(1);
  inputs(*runtime, 0);
  runtime->advance(0);
  outputs(*runtime, 0);
  runtime->advance(500, [&] { runtime->signal("clock").set(1); });
  inputs(*runtime, 1);
  runtime->advance(501);
  outputs(*runtime, 1);
  runtime->advance(1000, [&] { runtime->signal("clock").set(0); });
  runtime->advance(1500, [&] { runtime->signal("clock").set(1); });
  std::puts("PASS: 33000 native input ports, complete output checks, and shared clocked literals");
}
