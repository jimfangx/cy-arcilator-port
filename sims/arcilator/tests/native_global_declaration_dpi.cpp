#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
namespace {
std::array<unsigned, 5> calls{};
std::array<unsigned, 2> locals{}, observers{};
svScope outside;
constexpr uint64_t seed = 0xfedcba9876543201ULL,
                   derived = seed ^ 0x8100010000000077ULL;
void require(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
void ambient() {
  require(svGetScope() == outside, "global initializer changed ambient scope");
}
unsigned index(int tag) {
  require(tag == 17 || tag == 29, "global module tag ABI");
  return tag == 29;
}
uint64_t local(int tag) { return derived ^ (uint64_t(tag) * 0x10203); }
void ready() {
  require(calls == std::array<unsigned, 5>{{1, 1, 1, 1, 1}},
          "module initializer preceded complete package initialization");
}
} // namespace
extern "C" uint64_t global_seed(int tag) {
  ambient();
  require(tag == 43 && ++calls[0] == 1 && calls[1] == 0,
          "global initializer repeated or wrong seed order");
  return seed;
}
extern "C" uint64_t global_step(uint64_t value, int tag) {
  ambient();
  require(tag == 7 && calls[0] == 1 && ++calls[1] == 1 && calls[2] == 0 &&
              value == seed,
          "global dependent initializer saw an uninitialized value");
  return derived;
}
extern "C" uint64_t global_discard(int tag) {
  ambient();
  require(tag == 9 && calls[1] == 1 && ++calls[2] == 1 && calls[3] == 0,
          "unused global initializer dropped/repeated/reordered");
  return 0x8000000076543210ULL;
}
extern "C" double global_real(int tag) {
  ambient();
  require(tag == 43 && calls[2] == 1 && ++calls[3] == 1 && calls[4] == 0,
          "global real initializer ABI/order");
  return 2.0;
}
extern "C" float global_short(int tag) {
  ambient();
  require(tag == 43 && calls[3] == 1 && ++calls[4] == 1,
          "global shortreal initializer ABI/order");
  return -0x1.23456p-10f;
}
extern "C" uint64_t global_module_seed(uint64_t value, uint64_t upper,
                                       int tag) {
  ambient();
  ready();
  require(!std::fetestexcept(FE_DIVBYZERO),
          "skipped global initializer evaluated floating expression");
  require(value == derived && upper == seed,
          "module initializer saw uninitialized global storage");
  auto i = index(tag);
  require(++locals[i] <= (i ? 1u : 2u), "module initializer repeated");
  return local(tag);
}
extern "C" void global_observe(int tag, uint64_t value, uint64_t local_value) {
  ambient();
  ready();
  require(locals == std::array<unsigned, 2>{{2, 1}} && value == seed &&
              local_value == local(tag),
          "time-zero observer saw uninitialized package/module values");
  auto i = index(tag);
  require(++observers[i] <= (i ? 1u : 2u), "global observer repeated");
}
// Fault-only entry point selected by the test's model-IR mutation.
extern "C" uint64_t chipyard_arc_signal_claim_startup_fault(uint64_t) {
  return 1;
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected global trace path");
    FILE *f = std::fopen(argv[1], "wb");
    require(f, "trace open");
    chipyard_arc::DpiScope caller("global-initializer-caller");
    outside = caller.handle();
    auto saved = svSetScope(outside);
    const std::array<const char *, 21> names{
        {"first",          "first_derived",   "first_local",
         "first_observed", "first_metric",    "first_wide",
         "first_small",    "second",          "second_derived",
         "second_local",   "second_observed", "second_metric",
         "second_wide",    "second_small",    "third",
         "third_derived",  "third_local",     "third_observed",
         "third_metric",   "third_wide",      "third_small"}};
    for (unsigned recreation = 0; recreation < 2; ++recreation) {
      calls = {};
      locals = {};
      observers = {};
      std::feclearexcept(FE_ALL_EXCEPT);
      auto model = chipyard_arc::create_model(argc, argv);
      model->initialize();
      ambient();
      ready();
      require(locals == std::array<unsigned, 2>{{2, 1}},
              "missing independent module initializer");
      for (unsigned sample = 0; sample < 24; ++sample) {
        bool clock = sample & 1, reset = sample >= 5 && sample < 10,
             write_a = sample % 4 < 2;
        uint32_t data = 0xf1234567u + sample * 0x10203u;
        model->advance(sample * 1000, [&] {
          model->signal("clock").set(clock);
          model->signal("reset").set(reset);
          model->signal("write_a").set(write_a);
          model->signal("write_b").set(!write_a);
          model->signal("data").set(data);
        });
        for (unsigned held = 0; held < 2; ++held) {
          if (held)
            model->advance(sample * 1000 + 1, [&] {
              model->signal("write_a").set(!write_a);
              model->signal("write_b").set(write_a);
              model->signal("data").set(data ^ 0xffffffffu);
            });
          ambient();
          ready();
          require(locals == std::array<unsigned, 2>{{2, 1}} &&
                      observers == locals,
                  "startup/ordinary initial callbacks repeated");
          for (auto name : names) {
            auto bytes = model->signal(name).copy();
            require(std::fwrite(bytes.data(), 1, bytes.size(), f) ==
                        bytes.size(),
                    "trace write");
          }
        }
      }
    }
    svSetScope(saved);
    require(std::fclose(f) == 0, "trace close");
    std::puts("PASS shared native global DPI initialization: dependencies, "
              "three instances/two specializations, integral/real/shortreal, "
              "129-bit state, writes, held events and recreation");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
