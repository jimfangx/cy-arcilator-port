#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "compiler_tasks_layout.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" void TaskHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_TaskHarness;
static std::vector<uint32_t> calls;
extern "C" uint32_t chipyard_task_record(uint32_t value) {
  calls.push_back(value);
  return value ^ (uint32_t(calls.size()) * 0x9e3779b9u);
}
int main() {
  auto *instance =
      arcRuntimeAllocateInstance(&arcRuntimeModel_TaskHarness, nullptr);
  arcRuntimeOnInitialized(instance);
  auto set = [&](unsigned offset, uint32_t value, unsigned bytes) {
    std::memcpy(instance->modelState + offset, &value, bytes);
  };
  uint32_t expected = 0;
  std::vector<uint32_t> expectedCalls;
  auto reference = [&](uint32_t data, bool select) {
    uint32_t argument = select ? data + 7u : data ^ 0x5a5a5a5au;
    expectedCalls.push_back(argument);
    return argument ^ (uint32_t(expectedCalls.size()) * 0x9e3779b9u);
  };
  auto eval = [&] {
    arcRuntimeOnEval(instance);
    TaskHarness_eval(instance->modelState);
    uint32_t value;
    std::memcpy(&value, instance->modelState + valueOffset, 4);
    if (value != expected || calls != expectedCalls)
      throw std::runtime_error(
          "task values, callback order, count or activation changed");
  };
  for (unsigned c = 0; c < 512; ++c) {
    // Identical arguments on adjacent activations must still call twice.
    uint32_t data = 0x12345678u ^ ((c / 2) * 0x01234567u);
    bool reset = c % 17 == 0, enable = c % 3 != 1;
    set(clockOffset, 0, 1);
    set(resetOffset, reset, 1);
    set(enableOffset, enable, 1);
    set(dataOffset, data, 4);
    eval();
    if (reset)
      expected = 0;
    else if (enable) {
      uint32_t first = reference(data, data & 1);
      expected = first ^ reference(first, data & 1);
    }
    set(clockOffset, 1, 1);
    eval();
    set(dataOffset, ~data, 4);
    eval();
    eval();
  }
  arcRuntimeDeleteInstance(instance);
  std::puts("PASS: 512 nested task cycles, multiple returns, sequential result "
            "dependencies, reset/enables, repeated arguments and exact "
            "callback order/count");
}
