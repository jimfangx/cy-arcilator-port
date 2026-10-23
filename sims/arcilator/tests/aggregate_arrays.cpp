#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" uint64_t aggregate_lookup(uint8_t, uint8_t, bool, uint32_t);
extern "C" uint64_t fp_table(uint8_t);

int main() {
  struct Entry {
    unsigned tag, lanes[3], lo;
    uint32_t hi;
  };
  const Entry table[] = {{31, {4096, 777, 1}, 5, 0x80000001},
                         {3, {13, 8190, 4097}, 127, 0xdeadbeef},
                         {17, {8191, 0, 701}, 91, 0x12345678}};
  const unsigned exp[] = {8, 5, 5, 11, 8};
  const unsigned man[] = {7, 2, 10, 52, 23};
  uint32_t seed = 0xa83be741;
  unsigned checks = 0;
  for (unsigned trial = 0; trial < 1000; ++trial) {
    seed = seed * 1664525 + 1013904223;
    for (unsigned i = 0; i < 5; ++i) {
      if (fp_table(i) != ((uint64_t(exp[i]) << 32) | man[i]))
        throw std::runtime_error("FP table field or array order changed");
      ++checks;
    }
    for (unsigned row = 0; row < 3; ++row)
      for (unsigned lane = 0; lane < 3; ++lane)
        for (bool zero : {false, true}) {
          const auto &e = table[row];
          uint64_t expected = seed;
          if (!zero)
            expected = (uint64_t(e.tag) << 52) |
                       (uint64_t(e.lanes[lane]) << 39) |
                       (uint64_t(e.lo) << 32) | (e.hi ^ seed);
          if (aggregate_lookup(row, lane, zero, seed) != expected)
            throw std::runtime_error(
                "nested aggregate constant, CFG or copy semantics changed");
          ++checks;
        }
  }
  std::printf("PASS: %u aggregate constant checks, FP table, nested struct/"
              "array fields, zero/data CFG returns and immutable updates\n",
              checks);
}
