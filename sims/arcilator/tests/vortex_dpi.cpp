#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "native/compat.h"
#include "vortex_dpi_layout.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>

using Word = std::conditional_t<VORTEX_DPI_XLEN == 32, uint32_t, uint64_t>;
using Signed = std::make_signed_t<Word>;
static constexpr unsigned width = sizeof(Word) * 8;
static std::array<unsigned, 3> calls{};
#ifdef VORTEX_DPI_CALL_WHEN_DISABLED
static constexpr bool call_when_disabled = true;
#else
static constexpr bool call_when_disabled = false;
#endif
extern "C" {
void VortexDPIHarness_eval(void *);
extern ArcRuntimeModelInfo arcRuntimeModel_VortexDPIHarness;
void __real_dpi_imul(bool, bool, bool, Signed, Signed, Signed *, Signed *);
void __real_dpi_idiv(bool, bool, Signed, Signed, Signed *, Signed *);
uint64_t __real_dpi_uuid_gen(bool, int, uint64_t);
int dpi_register();
void dpi_assert(int, bool, int);
void __wrap_dpi_imul(bool enable, bool sa, bool sb, Signed a, Signed b,
                     Signed *low, Signed *high) {
  ++calls[0];
  if (*low || *high)
    throw std::runtime_error(
        "Vortex multiply outputs were not seeded on entry");
  __real_dpi_imul(enable, sa, sb, a, b, low, high);
}
void __wrap_dpi_idiv(bool enable, bool sign, Signed a, Signed b,
                     Signed *quotient, Signed *remainder) {
  ++calls[1];
  if (*quotient || *remainder)
    throw std::runtime_error("Vortex divide outputs were not seeded on entry");
  __real_dpi_idiv(enable, sign, a, b, quotient, remainder);
}
uint64_t __wrap_dpi_uuid_gen(bool reset, int wid, uint64_t pc) {
  ++calls[2];
  return __real_dpi_uuid_gen(reset, wid, pc);
}
}
static void check(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  chipyard_arc::DpiScope scope("VortexDPIHarness");
  chipyard_arc::DpiContext context(scope);
  if (argc == 2 && std::string(argv[1]) == "--assert-failure") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int assertion = dpi_register();
    dpi_assert(assertion, false, 2);
    dpi_assert(assertion, false, 2);
    return 99;
  }
  // The original functions intentionally preserve caller storage when disabled.
  // This tests the driver directly. The compiled DPI wrapper supplies its own
  // initialized output storage and must not retain a previous SV actual value.
  Signed low = 17, high = -23, quotient = 31, remainder = -47;
  __real_dpi_imul(false, true, true, -7, 13, &low, &high);
  __real_dpi_idiv(false, true, -7, 13, &quotient, &remainder);
  check(low == 17 && high == -23 && quotient == 31 && remainder == -47,
        "disabled original DPI arithmetic modified caller outputs");
  int assertion = dpi_register();
  for (unsigned i = 0; i < 5; ++i)
    dpi_assert(assertion, true, 2);

  auto *instance =
      arcRuntimeAllocateInstance(&arcRuntimeModel_VortexDPIHarness, nullptr);
  arcRuntimeOnInitialized(instance);
  auto set = [&](unsigned offset, uint64_t value, unsigned bytes) {
    std::memcpy(instance->modelState + offset, &value, bytes);
  };
  auto read = [&](unsigned offset, unsigned bytes) {
    uint64_t value = 0;
    std::memcpy(&value, instance->modelState + offset, bytes);
    return value;
  };
  std::array<Word, 4> expected{};
  uint64_t expected_uuid = 0;
  std::array<unsigned, 3> expected_calls{};
  struct PCState {
    uint32_t id = 0, references = 0;
  };
  struct Warp {
    uint32_t next = 0;
    std::map<uint64_t, PCState> pcs;
  };
  std::map<unsigned, Warp> warps;
  auto eval = [&] {
    arcRuntimeOnEval(instance);
    VortexDPIHarness_eval(instance->modelState);
    std::array<Word, 4> observed{Word(read(lowOffset, sizeof(Word))),
                                 Word(read(highOffset, sizeof(Word))),
                                 Word(read(quotientOffset, sizeof(Word))),
                                 Word(read(remainderOffset, sizeof(Word)))};
    auto uuid = read(uuidOffset, 6) & ((UINT64_C(1) << 44) - 1);
    if (observed != expected || uuid != expected_uuid ||
        calls != expected_calls) {
      std::fprintf(stderr,
                   "Vortex DPI mismatch at %u multiply calls (XLEN=%u)\n",
                   calls[0], width);
      throw std::runtime_error(
          "original Vortex DPI values or scheduling mismatch");
    }
  };
  uint64_t random = UINT64_C(0x6a09e667f3bcc909);
  auto next = [&] {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return random;
  };
  unsigned sign_cases = 0, enabled = 0;
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    bool enable = cycle % 3 != 1, sa = cycle & 1, sb = cycle & 2;
    bool reset = cycle == 0 || cycle == 127 || cycle == 511;
    Word a = next(), b = next();
    if (cycle % 17 == 0)
      b = 0;
    if (cycle % 19 == 0) {
      a = Word(1) << (width - 1);
      b = Word(-1);
      sa = true;
    }
    auto wid = (cycle / 11) % 4;
    auto pc = UINT64_C(0x80000000) + ((cycle / 3) % 7) * 4;
    set(clockOffset, 0, 1);
    set(enableOffset, enable, 1);
    set(resetOffset, reset, 1);
    // A valid i1 load ignores padding. The foreign C ABI must normalize the
    // whole byte before the original C++ bool arguments consume it.
    set(signed_aOffset, sa ? 255 : 254, 1);
    set(signed_bOffset, sb ? 255 : 254, 1);
    set(aOffset, a, sizeof(Word));
    set(bOffset, b, sizeof(Word));
    set(widOffset, wid, 4);
    set(pcOffset, pc, 8);
    eval();
    if (enable || call_when_disabled) {
      ++expected_calls[0];
      ++expected_calls[1];
    }
    if (!enable && call_when_disabled)
      expected.fill(0);
    if (enable) {
      ++enabled;
      sign_cases |= 1u << (unsigned(sa) | (unsigned(sb) << 1));
      auto product = static_cast<unsigned __int128>(a) * b;
      expected[0] = Word(product);
      expected[1] = Word(product >> width);
      if (sa && (a >> (width - 1)))
        expected[1] -= b;
      if (sb && (b >> (width - 1)))
        expected[1] -= a;
      if (!b) {
        expected[2] = Word(-1);
        expected[3] = a;
      } else if (sa) {
        __int128 dividend = Signed(a), divisor = Signed(b);
        expected[2] = Word(dividend / divisor);
        expected[3] = Word(dividend % divisor);
      } else {
        expected[2] = a / b;
        expected[3] = a % b;
      }
    }
    if (reset || enable) {
      ++expected_calls[2];
      if (reset) {
        expected_uuid = 0;
        warps.clear();
      } else {
        auto &warp = warps[wid];
        auto found = warp.pcs.find(pc);
        if (found == warp.pcs.end())
          found = warp.pcs.emplace(pc, PCState{warp.next++, 0}).first;
        auto &entry = found->second;
        expected_uuid = ((uint64_t(entry.references++) << 32) |
                         (uint64_t(wid) << 16) | entry.id) &
                        ((UINT64_C(1) << 44) - 1);
      }
    }
    set(clockOffset, 1, 1);
    eval();
    set(aOffset, ~uint64_t(a), sizeof(Word));
    set(pcOffset, pc + 4, 8);
    eval();
    eval();
  }
  check(sign_cases == 15 && enabled == 683,
        "original DPI test did not cover all sign modes and enabled edges");
  arcRuntimeDeleteInstance(instance);
  std::printf("PASS: original Vortex DPI XLEN=%u, 1024 cycles, signed/unsigned "
              "multiply/divide, zero divisor, overflow, per-call output seeds, "
              "UUID state/reset/44-bit narrowing and exact callback counts\n",
              width);
  if (call_when_disabled)
    std::puts("PASS: disabled original Vortex callbacks leave zero-seeded DPI "
              "outputs unwritten");
}
