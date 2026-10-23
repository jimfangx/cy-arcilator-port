#include <cstdint>
#include <cstdio>
extern "C" uint32_t driver(uint8_t selector, uint32_t input);
extern "C" uint32_t local_block(bool condition, uint32_t input);
extern "C" uint64_t driver_pair(uint8_t selector, uint32_t input);
extern "C" uint64_t nested_driver(uint8_t row, uint8_t lane, uint32_t seed,
                                  bool choose);
extern "C" uint32_t *mixed_forward(uint32_t *output, uint32_t seed);
struct ArrayPair {
  uint32_t *borrowed;
  uint32_t *copied;
};
extern "C" ArrayPair borrow_and_copy(uint32_t *output, uint32_t *borrowed,
                                     uint32_t *input);
int main() {
  uint32_t seed = 0xdeadbeef;
  for (unsigned i = 0; i < 10000; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    for (unsigned sel = 0; sel < 4; ++sel) {
      uint32_t expected = seed;
      if (sel == 1)
        expected = ((seed & 255) << 24) | ((seed & 0xff00) << 8) |
                   ((seed >> 8) & 0xff00) | (seed >> 24);
      if (sel == 2)
        expected = (seed << 8) | (seed >> 24);
      uint32_t result = driver(sel, seed);
      uint64_t pair = driver_pair(sel, seed);
      if (pair != ((uint64_t(seed) << 32) | expected)) {
        std::fprintf(
            stderr,
            "multiple array result buffers alias or have incorrect contents\n");
        return 1;
      }
      if (local_block(bool(sel & 1), seed) != seed) {
        std::fprintf(
            stderr,
            "array block argument with scalar function signature failed\n");
        return 1;
      }
      if (result != expected) {
        std::fprintf(stderr,
                     "input %08x selector %u actual %08x expected %08x\n", seed,
                     sel, result, expected);
        return 1;
      }
    }
  }
  for (unsigned i = 0; i < 1000; ++i) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t output[8]{};
    if (mixed_forward(output, seed) != output) {
      std::fprintf(
          stderr,
          "mixed scalar/array result did not return its caller-owned buffer\n");
      return 1;
    }
    for (unsigned lane = 0; lane < 8; ++lane) {
      if (output[lane] != (seed ^ (unsigned(lane == 0) | (lane & 1)))) {
        std::fprintf(stderr,
                     "mixed scalar/array result aliased an input buffer\n");
        return 1;
      }
    }
    uint32_t borrowed[8], input[8], copied[8]{};
    for (unsigned lane = 0; lane < 8; ++lane) {
      borrowed[lane] = seed + lane;
      input[lane] = seed ^ (lane * 0x1001001U);
    }
    auto pair = borrow_and_copy(copied, borrowed, input);
    if (pair.borrowed != borrowed || pair.copied != copied) {
      std::fprintf(stderr,
                   "borrowed reference consumed a return-buffer slot\n");
      return 1;
    }
    for (unsigned lane = 0; lane < 8; ++lane) {
      if (copied[lane] != input[lane] || borrowed[lane] != seed + lane) {
        std::fprintf(stderr,
                     "borrowed reference or caller-owned copy changed\n");
        return 1;
      }
    }
    auto word = [&](unsigned row, unsigned lane) {
      auto rotate = row + 1;
      return ((seed << rotate) | (seed >> (32 - rotate))) ^
             ((row + 1) * 0x1001001U) ^ ((lane + 1) * 0x01020408U);
    };
    for (unsigned choose = 0; choose < 2; ++choose) {
      for (unsigned row = 0; row < 3; ++row) {
        for (unsigned lane = 0; lane < 8; ++lane) {
          unsigned selected_row = choose ? (row + 1) % 3 : row;
          uint64_t expected = (uint64_t(word(choose ? 1 : 2, lane)) << 32) |
                              word(selected_row, lane);
          if (nested_driver(row, lane, seed, choose) != expected) {
            std::fprintf(stderr,
                         "nested value and flat result buffer disagree: row=%u "
                         "lane=%u choose=%u\n",
                         row, lane, choose);
            return 1;
          }
        }
      }
    }
  }
  std::puts("PASS: 120000 CFG array checks, switches, local block arguments, "
            "multiple returns and result buffers");
  std::puts(
      "PASS: 48000 nested 3x8 array checks, mixed nested-value/flat-buffer "
      "arguments and results, branches, lane order and independent buffers");
  std::puts("PASS: 1000 mixed scalar/array return-buffer ownership cases and "
            "1000 borrowed-reference/copy ABI cases");
}
