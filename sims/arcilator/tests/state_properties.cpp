#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "state_properties_layout.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" void EnabledStateHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_EnabledStateHarness;

int main() {
  auto *instance =
      arcRuntimeAllocateInstance(&arcRuntimeModel_EnabledStateHarness, nullptr);
  arcRuntimeOnInitialized(instance);
  auto set = [&](unsigned offset, unsigned bytes, uint64_t value) {
    std::memcpy(instance->modelState + offset, &value, bytes);
  };
  auto get = [&](unsigned offset) {
    uint64_t value = 0;
    std::memcpy(&value, instance->modelState + offset, 5);
    return value & ((1ULL << 34) - 1);
  };
  auto eval = [&] {
    arcRuntimeOnEval(instance);
    EnabledStateHarness_eval(instance->modelState);
  };
  uint64_t array = 0, structure = 0;
  for (unsigned cycle = 0; cycle < 300; ++cycle) {
    bool reset = cycle % 29 == 0, enable = cycle % 3 == 0;
    uint64_t data = (cycle * 997 + 0x1a35) & 0x1ffff;
    set(clockOffset, 1, 0);
    set(resetOffset, 1, reset);
    set(enableOffset, 1, enable);
    set(dataOffset, 3, data);
    eval();
    if (get(array_stateOffset) != array || get(struct_stateOffset) != structure)
      throw std::runtime_error(
          "enabled aggregate state changed before its edge");
    if (reset && enable) {
      array = (5ULL << 17) | 3;
      structure = (13ULL << 17) | 11;
    } else if (enable) {
      array = ((data ^ 0x12345) << 17) | data;
      structure = (data << 17) | (data ^ 0x12345);
    }
    set(clockOffset, 1, 1);
    eval();
    if (get(array_stateOffset) != array || get(struct_stateOffset) != structure)
      throw std::runtime_error(
          "enabled aggregate state lost its value or reset");
  }
  arcRuntimeDeleteInstance(instance);
  std::puts("PASS: enabled array and structure state, hold, clock sampling, "
            "and synchronous reset (300 cycles)");
}
