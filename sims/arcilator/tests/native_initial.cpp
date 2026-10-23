#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  const std::array<unsigned, 2> sizes{8, 17}, biases{3, 6161};
  std::array<std::vector<uint64_t>, 2> memories;
  std::array<uint64_t, 2> scalars;
  std::array<bool, 2> previous{};
  for (unsigned bank = 0; bank < 2; ++bank) {
    for (unsigned i = 0; i < sizes[bank]; ++i)
      memories[bank].push_back((i * 73 + 17 + biases[bank]) & 8191);
    scalars[bank] = (biases[bank] + 142) & 255;
  }
  auto check = [&](unsigned index) {
    const std::array<const char *, 2> values{"first", "second"};
    const std::array<const char *, 2> scalarNames{"scalar_first",
                                                  "scalar_second"};
    for (unsigned bank = 0; bank < 2; ++bank) {
      if (model->signal(scalarNames[bank]).get() != scalars[bank] ||
          model->signal(values[bank]).get() !=
              memories[bank][index % sizes[bank]]) {
        std::fprintf(stderr, "initial memory mismatch: bank %u, address %u\n",
                     bank, index);
        throw std::runtime_error(
            "native initial values or clocked write mismatch");
      }
    }
  };
  // Every nonzero initializer must be visible before the first clock edge.
  for (unsigned i = 0; i < 17; ++i) {
    model->advance(i * 10, [&] { model->signal("raddr").set(i); });
    check(i);
  }
  for (unsigned step = 0; step < 180; ++step) {
    std::array<bool, 2> clocks{bool((step / 2) & 1), bool((step / 3) & 1)};
    bool reset = (step >= 5 && step < 11) || (step >= 71 && step < 79);
    bool enable = step % 5 != 3;
    unsigned address = (step * 5 + 1) % 17;
    uint64_t data = (step * 293 + 2031) & 8191;
    for (unsigned bank = 0; bank < 2; ++bank) {
      if (clocks[bank] && !previous[bank]) {
        if (reset) {
          std::fill(memories[bank].begin(), memories[bank].end(),
                    (biases[bank] + 37) & 8191);
          scalars[bank] = (biases[bank] + 11) & 255;
        } else if (enable) {
          memories[bank][address % sizes[bank]] = data;
          scalars[bank] = (scalars[bank] + 3) & 255;
        }
      }
    }
    auto now = 1000 + step * 1000;
    model->advance(now, [&] {
      model->signal("clock").set(clocks[0]);
      model->signal("clock_b").set(clocks[1]);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("waddr").set(address);
      model->signal("data").set(data);
    });
    for (unsigned i = 0; i < 17; ++i) {
      model->advance(now + 10 + i * 10, [&] {
        model->signal("raddr").set(i);
        model->signal("data").set(data ^ 8191);
      });
      check(i);
    }
    previous = clocks;
  }
  std::puts("PASS: nonzero loop-initialized scalar and 13-bit memory contents, "
            "independent instances/clocks, dynamic writes, enable and reset");
}
