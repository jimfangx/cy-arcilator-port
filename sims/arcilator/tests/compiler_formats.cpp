#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "compiler_formats_layout.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

extern "C" void RealHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_RealHarness;
extern "C" void __real_chipyard_arc_format_real(double, int32_t, int32_t,
                                                int32_t, int32_t, uint8_t *);
static unsigned calls = 0;
extern "C" void __wrap_chipyard_arc_format_real(double v, int32_t width,
                                                int32_t digits, int32_t left,
                                                int32_t kind, uint8_t *stream) {
  ++calls;
  __real_chipyard_arc_format_real(v, width, digits, left, kind, stream);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  std::ifstream vectors(argv[1]);
  auto *m = arcRuntimeAllocateInstance(&arcRuntimeModel_RealHarness, nullptr);
  arcRuntimeOnInitialized(m);
  auto set = [&](unsigned offset, uint64_t value, unsigned bytes) {
    std::memcpy(m->modelState + offset, &value, bytes);
  };
  auto eval = [&] {
    arcRuntimeOnEval(m);
    RealHarness_eval(m->modelState);
  };
  uint64_t bits;
  unsigned cycle = 0, expected = 0;
  while (vectors >> std::hex >> bits) {
    bool enabled = cycle % 3 != 1;
    set(clockOffset, 0, 1);
    set(enableOffset, enabled, 1);
    set(bitsOffset, bits, 8);
    eval();
    if (calls != expected)
      throw std::runtime_error("real formatter ran before clock edge");
    set(clockOffset, 1, 1);
    if (enabled)
      expected += formatsPerActivation;
    eval();
    if (calls != expected)
      throw std::runtime_error("real formatter activation count changed");
    set(bitsOffset, ~bits, 8);
    set(enableOffset, !enabled, 1);
    eval();
    eval();
    if (calls != expected)
      throw std::runtime_error("real formatter repeated while clock high");
    ++cycle;
  }
  if (!cycle || !vectors.eof())
    return 2;
  arcRuntimeDeleteInstance(m);
  std::fprintf(
      stderr,
      "PASS: %u real-format clock cycles and %u exact formatter callbacks\n",
      cycle, calls);
}
