#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
using namespace chipyard_arc;
extern "C" double real_identity(double);
extern "C" float short_identity(float);
extern "C" double real_transform(double, double *, double *);
extern "C" float short_transform(float, float *, float *);
extern "C" double real_saved();
extern "C" float short_saved();
template <class T, class U> T bits(U value) {
  static_assert(sizeof(T) == sizeof(U));
  T out;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}
static void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  constexpr std::array<uint64_t, 12> doubles = {0,
                                                0x8000000000000000ull,
                                                0x3ff0000000000000ull,
                                                0xbff4000000000000ull,
                                                1,
                                                0x8000000000000001ull,
                                                0x0010000000000000ull,
                                                0x7fefffffffffffffull,
                                                0x7ff0000000000000ull,
                                                0xfff0000000000000ull,
                                                0x7ff8123456789abcull,
                                                0xfff8abcdef012345ull};
  constexpr std::array<uint32_t, 12> singles = {
      0,           0x80000000u, 0x3f800000u, 0xbfa00000u,
      1,           0x80000001u, 0x00800000u, 0x7f7fffffu,
      0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc54321u};
  unsigned checks = 0;
  for (unsigned round = 0; round < 3; ++round) {
    auto m = create_model(argc, argv);
    m->initialize();
    std::array<svScope, 2> scopes;
    for (unsigned n = 0; n < 2; ++n) {
      std::string path = std::string("RealExportHarness/") +
                         (n ? "second" : "first") + "/rtl/rtl/child";
      scopes[n] = svGetScopeFromName(path.c_str());
      check(scopes[n], "missing floating export scope");
      svSetScope(scopes[n]);
      check(real_saved() == 1.25 * (n + 1),
            "real initializer or scope mismatch");
      check(short_saved() == 2.5f * (n + 1),
            "shortreal initializer or scope mismatch");
      checks += 2;
      for (auto v : doubles) {
        check(bits<uint64_t>(real_identity(bits<double>(v))) == v,
              "real identity lost IEEE bits");
        ++checks;
      }
      for (auto v : singles) {
        check(bits<uint32_t>(short_identity(bits<float>(v))) == v,
              "shortreal identity lost IEEE bits");
        ++checks;
      }
      uint64_t random = 0x39ab721bc0ff3211ull;
      for (unsigned j = 0; j < 1024; ++j) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        check(bits<uint64_t>(real_identity(bits<double>(random))) == random,
              "random real identity changed IEEE bits");
        check(bits<uint32_t>(short_identity(bits<float>(uint32_t(random)))) ==
                  uint32_t(random),
              "random shortreal identity changed IEEE bits");
        checks += 2;
      }
      double y = 1, z = 2;
      bool rejected = false;
      try {
        real_transform(0, nullptr, &z);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      check(rejected, "null real inout accepted");
      rejected = false;
      try {
        real_transform(0, &y, nullptr);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      check(rejected, "null real output accepted");
      checks += 2;
    }
    for (unsigned i = 0; i < 256; ++i) {
      // Binary-exact inputs and coefficients provide independent host oracles,
      // including cancellation, signed results and nonzero FP32 rounding.
      unsigned n = i % 2;
      svSetScope(scopes[n]);
      double x = (int(i) - 117) / 8.0, y = (int(i % 19) - 9) / 16.0;
      double want_y = x * 1.25 + y, want_z = y - x * .5, z = 0;
      double r = real_transform(x, &y, &z);
      check(y == want_y && z == want_z && r == want_z + n + 1,
            "real output/inout/return arithmetic mismatch");
      check(real_saved() == y, "real capture write/read mismatch");
      float sx = (int(i) - 117) / 7.0f, sy = (int(i % 19) - 9) / 13.0f, sz = 0;
      // SV unsized 1.25 and 0.5 promote the expression to real before the cast.
      float want_sy = float(double(sx) * 1.25 + double(sy));
      float want_sz = float(double(sy) - double(sx) * .5);
      float sr = short_transform(sx, &sy, &sz);
      check(sy == want_sy && sz == want_sz &&
                sr == float(want_sz + float(n + 1)),
            "shortreal output/inout/return rounding mismatch");
      check(short_saved() == sy, "shortreal capture write/read mismatch");
      m->advance(i * 2, [&] {
        m->signal("clock").set(0);
        m->signal("enable").set(0);
      });
      auto prefix = n ? "second" : "first";
      check(m->signal(std::string(prefix) + "_real").get() == bits<uint64_t>(y),
            "real export write not visible to RTL");
      check(m->signal(std::string(prefix) + "_short").get() ==
                bits<uint32_t>(sy),
            "shortreal export write not visible to RTL");
      uint64_t value = doubles[i % doubles.size()];
      // Both float stores receive independently selected raw bits via RTL.
      value = (value & 0xffffffff00000000ull) | singles[i % singles.size()];
      m->advance(i * 2 + 1, [&] {
        m->signal("enable").set(1);
        m->signal("input_bits").set(value);
        m->signal("clock").set(1);
      });
      for (auto scope : scopes) {
        svSetScope(scope);
        check(bits<uint64_t>(real_saved()) == value,
              "RTL real write not visible to export");
        check(bits<uint32_t>(short_saved()) == uint32_t(value),
              "RTL shortreal write not visible to export");
      }
      checks += 10;
    }
  }
  std::printf("PASS: %u native real/shortreal export IEEE, scope, "
              "initialization, reference and RTL coherence checks\n",
              checks);
}
