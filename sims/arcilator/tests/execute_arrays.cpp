#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

extern "C" uint32_t execute_test(uint32_t *, uint32_t, uint8_t);
static std::vector<std::array<uint32_t, 2>> calls;
extern "C" uint32_t execute_callback(uint32_t value, uint32_t seed) {
  calls.push_back({value, seed});
  return value ^ seed ^ (uint32_t(calls.size()) * 0x937ac41dU);
}
int main() {
  uint32_t rng = 0x87e65b2a;
  for (unsigned trial = 0; trial < 1000; ++trial) {
    rng = rng * 1664525 + 1013904223;
    for (uint8_t flags = 0; flags < 4; ++flags) {
      std::array<uint32_t, 4> expected, actual{};
      for (unsigned i = 0; i < 4; ++i)
        expected[i] = rng + i * 17;
      std::vector<std::array<uint32_t, 2>> wanted;
      uint32_t total = rng;
      unsigned iterations = flags & 1 ? 2 : 4;
      for (unsigned i = 0; i < iterations; ++i) {
        wanted.push_back({expected[i], rng});
        expected[i] = expected[i] ^ rng ^ ((i + 1) * 0x937ac41dU);
        total += expected[i];
        if (rng & 1)
          total ^= 0xa5a5a5a5;
      }
      calls.clear();
      auto result = execute_test(actual.data(), rng, flags);
      if (actual != expected || result != (total ^ rng) || calls != wanted)
        throw std::runtime_error(
            "nested execute CFG array/result/callback mismatch");
    }
  }
  std::puts(
      "PASS: 4000 nested execute regions, CFG loop/early exits, array and "
      "scalar results, borrowed inputs, callback order and exact counts");
}
