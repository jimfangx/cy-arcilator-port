#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
using namespace chipyard_arc;
static std::array<unsigned, 32> pool;
static unsigned calls;
extern "C" void *get_handle(int seed) {
  return seed % 5 == 0 ? nullptr : &pool[unsigned(seed) % 32];
}
extern "C" int pointer_arrays(void *const *input, void **output, void **work) {
  if (std::string(svGetNameFromScope(svGetScope())) !=
      "PointerHarness/rtl/rtl/rtl")
    throw std::runtime_error("pointer array context mismatch");
  if (work[0] != input[1] || work[1])
    throw std::runtime_error("pointer array input layout mismatch");
  output[0] = input[0];
  output[1] = nullptr;
  work[1] = input[0];
  ++calls;
  return 7;
}
extern "C" void open_pointer_arrays(int seed, svOpenArrayHandle ih,
                                    svOpenArrayHandle oh) {
  if (svDimensions(ih) != 2 || svLeft(ih, 1) != 2 || svRight(ih, 1) != 1 ||
      svLeft(ih, 2) != -2 || svRight(ih, 2) != -1 || svSizeOfArray(ih) != 32 ||
      svSizeOfArray(oh) != 32)
    throw std::runtime_error("open chandle array bounds/layout mismatch");
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      auto input = static_cast<void **>(svGetArrElemPtr2(ih, 2 - i, -2 + j));
      auto output = static_cast<void **>(svGetArrElemPtr2(oh, -1 + i, 4 + j));
      if (!input || !output || *input != get_handle(seed + i * 2 + j) ||
          *output)
        throw std::runtime_error("open chandle input/output pointer mismatch");
      *output = *input;
    }
  ++calls;
}
static char mutable_text[16];
extern "C" void string_arrays(int seed, const char *const *input,
                              const char **output, const char **work) {
  if (std::string(svGetNameFromScope(svGetScope())) !=
      "PointerHarness/rtl/rtl/rtl")
    throw std::runtime_error("string array context mismatch");
  if (std::strcmp(input[0], "alpha") ||
      std::strcmp(input[1], seed & 1 ? "" : "beta") ||
      std::strcmp(input[2], "你好") || std::strcmp(input[3], "z") ||
      std::strcmp(work[0], "left") || std::strcmp(work[1], "right"))
    throw std::runtime_error("string array C input layout mismatch");
  for (int i = 0; i < 4; i++)
    if (!output[i] || *output[i])
      throw std::runtime_error("string output default mismatch");
  std::strcpy(mutable_text, "tail");
  output[0] = input[0];
  output[1] = nullptr;
  output[2] = input[2];
  if (!(seed & 2))
    output[3] = mutable_text;
  const char *previous = work[0];
  work[0] = work[1];
  work[1] = previous;
  ++calls;
}
extern "C" void clobber_strings() { std::strcpy(mutable_text, "xxxx"); }
extern "C" void open_string_arrays(svOpenArrayHandle ih, svOpenArrayHandle oh) {
  if (svDimensions(ih) != 3 || svDimensions(oh) != 3 ||
      svSizeOfArray(ih) != 64 || svSizeOfArray(oh) != 64)
    throw std::runtime_error("open string array rank/layout mismatch");
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++)
      for (int k = 0; k < 2; k++) {
        auto input =
            static_cast<const char **>(svGetArrElemPtr3(ih, -1 + i, 5 - j, k));
        auto output = static_cast<const char **>(
            svGetArrElemPtr(oh, 2 - i, -2 + j, 7 - k));
        if (!input || !output || !*input || !*output || **output ||
            std::strcmp(*input, k ? "你好" : ""))
          throw std::runtime_error("open string array pointer/data mismatch");
        if (k)
          *output = *input;
      }
  ++calls;
}
int main(int argc, char **argv) {
  for (unsigned recreation = 0; recreation < 2; recreation++) {
    auto m = create_model(argc, argv);
    m->initialize();
    calls = 0;
    unsigned expected = 0;
    std::mt19937 rng(173);
    for (unsigned c = 0; c < 256; c++) {
      int seed = int(rng() % 65536) - 32768;
      bool reset = c % 31 == 0, enable = c % 5 != 0;
      m->advance(c * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
        m->signal("seed").set(uint32_t(seed));
      });
      m->advance(c * 2 + 1, [&] { m->signal("clock").set(1); });
      if (!reset && enable)
        expected += 4;
      if (m->signal("observed").get() || calls != expected)
        throw std::runtime_error("pointer array copyback/activation mismatch");
    }
  }
  std::puts("PASS: 512 native pointer/string array cycles; fixed/open "
            "multidimensional arrays, "
            "C pointer ABI, owned UTF8 strings, borrowed mutable buffers, "
            "partial outputs, nulls, "
            "shifted-bound aliases, scope, gating and model recreation");
}
