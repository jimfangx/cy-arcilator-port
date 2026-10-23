#include "native/evaluation.h"
#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <vector>

static std::array<std::vector<uint32_t>, 16> arguments;
static uint32_t transform(unsigned site, uint32_t value) {
  unsigned shift = site + 1;
  return ((value << shift) | (value >> (32 - shift))) ^
         (0x9e3779b9u * (site + 1));
}
extern "C" uint32_t chipyard_dpi_chain(uint32_t site, uint32_t value) {
  if (chipyard_arc_is_sampling())
    throw std::runtime_error(
        "combinational DPI executed while registers were sampling");
  auto &log = arguments.at(site);
  if (!log.empty() && log.back() == value)
    throw std::runtime_error("stable internal DPI input called again");
  log.push_back(value);
  return transform(site, value);
}
int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t time = 0;
  uint32_t count = 0, expected = 0;
  auto expect = [&] {
    if (model->signal("sampled").get() != expected ||
        model->signal("count").get() != count) {
      std::fprintf(
          stderr,
          "chain mismatch time=%llu sampled=%08llx/%08x count=%llu/%u\n",
          (unsigned long long)model->time(),
          (unsigned long long)model->signal("sampled").get(), expected,
          (unsigned long long)model->signal("count").get(), count);
      for (unsigned site = 0; site < 16; ++site)
        std::fprintf(stderr, "site=%u calls=%zu last=%08x result=%08x\n", site,
                     arguments[site].size(),
                     arguments[site].empty() ? 0 : arguments[site].back(),
                     arguments[site].empty()
                         ? 0
                         : transform(site, arguments[site].back()));
      throw std::runtime_error(
          "internal DPI chain was not settled before register sampling");
    }
  };
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    uint32_t data = (cycle / 3) * 0x01234567u;
    bool reset = cycle % 17 == 0;
    // Data and the clock edge change together. No intermediate DPI result is
    // a public port, so output-only settling cannot observe the propagation.
    model->advance(time++, [&] { model->signal("clock").set(0); });
    expect();
    if (reset) {
      count = expected = 0;
    } else {
      expected = data ^ count;
      for (unsigned site = 0; site < 16; ++site)
        expected = transform(site, expected);
      ++count;
    }
    model->advance(time++, [&] {
      model->signal("data").set(data);
      model->signal("reset").set(reset);
      model->signal("clock").set(1);
    });
    expect();
    std::array<size_t, 16> before;
    for (unsigned site = 0; site < 16; ++site)
      before[site] = arguments[site].size();
    model->advance(time++);
    expect();
    for (unsigned site = 0; site < 16; ++site)
      if (before[site] != arguments[site].size())
        throw std::runtime_error("unchanged internal chain activated again");
  }
  std::puts("PASS: 512 hidden 16-stage DPI chain cycles, simultaneous "
            "data/clock changes, synchronous reset, pre-edge sampling, "
            "stable-input guards and deferred post-edge activations");
}
