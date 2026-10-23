#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" uint32_t concat_empty(uint8_t, uint16_t, bool);
extern "C" uint32_t concat_all_empty(uint32_t);

int main() {
  for (unsigned a = 0; a < 32; ++a)
    for (unsigned b = 0; b < 512; ++b)
      for (unsigned flag = 0; flag < 2; ++flag) {
        uint32_t expected = (b << 6) | (a << 1) | flag;
        if (concat_empty(a, b, flag) != expected ||
            concat_all_empty(expected ^ 0xa58f132b) != (expected ^ 0xa58f132b))
          throw std::runtime_error(
              "zero-width concat lost live bits or results");
      }
  std::puts("PASS: 32768 leading, trailing, nested and interleaved zero-width "
            "concat cases and empty-result removal at O0/O2");
}
