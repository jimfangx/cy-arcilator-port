#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
extern "C" void cache_update(unsigned char *, const unsigned char *,
                             const unsigned char *, unsigned char, uint64_t,
                             uint64_t, bool);
extern "C" uint64_t preserved(uint64_t);
extern "C" double floatprobe(double);
int main() {
  for (double value : {-123.5, -0.0, 0.125, 999.0})
    if (floatprobe(value) != value * 1.25 + 2.0 || preserved(998) != 1000)
      throw std::runtime_error("original flags or explicit FP changed");
  uint64_t random = 0x71839412a5c6d7e8ULL;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return random;
  };
  unsigned checks = 0;
  for (unsigned index = 0; index < 256; ++index)
    for (bool enable : {false, true})
      for (unsigned alias = 0; alias < 3; ++alias) {
        std::array<unsigned char, 4098> old, base, out;
        for (auto *buffer : {&old, &base, &out})
          for (auto &b : *buffer)
            b = next();
        std::array<unsigned char, 4096> expected;
        std::memcpy(expected.data(), (enable ? base.data() : old.data()) + 1,
                    4096);
        uint64_t low = next(), high = next();
        if (enable)
          for (unsigned byte = 0; byte < 16; ++byte)
            expected[index * 16 + byte] =
                (byte < 8 ? low : high) >> (8 * (byte % 8));
        auto &destination = alias == 0 ? out : alias == 1 ? old : base;
        auto before = destination.front(), after = destination.back();
        cache_update(destination.data() + 1, old.data() + 1, base.data() + 1,
                     index, low, high, enable);
        if (std::memcmp(destination.data() + 1, expected.data(), 4096) ||
            destination.front() != before || destination.back() != after)
          throw std::runtime_error(
              "full cache image, enable, address, alias or guard changed");
        ++checks;
      }
  std::printf(
      "PASS: %u exact full4096-byte cache images, all256 write addresses, "
      "both enables, independent old/base, 3 aliases and unaligned guards\n",
      checks);
}
