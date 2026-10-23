#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2)
    return 2;
  bool value_fault = false, assertion_fault = false;
  for (int i = 2; i < argc; ++i) {
    value_fault |= std::string(argv[i]) == "+fault-value";
    assertion_fault |= std::string(argv[i]) == "+fault-assert";
  }
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    model->initialize();
    std::ifstream vectors(argv[1]);
    if (!vectors)
      throw std::runtime_error("missing independent Vortex DPI vectors");
    constexpr const char *outputs[] = {"low_product", "high_product", "quotient",
                                       "remainder", "uuid"};
    unsigned cycle = 0, checked = 0;
    std::array<uint64_t, 16> row;
    while (vectors >> std::hex >> row[0]) {
      for (unsigned i = 1; i < row.size(); ++i)
        if (!(vectors >> std::hex >> row[i]))
          throw std::runtime_error("truncated independent Vortex DPI vector");
      model->advance(uint64_t(cycle) * 2, [&] {
        model->signal("clock").set(0);
        constexpr const char *inputs[] = {
            "reset", "enable", "arithmetic_enable", "signed_a", "signed_b",
            "signed_division", "a", "b", "warp", "pc", "trace_enable"};
        for (unsigned i = 0; i < 11; ++i)
          model->signal(inputs[i]).set(row[i]);
        model->signal("assertion_failure").set(assertion_fault && cycle >= 10);
      });
      if (assertion_fault && !row[0] && row[1])
        std::printf("ASSERT_CALL %u\n", cycle);
      model->advance(uint64_t(cycle) * 2 + 1,
                     [&] { model->signal("clock").set(1); });
      for (unsigned i = 0; i < 5; ++i) {
        auto expected = row[11 + i];
        if (value_fault && cycle == 1 && i == 0)
          expected ^= 1;
        auto actual = model->signal(outputs[i]).get();
        if (actual != expected) {
          std::cerr << "Vortex DPI mismatch cycle=" << cycle
                    << " output=" << outputs[i] << " actual=" << std::hex
                    << actual << " expected=" << expected << '\n';
          return 1;
        }
        ++checked;
      }
      ++cycle;
    }
    if (!vectors.eof() || cycle < 2000)
      throw std::runtime_error("incomplete independent Vortex DPI corpus");
    std::printf("PASS: %u original Vortex DPI results across %u cycles\n",
                checked, cycle);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
