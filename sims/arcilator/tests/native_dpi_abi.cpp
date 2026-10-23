#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <cstring>

struct Inputs {
  uint32_t data = 0, fp32 = 0;
  uint64_t fp64 = 0;
  std::array<uint32_t, 5> wide{};
  bool enable = false;
} input;
static std::array<unsigned, 4> calls{};
static void require(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}
extern "C" int chipyard_abi_atom(int8_t a, uint8_t b, int16_t c, uint16_t d,
                                 int32_t e, uint32_t f, int64_t g, uint64_t h,
                                 svBit i, svLogic j, const svBitVecVal *m,
                                 const svLogicVecVal *n, svBit *k, svLogic *l) {
  ++calls[0];
  require(a == int8_t(input.data) && b == uint8_t(input.data >> 8) &&
              c == int16_t(input.data) && d == uint16_t(input.data >> 16) &&
              uint32_t(e) == input.data && f == ~input.data &&
              g == int64_t(int32_t(input.data)) &&
              h == ((uint64_t(input.data) << 32) | uint32_t(~input.data)) &&
              i == (input.data & 1) && j == ((input.data >> 1) & 1),
          "native scalar DPI C values or signed/zero extension changed");
  require(*m == ((input.data >> 2) & 1) && n->aval == ((input.data >> 3) & 1) &&
              n->bval == 0,
          "one-bit packed vectors were confused with scalar bit/logic ABI");
  require(*k == 0 && *l == 0, "native scalar DPI outputs were not initialized");
  *k = input.data & 1;
  *l = (input.data >> 2) & 3;
  return int32_t(input.data ^ 0x12345678u);
}
static std::array<uint32_t, 3> logicInput() {
  unsigned __int128 value = (static_cast<unsigned __int128>(input.data) << 33) |
                            (uint64_t(uint32_t(~input.data)) << 1) | 1;
  return {uint32_t(value), uint32_t(value >> 32), uint32_t(value >> 64)};
}
extern "C" void chipyard_abi_vectors(const svBitVecVal *a,
                                     const svLogicVecVal *b,
                                     const svBitVecVal *c,
                                     const svLogicVecVal *d, svLogicVecVal *e,
                                     svBitVecVal *f, svLogicVecVal *g) {
  ++calls[1];
  require(*a == input.data && b->aval == ~input.data && b->bval == 0,
          "same-width scalar, bit vector or logic vector ABI confused");
  for (unsigned n = 0; n < 5; ++n)
    require(c[n] == input.wide[n],
            "129-bit input word order or padding changed");
  auto words = logicInput();
  for (unsigned n = 0; n < 3; ++n)
    require(d[n].aval == words[n] && d[n].bval == 0,
            "65-bit aval/bval layout or input state domain changed");
  uint64_t initial = (uint64_t(input.data) << 1) | 1;
  require(e[0].aval == uint32_t(initial) &&
              e[1].aval == uint32_t(initial >> 32) && e[0].bval == 0 &&
              e[1].bval == 0,
          "logic-vector inout copy-in changed");
  for (unsigned n = 0; n < 5; ++n)
    require(f[n] == 0, "packed bit-vector output was not initialized");
  for (unsigned n = 0; n < 3; ++n)
    require(g[n].aval == 0 && g[n].bval == 0,
            "packed logic-vector output was not initialized");
  f[2] = input.data ^ 0xa55aa55au;
  f[4] = 1;
  e[0].aval ^= 0x2468ace0u;
  e[0].bval = input.data & 0x11111111u;
  e[1].aval ^= 1;
  e[1].bval = (input.data >> 5) & 1;
  for (unsigned n = 0; n < 3; ++n) {
    g[n].aval = words[n] ^ (0x9e3779b9u * (n + 1));
    g[n].bval = input.data ^ (0x13579bdfu * (n + 1));
  }
}
extern "C" double chipyard_abi_real(double a, float b, float *c) {
  ++calls[2];
  uint64_t bits64;
  uint32_t bits32, original;
  std::memcpy(&bits64, &a, 8);
  std::memcpy(&bits32, &b, 4);
  std::memcpy(&original, c, 4);
  require(
      bits64 == input.fp64 && bits32 == input.fp32 && original == 0,
      "real/shortreal C ABI, IEEE payload or output initialization changed");
  bits64 ^= UINT64_C(0x8000000000000000);
  bits32 ^= 0x80000000u;
  std::memcpy(&a, &bits64, 8);
  std::memcpy(c, &bits32, 4);
  return a;
}
extern "C" void chipyard_abi_clocked(svBit enable, int32_t data,
                                     svLogicVecVal *result) {
  ++calls[3];
  require(enable == unsigned(input.enable) && uint32_t(data) == input.data,
          "clocked DPI sampled incorrect inputs");
  require(result[0].aval == 0 && result[0].bval == 0 && result[1].aval == 0 &&
              result[1].bval == 0,
          "clocked DPI output was not initialized");
  if (enable) {
    result[0].aval = input.data ^ 0x87654321u;
    result[0].bval = input.data & 0x10101010u;
    result[1].aval = (input.data >> 31) & 1;
    result[1].bval = (input.data >> 30) & 1;
  }
}

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t time = 0, clocked = 0, clockedFP64 = 0;
  uint32_t clockedFP32 = 0;
  std::array<unsigned, 4> expectedCalls{1, 1, 1, 0};
  auto readWords = [&](const char *name, unsigned count) {
    std::array<uint32_t, 5> result{};
    auto bytes = model->signal(name).copy();
    require(bytes.size() <= count * 4, "ABI fixture port width changed");
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
  };
  auto check = [&] {
    auto bits = readWords("vector_bits", 5);
    std::array<uint32_t, 5> expectedBits{0, 0, input.data ^ 0xa55aa55au, 0, 1};
    auto logic = readWords("vector_logic", 3);
    auto original = logicInput();
    std::array<uint32_t, 5> expectedLogic{};
    for (unsigned n = 0; n < 3; ++n)
      expectedLogic[n] = (original[n] ^ (0x9e3779b9u * (n + 1))) &
                         ~(input.data ^ (0x13579bdfu * (n + 1)));
    expectedLogic[2] &= 1;
    uint64_t acc = ((uint64_t(input.data) << 1) | 1) ^ UINT64_C(0x12468ace0);
    acc &=
        ~((uint64_t((input.data >> 5) & 1) << 32) | (input.data & 0x11111111u));
    unsigned scalarLogic = (input.data >> 2) & 3;
    if (calls != expectedCalls || bits != expectedBits ||
        logic != expectedLogic ||
        model->signal("atom").get() != (input.data ^ 0x12345678u) ||
        model->signal("atom_bit").get() != (input.data & 1) ||
        model->signal("atom_logic").get() !=
            (scalarLogic < 2 ? scalarLogic : 0) ||
        model->signal("accumulator").get() != acc ||
        model->signal("clocked").get() != clocked ||
        model->signal("clocked_real_bits").get() != clockedFP64 ||
        model->signal("clocked_shortreal_bits").get() != clockedFP32 ||
        model->signal("real_bits").get() !=
            (input.fp64 ^ UINT64_C(0x8000000000000000)) ||
        model->signal("shortreal_bits").get() != (input.fp32 ^ 0x80000000u)) {
      std::fprintf(stderr,
                   "ABI mismatch at time %llu data=%08x callbacks=%u,%u,%u,%u "
                   "expected=%u,%u,%u,%u\n",
                   (unsigned long long)model->time(), input.data, calls[0],
                   calls[1], calls[2], calls[3], expectedCalls[0],
                   expectedCalls[1], expectedCalls[2], expectedCalls[3]);
      throw std::runtime_error("native imported DPI ABI values, activation or "
                               "clocked sampling mismatch");
    }
  };
  model->advance(time++);
  check();
  const uint64_t doubles[] = {0,
                              UINT64_C(0x8000000000000000),
                              UINT64_C(0x7ff8000000000123),
                              UINT64_C(0xfff8000000000456),
                              UINT64_C(0x7ff0000000000000),
                              1,
                              UINT64_C(0x3ff0000000000000)};
  const uint32_t floats[] = {0,           0x80000000u, 0x7fc00123u, 0xffc00456u,
                             0x7f800000u, 1,           0x3f800000u};
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    auto previous = input;
    input.data = 0xa5a50001u ^ ((cycle / 3) * 0x13579bdu);
    input.enable = cycle % 3 != 1;
    input.fp32 = floats[(cycle / 4) % 7];
    input.fp64 = doubles[(cycle / 5) % 7];
    for (unsigned n = 0; n < 4; ++n)
      input.wide[n] =
          ((cycle / 7) * 0x01234567u) ^ (0x89abcdefu + n * 0x11111111u);
    input.wide[4] = (cycle / 11) & 1;
    expectedCalls[0] += input.data != previous.data;
    expectedCalls[1] +=
        input.data != previous.data || input.wide != previous.wide;
    expectedCalls[2] +=
        input.fp32 != previous.fp32 || input.fp64 != previous.fp64;
    model->advance(time++, [&] {
      model->signal("clock").set(0);
      model->signal("enable").set(input.enable);
      model->signal("data").set(input.data);
      model->signal("fp32").set(input.fp32);
      model->signal("fp64").set(input.fp64);
      auto *wide = model->signal("wide").data;
      std::memcpy(wide, input.wide.data(), 17);
      wide[16] |= 0xfe;
    });
    check();
    model->advance(time++, [&] { model->signal("wide").data[16] ^= 0xfe; });
    check();
    ++expectedCalls[3];
    ++expectedCalls[2];
    clockedFP64 = input.fp64 ^ UINT64_C(0x8000000000000000);
    clockedFP32 = input.fp32 ^ 0x80000000u;
    clocked = input.enable ? (((uint64_t((input.data >> 31) & 1) << 32) |
                               (input.data ^ 0x87654321u)) &
                              ~((uint64_t((input.data >> 30) & 1) << 32) |
                                (input.data & 0x10101010u)))
                           : 0;
    model->advance(time++, [&] { model->signal("clock").set(1); });
    check();
    model->advance(time++);
    check();
  }
  std::puts("PASS: 512 imported native DPI ABI cycles, C-name aliases, "
            "signed/unsigned 8/16/32/64-bit atoms, bit/logic scalars, "
            "32/33/65/129-bit vectors, aval/bval and X/Z-to-zero, partial "
            "outputs, inouts, clocked calls, real/shortreal IEEE payloads and "
            "stable-input guards");
}
