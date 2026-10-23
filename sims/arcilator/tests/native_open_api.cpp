#include "native/compat.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
extern "C" void *chipyard_arc_open_array_create(void *, uint64_t, uint64_t,
                                                uint64_t, const char *, int64_t,
                                                int64_t);
extern "C" void chipyard_arc_open_array_delete(void *);
int main() {
  // Expected values use the canonical aval/bval representation independently
  // of the accessor implementations, including selects across a word boundary.
  svBitVecVal bit[3] = {0x89abcdefU, 0x76543210U, 1};
  svLogicVecVal logic[3] = {
      {0x89abcdefU, 0x12345678U}, {0x76543210U, 0xfedcba98U}, {1, 0}};
  for (int start = 0; start < 34; start++) {
    svBitVecVal out;
    svLogicVecVal lo;
    svGetPartselBit(&out, bit, start, 32);
    svGetPartselLogic(&lo, logic, start, 32);
    uint64_t av = uint64_t(bit[1]) << 32 | bit[0];
    uint64_t bv = uint64_t(logic[1].bval) << 32 | logic[0].bval;
    uint32_t expected =
        start < 32
            ? uint32_t(av >> start)
            : uint32_t((uint64_t(bit[2]) << 32 | bit[1]) >> (start - 32));
    uint32_t unknown = start < 32
                           ? uint32_t(bv >> start)
                           : uint32_t(uint64_t(logic[1].bval) >> (start - 32));
    assert(out == expected && lo.aval == expected && lo.bval == unknown);
  }
  svBitVecVal put[3] = {~0U, ~0U, ~0U};
  svLogicVecVal lp[3] = {{~0U, ~0U}, {~0U, ~0U}, {~0U, ~0U}};
  svPutPartselBit(put, 0x12345678, 25, 32);
  svPutPartselLogic(lp, {0x12345678, 0x89abcdef}, 25, 32);
  for (int i = 0; i < 96; i++) {
    unsigned v = i >= 25 && i < 57 ? (0x12345678U >> (i - 25)) & 1 : 1;
    unsigned x = i >= 25 && i < 57 ? (0x89abcdefU >> (i - 25)) & 1 : 1;
    assert(((put[i / 32] >> (i % 32)) & 1) == v);
    assert(svGetBitselBit(put, i) == v);
    assert(svGetBitselLogic(lp, i) == (v | (x << 1)));
  }
  for (int code = 0; code < 4; code++) {
    svPutBitselLogic(lp, 31, code);
    assert(((lp[0].aval >> 31) & 1) == (unsigned(code) & 1));
    assert(((lp[0].bval >> 31) & 1) == ((unsigned(code) >> 1) & 1));
  }
  std::array<uint32_t, 16> data{};
  auto h = chipyard_arc_open_array_create(data.data(), 4, 32, 0,
                                          "1:0,-3:-2,4:3,0:1", 31, 0);
  assert(svDimensions(h) == 4 && svSizeOfArray(h) == 64);
  for (int a = 1; a >= 0; a--)
    for (int b = -3; b <= -2; b++)
      for (int c = 4; c >= 3; c--)
        for (int d = 0; d <= 1; d++) {
          int offset = (((1 - a) * 2 + b + 3) * 2 + 4 - c) * 2 + d;
          assert(svGetArrElemPtr(h, a, b, c, d) == &data[offset]);
          uint32_t value = 0xabcdef00U + unsigned(offset);
          svPutBitArrElemVecVal(h, &value, a, b, c, d);
          assert(data[offset] == value);
          uint32_t got = 0;
          svGetBitArrElemVecVal(&got, h, a, b, c, d);
          assert(got == value);
        }
  assert(!svGetArrElemPtr(h, 2, -3, 4, 0));
  chipyard_arc_open_array_delete(h);
  // Legacy descriptors remain compatible with the original SimDRAM byte API.
  uint8_t bytes[4] = {};
  chipyard_arc::OpenArray legacy{bytes, 4, 1};
  svPutBitArrElem(&legacy, 0xda, 2);
  assert(bytes[2] == 0xda && svGetArrElemPtr1(&legacy, 2) == bytes + 2);
  uint32_t byte = 0xa9;
  svPutBitArrElemVecVal(&legacy, &byte, 1);
  assert(bytes[1] == 0xa9);
  assert(!svGetArrElemPtr1(nullptr, 0));
  std::puts("PASS: native DPI select helpers, generic 4D indexing/copying, "
            "bounds and original SimDRAM byte descriptor");
}
