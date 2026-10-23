#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "compiler_dpi_layout.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" void OpaqueCallbackHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_OpaqueCallbackHarness;
static uint64_t calls = 0;
static uint64_t mixed_calls = 0;
extern "C" uint64_t chipyard_test_opaque_callback(uint64_t data) {
  return (data * 17) ^ (++calls * UINT64_C(0x9e3779b97f4a7c15));
}

extern "C" bool chipyard_test_mixed_callback(bool mode, bool signed_a,
                                             bool signed_b, uint32_t a,
                                             uint32_t b, uint32_t *accumulator,
                                             uint32_t *low, uint32_t *high,
                                             bool *flag) {
  ++mixed_calls;
  __int128 left = signed_a ? __int128(int32_t(a)) : __int128(a);
  __int128 right = signed_b ? __int128(int32_t(b)) : __int128(b);
  uint64_t product = mode ? uint64_t(left * right) : 0;
  *low = uint32_t(product);
  *high = uint32_t(product >> 32);
  *accumulator += *low ^ *high;
  *flag = mode ^ signed_a ^ signed_b;
  return (*accumulator >> 31) != 0;
}

int main() {
  auto *instance = arcRuntimeAllocateInstance(
      &arcRuntimeModel_OpaqueCallbackHarness, nullptr);
  arcRuntimeOnInitialized(instance);
  auto set = [&](unsigned offset, unsigned bytes, uint64_t value) {
    std::memcpy(instance->modelState + offset, &value, bytes);
  };
  uint64_t expected = 0, expected_calls = 0;
  uint8_t expected_comb = 0;
  bool expected_valid = false;
  uint32_t expected_accumulator = 0, expected_low = 0, expected_high = 0;
  bool expected_flag = false, expected_boolean_result = false;
  unsigned modes_seen = 0, signed_cases_seen = 0, aliases_seen = 0;
  auto read32 = [&](unsigned offset) {
    uint32_t value;
    std::memcpy(&value, instance->modelState + offset, sizeof(value));
    return value;
  };
  auto eval = [&] {
    arcRuntimeOnEval(instance);
    OpaqueCallbackHarness_eval(instance->modelState);
    uint64_t value = 0;
    std::memcpy(&value, instance->modelState + valueOffset, 6);
    value &= (1ULL << 44) - 1;
    if (value != expected || calls != expected_calls ||
        mixed_calls != expected_calls ||
        bool(instance->modelState[validOffset]) != expected_valid ||
        instance->modelState[comb_valueOffset] != expected_comb ||
        read32(accumulatorOffset) != expected_accumulator ||
        read32(lowOffset) != expected_low ||
        read32(highOffset) != expected_high ||
        bool(instance->modelState[flagOffset]) != expected_flag ||
        bool(instance->modelState[boolean_resultOffset]) !=
            expected_boolean_result) {
      std::fprintf(stderr,
                   "DPI mismatch: value %llx/%llx calls %llu/%llu/%llu "
                   "acc %x/%x low %x/%x high %x/%x flag %u/%u return %u/%u\n",
                   (unsigned long long)value, (unsigned long long)expected,
                   (unsigned long long)calls, (unsigned long long)mixed_calls,
                   (unsigned long long)expected_calls,
                   read32(accumulatorOffset), expected_accumulator,
                   read32(lowOffset), expected_low, read32(highOffset),
                   expected_high, unsigned(instance->modelState[flagOffset]),
                   unsigned(expected_flag),
                   unsigned(instance->modelState[boolean_resultOffset]),
                   unsigned(expected_boolean_result));
      throw std::runtime_error("opaque DPI return value or callback count "
                               "changed across suspension");
    }
  };
  uint64_t random = UINT64_C(0xa5a53c3c8e8e7272);
  for (uint64_t cycle = 0; cycle < 100; ++cycle) {
    bool enable = cycle % 3 != 1;
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    uint64_t data = (random & ~UINT64_C(7)) | (cycle & 7);
    if (cycle < 24)
      data = UINT64_C(0x8000000180000000) | (cycle & 7);
    set(clockOffset, 1, 0);
    set(enableOffset, 1, enable);
    set(dataOffset, 8, data);
    expected_comb = enable ? uint8_t(data) : 0;
    expected = 0;
    expected_valid = false;
    expected_accumulator = expected_low = expected_high = 0;
    expected_flag = expected_boolean_result = false;
    eval();
    if (enable) {
      aliases_seen |= 1u << ((data >> 3) & 1);
      expected_valid = true;
      ++expected_calls;
      expected =
          ((data * 17) ^ (expected_calls * UINT64_C(0x9e3779b97f4a7c15))) &
          ((1ULL << 44) - 1);
      bool mode = (data >> 2) & 1, signed_a = data & 1,
           signed_b = (data >> 1) & 1;
      modes_seen |= 1u << (data & 7);
      uint32_t a = uint32_t(data), b = uint32_t(data >> 32);
      uint64_t product = uint64_t(a) * b;
      if (mode) {
        signed_cases_seen |= 1u << (unsigned(signed_a && (a >> 31)) |
                                    (unsigned(signed_b && (b >> 31)) << 1));
        expected_low = uint32_t(product);
        expected_high = uint32_t(product >> 32);
        if (signed_a && (a >> 31))
          expected_high -= b;
        if (signed_b && (b >> 31))
          expected_high -= a;
      }
      expected_accumulator = a + (expected_low ^ expected_high);
      expected_flag = mode ^ signed_a ^ signed_b;
      expected_boolean_result = (expected_accumulator >> 31) != 0;
    }
    set(clockOffset, 1, 1);
    eval();
    // A changed unobserved input and repeated evaluations must not call again.
    set(dataOffset, 8, ~data);
    expected_comb = enable ? uint8_t(~data) : 0;
    eval();
    eval();
  }
  if (modes_seen != 255 || signed_cases_seen != 15 || aliases_seen != 3)
    throw std::runtime_error("DPI Boolean/sign combinations were not covered");
  arcRuntimeDeleteInstance(instance);
  std::puts("PASS: scalar DPI return values across suspension, edge/enable "
            "gating, exact callback counts and sibling combinational drives "
            "(100 cycles); Boolean inputs/return, mixed output pointers and "
            "inout copy-in/copy-out; both aliases of shared C functions");
}
