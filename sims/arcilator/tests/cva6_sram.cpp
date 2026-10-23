#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  std::array<std::array<uint64_t, 256>, 2> memory{};
  std::array<uint64_t, 2> dn{};
  uint64_t registered = 0, peer0 = 0, peer1 = 0;
  uint64_t rng = 0x826b13a90fc475de;
  unsigned checks = 0;
  auto verify = [&](unsigned cycle) {
    for (auto item : {std::pair{"q0", dn[0]},
                      {"q1", registered},
                      {"peer0", peer0},
                      {"peer1", peer1}}) {
      auto actual = runtime->signal(item.first).get();
      if (actual != item.second) {
        std::fprintf(stderr, "cycle %u %s actual=%016llx expected=%016llx\n",
                     cycle, item.first, static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(item.second));
        throw std::runtime_error("original CVA6 SRAM semantics changed");
      }
      ++checks;
    }
  };
  for (unsigned cycle = 0; cycle < 2048; ++cycle) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    bool reset = cycle < 3 || cycle == 1100 || cycle == 1101;
    bool fill = cycle >= 3 && cycle < 259;
    bool sweep = cycle >= 259 && cycle < 515;
    bool select = fill || sweep || (rng & 3) != 0;
    bool write = fill || (!sweep && ((rng >> 5) & 1));
    uint8_t address = fill ? cycle - 3 : sweep ? cycle - 259 : rng >> 24;
    uint8_t mask = fill ? 255 : cycle % 11 == 0 ? 0 : rng >> 40;
    runtime->advance(cycle * 1000 + 100, [&] {
      runtime->signal("clock").set(0);
      runtime->signal("reset").set(reset);
      runtime->signal("select").set(select);
      runtime->signal("write").set(write);
      runtime->signal("address").set(address);
      runtime->signal("data").set(rng);
      runtime->signal("mask").set(mask);
    });
    if (reset)
      registered = 0;
    verify(cycle);
    peer0 = dn[0];
    peer1 = registered;
    registered = reset ? 0 : dn[1];
    for (unsigned bank = 0; bank < 2; ++bank) {
      auto index = uint8_t(address ^ (bank ? 165 : 0));
      auto data = rng ^ (bank ? 11961589489789340985ULL : 0);
      if (reset) {
        memory[bank].fill(0);
      } else if (select) {
        dn[bank] = memory[bank][index];
        if (write)
          for (unsigned byte = 0; byte < 8; ++byte)
            if (mask >> byte & 1) {
              auto field = uint64_t(255) << (byte * 8);
              memory[bank][index] =
                  (memory[bank][index] & ~field) | (data & field);
            }
      }
    }
    runtime->advance(cycle * 1000 + 600,
                     [&] { runtime->signal("clock").set(1); });
    verify(cycle);
  }
  std::printf("PASS: %u original CVA6 SRAM comparisons, byte enables, "
              "read-before-write, idle hold, reset, optional output register "
              "and peer pre-edge sampling\n",
              checks);
}
