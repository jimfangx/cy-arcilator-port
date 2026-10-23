#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  std::array<unsigned, 2> widths{5, 7}, biases{9, unsigned(-7)}, lengths{4, 5};
  std::array<unsigned, 2> q{}, sampled{};
  unsigned small = 0, wide = 0;
  auto check = [&] {
    for (unsigned i = 0; i < 2; ++i) {
      std::string prefix = i ? "b_" : "a_";
      auto mask = (1u << widths[i]) - 1;
      auto value = wide & mask;
      auto sign_extended = value;
      if (value & (1u << (widths[i] - 1)))
        sign_extended |= 511 ^ mask;
      auto expect = [&](const char *name, unsigned expected) {
        auto actual = model->signal(prefix + name).get();
        if (actual != expected) {
          std::fprintf(stderr, "%s%s: actual %x expected %x small %x wide %x\n",
                       prefix.c_str(), name, unsigned(actual), expected, small,
                       wide);
          throw std::runtime_error("SV port connection mismatch: " + prefix +
                                   name);
        }
      };
      expect("narrow", small);
      expect("unsigned", value);
      expect("signed", sign_extended);
      expect("truncated", (value ^ biases[i]) & 31);
      expect("defaults", mask ^ 2);
      expect("flags", (lengths[i] << 2) | 1);
      expect("q", q[i]);
      expect("sampled", sampled[i]);
    }
  };
  model->advance(0);
  check();
  for (unsigned cycle = 0; cycle < 64; ++cycle) {
    small = cycle & 7;
    wide = cycle * 79 & 511;
    bool reset = cycle == 0 || cycle == 19;
    model->advance(cycle * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("small").set(small);
      model->signal("wide").set(wide);
    });
    check();
    sampled = q;
    for (unsigned i = 0; i < 2; ++i) {
      auto mask = (1u << widths[i]) - 1;
      q[i] = reset ? biases[i] & mask : (wide ^ small ^ biases[i]) & mask;
    }
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check();
    small ^= 7;
    wide ^= 511;
    model->advance(cycle * 1000 + 750, [&] {
      model->signal("small").set(small);
      model->signal("wide").set(wide);
    });
    check();
  }
  std::puts(
      "PASS: SV input/output width rules, signed and unsigned extension, "
      "truncation, nonzero omitted-input defaults, floating input, omitted "
      "output, keyword ports, parameters and pre-edge sampling");
}
