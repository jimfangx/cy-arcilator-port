#include "native/runtime.h"
#include "svdpi.h"
#include <cstdint>
#include <cstdio>
#include <random>
using namespace chipyard_arc;
static unsigned calls;
static void shape(svOpenArrayHandle h, std::initializer_list<int> bounds,
                  int bits, int bytes) {
  if (svDimensions(h) != int(bounds.size() / 2) || svSize(h, 0) != bits ||
      !svGetArrayPtr(h))
    throw std::runtime_error(
        "native open-array rank/packed-size/data mismatch");
  auto p = bounds.begin();
  int count = 1;
  for (int d = 1; p != bounds.end(); d++) {
    int l = *p++, r = *p++;
    count *= std::abs(l - r) + 1;
    if (svLeft(h, d) != l || svRight(h, d) != r ||
        svLow(h, d) != std::min(l, r) || svHigh(h, d) != std::max(l, r) ||
        svIncrement(h, d) != (l >= r ? 1 : -1) ||
        svSize(h, d) != std::abs(l - r) + 1)
      throw std::runtime_error("native open-array bounds mismatch");
  }
  if (svSizeOfArray(h) != count * bytes || svSize(h, svDimensions(h) + 1) != 0)
    throw std::runtime_error("native open-array C layout size mismatch");
}
extern "C" int open_mix(int seed, svOpenArrayHandle vh, svOpenArrayHandle rh,
                        svOpenArrayHandle wh) {
  shape(vh, {0, 2}, 32, 4);
  shape(rh, {-2, 0}, 8, 1);
  shape(wh, {3, 1}, 16, 2);
  auto values = static_cast<const int *>(svGetArrayPtr(vh));
  auto result = static_cast<signed char *>(svGetArrayPtr(rh));
  auto work = static_cast<short *>(svGetArrayPtr(wh));
  if (svGetArrElemPtr(vh, 1) != values + 1 ||
      svGetArrElemPtr1(rh, -1) != result + 1 ||
      svGetArrElemPtr1(wh, 2) != work + 1 || svGetArrElemPtr1(vh, -1) ||
      svGetArrElemPtr1(vh, 3) || svGetArrElemPtr2(vh, 0, 0) ||
      svGetArrElemPtr1(nullptr, 0))
    throw std::runtime_error("native open-array pointer indexing mismatch");
  if (std::string(svGetNameFromScope(svGetScope())) !=
      "OpenHarness/rtl/rtl/rtl")
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
extern "C" void open_vectors(int seed, svOpenArrayHandle ih,
                             svOpenArrayHandle oh) {
  shape(ih, {1, -1, 4, 5}, 65, 12);
  shape(oh, {2, 0, 0, 1}, 129, 40);
  auto input = static_cast<const svBitVecVal *>(svGetArrayPtr(ih));
  auto output = static_cast<svLogicVecVal *>(svGetArrayPtr(oh));
  if (svLeft(ih, 0) != 64 || svRight(ih, 0) != 0 || svLeft(oh, 0) != 128 ||
      svRight(oh, 0) != 0)
    throw std::runtime_error("native open packed bounds mismatch");
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
    svBitVecVal got[3] = {};
    svGetBitArrElem2VecVal(got, ih, row, col);
    if (got[0] != low || got[1] != high || got[2] != 1 ||
        svGetArrElemPtr(ih, row, col) != input + i * 3)
      throw std::runtime_error("native open vector getter mismatch");
    output[i * 5] = {low ^ 0xd19a3b27U, 0xff00ff00U};
    output[i * 5 + 1] = {high + 71U, 0x00ff00ffU};
    output[i * 5 + 2] = {~low, 0};
    output[i * 5 + 3] = {uint32_t(seed) ^ 0xabcddcbaU, 0};
    output[i * 5 + 4] = {0xffffffffU, 0xfffffffeU};
    svLogicVecVal written[5];
    for (unsigned w = 0; w < 5; w++)
      written[w] = output[i * 5 + w];
    if (i % 2)
      svPutLogicArrElem2VecVal(oh, written, 2 - int(i / 2), int(i % 2));
    else
      svPutLogicArrElemVecVal(oh, written, 2 - int(i / 2), int(i % 2));
  }
  ++calls;
}
extern "C" void open_cube(int seed, svOpenArrayHandle h) {
  shape(h, {-1, 0, 4, 3, 0, 1}, 64, 8);
  for (int x = -1; x <= 0; x++)
    for (int y = 4; y >= 3; y--)
      for (int z = 0; z <= 1; z++) {
        auto p = static_cast<int64_t *>(svGetArrElemPtr3(h, x, y, z));
        if (!p || *p != int64_t(seed) + x * 101 + y * 13 + z ||
            p != svGetArrElemPtr(h, x, y, z))
          throw std::runtime_error("native open 3D data/index mismatch");
        *p += 17;
      }
  ++calls;
}
extern "C" void open_scalars(int seed, svOpenArrayHandle ih,
                             svOpenArrayHandle oh) {
  shape(ih, {-3, -1}, 1, 1);
  shape(oh, {3, 1}, 1, 1);
  for (int i = 0; i < 3; i++)
    if (svGetBitArrElem1(ih, -3 + i) != ((unsigned(seed) >> i) & 1))
      throw std::runtime_error("native open scalar bit getter mismatch");
  svPutLogicArrElem1(oh, svGetBitArrElem(ih, -3), 3);
  svPutLogicArrElem(oh, sv_x, 2);
  svPutLogicArrElem1(oh, sv_z, 1);
  if (svGetLogicArrElem1(oh, 2) != sv_x || svGetLogicArrElem(oh, 1) != sv_z)
    throw std::runtime_error("native open scalar logic encoding mismatch");
  ++calls;
}
extern "C" int open_shape(svOpenArrayHandle h) {
  int sum = 0;
  if (svLeft(h, 1) == 9) {
    shape(h, {9, 8}, 32, 4);
    for (int i = 9; i >= 8; i--)
      sum += *static_cast<int *>(svGetArrElemPtr1(h, i));
  } else {
    shape(h, {0, 2}, 32, 4);
    for (int i = 0; i <= 2; i++)
      sum += *static_cast<int *>(svGetArrElemPtr1(h, i));
  }
  ++calls;
  return sum;
}
extern "C" int open_mixed(svOpenArrayHandle h) {
  int sum = 0;
  shape(h, {7, 6, -1, 1}, 32, 4);
  for (int i = 7; i >= 6; i--)
    for (int j = -1; j <= 1; j++)
      sum += *static_cast<int *>(svGetArrElemPtr2(h, i, j));
  ++calls;
  return sum;
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
        expected += 7;
      if (m->signal("observed").get() || calls != expected)
        throw std::runtime_error(
            "unpacked array return/copyback/activation mismatch");
    }
  }
  std::puts("PASS: 512 native open array import cycles; "
            "input/output/inout, mixed bounds/directions, signed bytes/shorts, "
            "2D65/129-bit vectors, 3D inout, partial outputs and X masking, "
            "mixed fixed/open dimensions, differently shaped C aliases, scalar "
            "logic, "
            "context scope and model recreation");
}
