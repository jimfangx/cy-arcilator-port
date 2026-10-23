#include "native/runtime.h"
#include <cstdio>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    model->initialize();
    uint64_t time = 0, checks = 0, random = 0xa7351ef6d81c462bULL;
    auto next = [&] {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      return random;
    };
    auto check = [&](const std::vector<uint8_t> &data) {
      model->advance(time++, [&] { model->signal("value").assign(data); });
      unsigned slot = 0;
      uint64_t expected = 0;
      auto bit = [&](bool value) { expected |= uint64_t(value) << slot++; };
      auto count = [&](unsigned width) {
        unsigned ones = 0;
        for (unsigned i = 0; i < width; ++i)
          ones += (data[i / 8] >> (i % 8)) & 1;
        return ones;
      };
      for (unsigned width : {1, 3, 7, 13, 65, 129}) {
        unsigned ones = count(width);
        bit(ones <= 1);
        bit(ones == 1);
        bit(false); // Known runtime inputs contain no X/Z bits.
        bit(false); // Equality with an X/Z literal cannot match known input.
        bit(true);
        bit(false);
        bit(true);
      }
      for (unsigned i = 0; i < 2; ++i) {
        unsigned ones = count(129);
        bit(ones <= 1);
        bit(ones == 1);
        bit(false);
      }
      bit(false); // $isunknown through an SV function call.
      bit(false); // Packed struct compared with a literal X mask.
      bit(true);
      bit(false); // Replicated X case pattern cannot match known input.
      bit(false); // Constant arithmetic in a case pattern retains its mask.
      bit(data[0] == 0); // Explicit bit cast maps the literal X to known zero.
      bit(count(129) == 0);
      bit(false);
      bit(true);
      if (model->signal("flags").get() != expected) {
        std::fprintf(
            stderr, "builtins vector %llu: actual %llx expected %llx\n",
            static_cast<unsigned long long>(time),
            static_cast<unsigned long long>(model->signal("flags").get()),
            static_cast<unsigned long long>(expected));
        throw std::runtime_error(
            "known-input bit builtin or case equality changed");
      }
      if (model->signal("constants").get() != 0x25534ddb)
        throw std::runtime_error(
            "literal four-state comparison/builtin changed");
      checks += slot + 30;
    };
    std::vector<uint8_t> data(17);
    for (unsigned value = 0; value < 8192; ++value) {
      data.assign(17, 0);
      data[0] = value;
      data[1] = value >> 8;
      check(data);
    }
    for (unsigned index = 0; index < 129; ++index) {
      data.assign(17, 0);
      data[index / 8] = 1 << (index % 8);
      check(data);
    }
    for (unsigned index = 0; index < 4096; ++index) {
      for (auto &byte : data)
        byte = next();
      data[16] &= 1;
      check(data);
    }
    std::printf(
        "PASS: %llu known-input builtin/case-comparison and literal X/Z "
        "checks; "
        "widths 1/3/7/13/65/129, packed aggregates and function calls\n",
        static_cast<unsigned long long>(checks));
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
