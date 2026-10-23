#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto r = chipyard_arc::create_model(argc, argv);
  r->initialize();
  auto expect = [&](unsigned first, unsigned second) {
    if (r->signal("first").get() != first || r->signal("second").get() != second)
      throw std::runtime_error("imported RTL state/parameter mismatch");
  };
  r->advance(0, [&] { r->signal("reset").set(1); });
  r->advance(5, [&] { r->signal("clock").set(1); });
  expect(0xa5, 0x3c);
  r->advance(10, [&] { r->signal("clock").set(0); r->signal("reset").set(0); r->signal("data").set(0xfe); });
  r->advance(15, [&] { r->signal("clock").set(1); });
  expect(0xa5, 0x3c);
  r->advance(20, [&] { r->signal("clock").set(0); r->signal("enable").set(1); });
  r->advance(25, [&] { r->signal("clock").set(1); });
  expect(0xec, 0xec);
  r->advance(27, [&] { r->signal("data").set(0x55); });
  expect(0xec, 0xec);
  r->advance(30, [&] { r->signal("clock").set(0); });
  r->advance(35, [&] { r->signal("clock").set(1); });
  expect(0x47, 0x47);
  r->advance(500, [&] { r->signal("clock").set(0); });
  r->advance(505, [&] { r->signal("clock").set(1); });
  expect(0x47, 0x47);
  std::puts("PASS: native RTL import, parameter specializations, hierarchy, clock conversions, and edge sampling");
}
