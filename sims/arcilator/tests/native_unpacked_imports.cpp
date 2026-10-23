#include "native/runtime.h"
#include "svdpi.h"
#include <cstdint>
#include <cstdio>
#include <random>
using namespace chipyard_arc;
static unsigned calls;
extern "C" int unpacked_mix(int seed, const int *values, signed char *result,
                            short *work) {
  if (std::string(svGetNameFromScope(svGetScope())) !=
      "UnpackedHarness/rtl/rtl/rtl")
    throw std::runtime_error("array context scope mismatch");
  if (values[0] != (seed ^ 7) || values[1] != seed - 11 ||
      values[2] != seed * 3 || work[0] != int16_t(seed + 3) ||
      work[1] != int16_t(seed - 5) || work[2] != int16_t(seed ^ 0x8181))
    throw std::runtime_error("unpacked array input layout mismatch");
  result[0] = int8_t(-seed);
  if (!(seed & 2)) {
    result[1] = int8_t(seed + 19);
    result[2] = int8_t(seed ^ 0xa5);
  }
  work[0] = int16_t(work[0] + 17);
  work[1] = int16_t(work[1] - 23);
  work[2] = int16_t(work[2] + 31);
  ++calls;
  return values[0] + values[1] + values[2];
}
extern "C" void unpacked_vectors(int seed, const svBitVecVal *input,
                                 svLogicVecVal *output) {
  for (unsigned i = 0; i < 6; i++) {
    int row = 1 - int(i / 2), col = 4 + int(i % 2);
    uint32_t low = uint32_t(seed) ^ uint32_t(row * 31 + col * 3);
    uint32_t high = uint32_t(seed + row * 17 + col);
    if (input[i * 3] != low || input[i * 3 + 1] != high ||
        input[i * 3 + 2] != 1)
      throw std::runtime_error(
          "multidimensional packed input padding/layout mismatch");
    if (i == 5 && (seed & 4))
      continue;
    output[i * 5] = {low ^ 0xd19a3b27U, 0xff00ff00U};
    output[i * 5 + 1] = {high + 71U, 0x00ff00ffU};
    output[i * 5 + 2] = {~low, 0};
    output[i * 5 + 3] = {uint32_t(seed) ^ 0xabcddcbaU, 0};
    output[i * 5 + 4] = {0xffffffffU, 0xfffffffeU};
  }
  ++calls;
}
int main(int argc, char **argv) {
  for (unsigned recreation = 0; recreation < 2; recreation++) {
    auto m = create_model(argc, argv);
    m->initialize();
    calls = 0;
    unsigned expected = 0;
    std::mt19937 rng(19);
    for (unsigned c = 0; c < 256; c++) {
      bool reset = c % 31 == 0, enable = c % 5 != 0;
      int seed = int(rng() % 32768) - 16384;
      m->advance(c * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
        m->signal("seed").set(uint32_t(seed));
      });
      m->advance(c * 2 + 1, [&] { m->signal("clock").set(1); });
      if (!reset && enable)
        expected += 2;
      if (m->signal("observed").get() || calls != expected)
        throw std::runtime_error(
            "unpacked array return/copyback/activation mismatch");
    }
  }
  std::puts(
      "PASS: 512 native unpacked array import cycles; "
      "input/output/inout, mixed bounds/directions, signed bytes/shorts, "
      "2D65/129-bit vectors, partial output zeroing, aliases and X masking, "
      "context scope and model recreation");
}
