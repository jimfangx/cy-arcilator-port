// Independent scalar oracle for packed SSA aggregate code generation.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void aggregate_update(uint64_t *, const uint64_t *, uint64_t, bool);

int main() {
  constexpr unsigned lanes = 4096, updates = 96;
  uint64_t checks = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    std::array<uint64_t, lanes> input, expected, output;
    const uint64_t seed =
        (uint64_t(trial) * 0xd1342543de82ef95ULL) ^ 0x0123456789abcdefULL;
    const bool enabled = (trial & 1) != 0;
    for (unsigned i = 0; i < lanes; ++i)
      input[i] = seed ^ (uint64_t(i + 1) * 0x94d049bb133111ebULL);
    expected = input;
    if (enabled)
      for (unsigned i = 0; i < updates; ++i)
        expected[(i * 37 + 13) % lanes] =
            seed ^ (uint64_t(i + 1) * 0x9e3779b97f4a7c15ULL);
    aggregate_update(output.data(), input.data(), seed, enabled);
    for (unsigned i = 0; i < lanes; ++i, ++checks)
      if (output[i] != expected[i]) {
        std::fprintf(stderr, "separate output mismatch trial=%u lane=%u\n",
                     trial, i);
        return 1;
      }
    aggregate_update(input.data(), input.data(), seed, enabled);
    for (unsigned i = 0; i < lanes; ++i, ++checks)
      if (input[i] != expected[i]) {
        std::fprintf(stderr, "alias output mismatch trial=%u lane=%u\n", trial,
                     i);
        return 1;
      }
  }
  std::printf("PASS: %llu aggregate lane checks\n",
              static_cast<unsigned long long>(checks));
}
