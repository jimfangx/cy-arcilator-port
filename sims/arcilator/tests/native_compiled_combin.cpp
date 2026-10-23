#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using namespace chipyard_arc;

int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected the stock SV reference file");
  auto *reference = std::fopen(argv[1], "rb");
  if (!reference)
    throw std::runtime_error("cannot open the stock SV reference file");
  auto model = create_model(argc, argv);
  model->initialize();
  bool held = false, held2 = false;
  std::array<bool, 4> previous{};
  std::array<unsigned, 4> count{};
  uint64_t rng = 0xb869fa6502327bd7ULL;
  bool clock = false, peer = false, en = false, test = false, en2 = false;
  bool pad = false, ie = false, oe = false, signal = false, reset = true;
  for (unsigned i = 0; i < 16384; ++i) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    // Enable/data setup precedes clock transitions. This also changes enables
    // while clocks are high, without a race at a transparent latch's opening.
    if (i % 2) {
      clock = (rng >> 0) & 1;
      peer = (rng >> 1) & 1;
    } else {
      en = (rng >> 2) & 1;
      test = (rng >> 3) & 1;
      en2 = (rng >> 4) & 1;
      pad = (rng >> 5) & 1;
      ie = (rng >> 6) & 1;
      oe = (rng >> 7) & 1;
      signal = (rng >> 8) & 1;
      reset = i == 0 || i % 97 < 3;
    }
    if (!clock)
      held = en || test;
    bool gate = clock && held;
    if (!gate)
      held2 = en2;
    bool chain = gate && held2, inv = !clock, joined = clock || peer;
    bool nor = !(clock || signal);
    std::array<bool, 4> edges{gate, chain, inv, nor};
    for (unsigned j = 0; j < 4; ++j) {
      if (reset)
        count[j] = 0;
      else if (edges[j] && !previous[j])
        count[j] = (count[j] + 1) & 255;
      previous[j] = edges[j];
    }
    model->advance(i + 1, [&] {
      model->signal("clock").set(clock);
      model->signal("peer").set(peer);
      model->signal("reset").set(reset);
      model->signal("en").set(en);
      model->signal("test").set(test);
      model->signal("second_en").set(en2);
      model->signal("pad").set(pad);
      model->signal("ie").set(ie);
      model->signal("oe").set(oe);
      model->signal("signal").set(signal);
    });
    const std::array<const char *, 11> names{
        "gated",   "chained", "inverted", "joined", "nor_clock", "read",
        "written", "count0",  "count1",   "count2", "count3"};
    const std::array<unsigned, 11> expected{
        gate,     chain,    inv,      joined,  nor, pad && ie, pad && ie && oe,
        count[0], count[1], count[2], count[3]};
    std::array<unsigned char, 11> stock{};
    if (std::fread(stock.data(), 1, stock.size(), reference) != stock.size())
      throw std::runtime_error("truncated stock SV reference");
    for (unsigned j = 0; j < names.size(); ++j) {
      auto actual = model->signal(names[j]).get();
      if (actual != expected[j] || actual != stock[j]) {
        std::fprintf(stderr, "step %u %s oracle %u stock %u native %llu\n", i,
                     names[j], expected[j], unsigned(stock[j]),
                     static_cast<unsigned long long>(actual));
        throw std::runtime_error("native combinational model mismatch");
      }
    }
  }
  if (std::fgetc(reference) != EOF)
    throw std::runtime_error("extra stock SV reference data");
  std::fclose(reference);
  std::puts("PASS: 16384 IO/gate/latch/reset states; 180224 fields match "
            "stock SV and the independent oracle");
}
