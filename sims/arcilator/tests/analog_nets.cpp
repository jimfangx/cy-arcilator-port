#include "native/runtime.h"
#include <cstdio>
#include <random>
#include "analog_cases.h"
using namespace chipyard_arc;
static size_t checks;
static void check(bool ok) {
  ++checks;
  if (!ok) throw std::runtime_error("native analog test failed at check " + std::to_string(checks));
}
template <typename F> static void rejects(F f) {
  bool threw = false;
  try { f(); } catch (const std::runtime_error &) { threw = true; }
  check(threw);
}
static void evaluate(void *) {}
class LifetimeProbe : public Binding {
  Signal cached;
  std::vector<uint8_t> expected;
  bool &passed;
public:
  LifetimeProbe(Runtime &r, Signal s, std::vector<uint8_t> value, bool &ok)
      : Binding(r, "LifetimeProbe"), cached(s), expected(std::move(value)),
        passed(ok) {}
  ~LifetimeProbe() override { passed = cached.copy() == expected; }
};
static void resolution(unsigned bits) {
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "AnalogTest", nullptr};
  Runtime r(0, nullptr, info, evaluate);
  r.add_analog_signal("pad", bits);
  auto pad = r.signal("pad");
  Binding a(r, "a"), b(r, "b");
  a.ports.emplace("x", pad); a.ports.emplace("y", pad);
  b.ports.emplace("x", pad);
  r.add_signal("digital", 0, 1, false);
  rejects([&] { r.add_analog_signal("pad", bits); });
  rejects([&] { r.add_analog_signal("zero", 0); });
  // Other net registrations cannot invalidate a cached Signal address.
  for (unsigned i = 0; i != 100; ++i)
    r.add_analog_signal("extra" + std::to_string(i), i + 1);
  check(r.signal("pad").data == pad.data);
  r.initialize(); r.settle();
  check(pad.copy() == std::vector<uint8_t>(pad.bytes()));
  std::mt19937_64 random(7251 + bits);
  std::vector<uint8_t> values[3], masks[3];
  for (unsigned step = 0; step != 200; ++step) {
    for (unsigned d = 0; d != 3; ++d) {
      values[d].resize(pad.bytes()); masks[d].resize(pad.bytes());
      for (size_t i = 0; i < pad.bytes(); ++i) {
        values[d][i] = random();
        masks[d][i] = step % 5 ? uint8_t(random()) : step % 10 ? 0 : 255;
      }
      auto &binding = d == 2 ? b : a;
      const char *name = d == 1 ? "y" : "x";
      if (bits <= 64 && step % 2) {
        uint64_t v = 0, m = 0;
        std::memcpy(&v, values[d].data(), pad.bytes());
        std::memcpy(&m, masks[d].data(), pad.bytes());
        binding.drive(name, v, m);
      } else binding.drive(name, values[d], masks[d]);
    }
    // Independent per-bit oracle, including conflicts and released drivers.
    std::vector<uint8_t> expected(pad.bytes());
    for (unsigned bit = 0; bit < bits; ++bit) {
      bool one = false, zero = false;
      for (unsigned d = 0; d != 3; ++d)
        if ((masks[d][bit / 8] >> (bit % 8)) & 1) {
          if ((values[d][bit / 8] >> (bit % 8)) & 1) one = true;
          else zero = true;
        }
      if (one && !zero) expected[bit / 8] |= 1u << (bit % 8);
    }
    r.settle(); check(pad.copy() == expected);
    check(pad.data == r.signal("pad").data);
    if (bits % 8) check(!(pad.data[pad.bytes() - 1] >> (bits % 8)));
    // Reject malformed updates without replacing the driver's previous value.
    auto short_value = values[0]; short_value.pop_back();
    rejects([&] { a.drive("x", short_value, masks[0]); });
    rejects([&] { a.drive("x", values[0], short_value); });
    rejects([&] { r.drive_analog(&a, "x", Signal{pad.data, bits + 1}, values[0], masks[0]); });
    rejects([&] { r.drive_analog(&a, "x", r.signal("digital"), 1, 1); });
    if (bits > 64) rejects([&] { a.drive("x", 1, 1); });
    r.settle(); check(pad.copy() == expected);
  }
}
int main() {
  bool lifetime_passed = false;
  {
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "LifetimeTest", nullptr};
    Runtime r(0, nullptr, info, evaluate);
    r.add_analog_signal("pad", 257);
    auto pad = r.signal("pad");
    std::vector<uint8_t> value(pad.bytes(), 0x5a), mask(pad.bytes(), 255);
    value.back() &= 1;
    auto probe = std::make_unique<LifetimeProbe>(r, pad, value, lifetime_passed);
    probe->ports.emplace("pad", pad);
    probe->drive("pad", value, mask);
    r.add_binding(std::move(probe)); r.initialize(); r.settle();
  }
  check(lifetime_passed);
  for (unsigned bits : {1, 7, 8, 31, 63, 64, 65, 73, 127, 128, 129, 257, 1027})
    resolution(bits);
  register_builtin_bindings();
  for (unsigned bits : {65, 73, 127, 128, 129, 257, 1027}) {
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "ConstantTest", nullptr};
    Runtime r(0, nullptr, info, evaluate);
    r.add_analog_signal("pad", bits);
    auto binding = make_binding(r, "AnalogConst");
    auto *constant = binding.get();
    constant->ports.emplace("io", r.signal("pad"));
    constant->parameters = {{"WIDTH", std::to_string(bits)}, {"CONST", "0"}};
    r.add_binding(std::move(binding)); r.initialize();
    for (const auto &test : analog_cases) if (test.bits == bits) {
      constant->parameters["CONST"] = test.literal;
      constant->configure(); r.settle();
      check(r.signal("pad").copy() == test.expected);
    }
    auto before = r.signal("pad").copy();
    for (const char *bad : {"", "-", "+", "0x", "12z", "0xgg", "08", "1 2"}) {
      constant->parameters["CONST"] = bad;
      rejects([&] { constant->configure(); });
      r.settle(); check(r.signal("pad").copy() == before);
    }
    constant->parameters["CONST"] = "1";
    constant->parameters["WIDTH"] = std::to_string(bits + 1);
    rejects([&] { constant->configure(); });
  }
  std::printf("PASS: %zu analog resolution, driver identity, stable storage and constant checks\n", checks);
}
