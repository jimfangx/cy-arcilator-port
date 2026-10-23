#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t random = 1979;
  unsigned checks = 0;
  for (unsigned vector = 0; vector < 1536; ++vector) {
    random = (random * 1664525 + 1013904223) & 131071;
    auto data = vector < 512 ? vector : random;
    model->advance(vector, [&] { model->signal("data").set(data); });
    for (unsigned width : {5, 9, 17}) {
      for (unsigned operation = 0; operation < 3; ++operation) {
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
          uint64_t expected = 0;
          bool accumulator = operation == 1;
          for (unsigned i = 0; i < width; ++i) {
            unsigned bit = reverse ? width - i - 1 : i;
            bool value = (data >> bit) & 1;
            switch (operation) {
            case 0:
              accumulator ^= value;
              break;
            case 1:
              accumulator &= value;
              break;
            case 2:
              accumulator |= value;
              break;
            }
            expected |= uint64_t(accumulator) << bit;
          }
          auto name = "scan_" + std::to_string(width) + "_" +
                      std::to_string(operation) + "_" + std::to_string(reverse);
          if (model->signal(name).get() != expected) {
            std::fprintf(stderr, "scan mismatch: %s, input %llu\n",
                         name.c_str(), static_cast<unsigned long long>(data));
            throw std::runtime_error("native vendor prefix scan mismatch");
          }
          ++checks;
        }
      }
    }
  }
  std::printf("PASS: %u vendor scan checks, 18 specializations, widths 5/9/17, "
              "XOR/AND/OR and both directions\n",
              checks);
}
