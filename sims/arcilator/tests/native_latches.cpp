#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <cstring>

using Wide = std::array<uint8_t, 17>;
static Wide bits(uint64_t low, uint64_t high, bool top) {
  Wide value{};
  std::memcpy(value.data(), &low, 8);
  std::memcpy(value.data() + 8, &high, 8);
  value[16] = top;
  return value;
}
static void check_latch_copies(chipyard_arc::Runtime &runtime) {
  unsigned checks = 0;
  for (unsigned width :
       {1u, 7u, 8u, 13u, 16u, 64u, 65u, 129u, 512u, 1023u, 4096u}) {
    unsigned bytes = (width + 7) / 8;
    for (unsigned alias = 0; alias < 4; ++alias) {
      std::vector<uint8_t> storage(bytes * 2 + 8);
      uint8_t enabled = 0;
      unsigned source = alias == 0 ? 0 : alias == 3 ? 3 : 2;
      unsigned target = alias == 0 ? bytes + 2 : alias == 2 ? 3 : 2;
      auto latch = chipyard_arc::make_binding(runtime, "ArcLatch");
      latch->ports = {{"value", {storage.data() + source, width}},
                      {"write", {&enabled, 1}},
                      {"q", {storage.data() + target, width}}};
      latch->initialize();
      for (unsigned step = 0; step < 257; ++step) {
        for (unsigned i = 0; i < storage.size(); ++i)
          storage[i] = uint8_t(step * 73 + i * 37 + (step >> 2));
        enabled = step % 5 != 0;
        auto original = storage, expected = storage;
        // A bit-level snapshot oracle covers dirty padding, identical storage,
        // both directions of overlapping ranges, and disabled transparency.
        if (enabled) {
          for (unsigned bit = 0; bit < bytes * 8; ++bit) {
            auto &byte = expected[target + bit / 8];
            auto mask = uint8_t(1u << (bit % 8));
            bool value =
                bit < width && ((original[source + bit / 8] >> (bit % 8)) & 1);
            byte = (byte & ~mask) | (value ? mask : 0);
          }
        }
        bool changed = false;
        for (unsigned i = 0; i < bytes; ++i)
          changed |= original[target + i] != expected[target + i];
        if (latch->combinational() != changed || storage != expected) {
          std::fprintf(stderr, "latch copy width=%u alias=%u step=%u\n", width,
                       alias, step);
          throw std::runtime_error(
              "native latch byte-copy or change detection mismatch");
        }
        ++checks;
      }
    }
  }
  std::printf("PASS: %u native latch copy cases, 1..4096 bits, "
              "disabled/dirty-padding/overlap snapshots\n",
              checks);
}

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  check_latch_copies(*model);
  Wide data{}, q = bits(0x0123456789abcdef, 0xfedcba9876543210, true),
               nba = bits(0xfedcba9876543210, 0x0123456789abcdef, false),
               aggregate = bits(0xaaaaaaaaaaaaaaaa, 0x5555555555555555, true),
               sampled{}, sampledNBA{};
  bool clock = false, peer = false, enable = false, test = false, open = false,
       nbaOpen = false, gate = false;
  unsigned count = 0, steps = 0;
  auto check = [&] {
    auto verify = [&](const char *name, const Wide &expected) {
      auto actual = model->signal(name).copy();
      if (actual != std::vector<uint8_t>(expected.begin(), expected.end())) {
        std::fprintf(stderr, "latch mismatch %s at step %u\n", name, steps);
        throw std::runtime_error(
            "native transparent-latch value or scheduling mismatch");
      }
    };
    verify("q", q);
    verify("nba_q", nba);
    verify("aggregate_q", aggregate);
    verify("sampled", sampled);
    verify("sampled_nba", sampledNBA);
    if (model->signal("count").get() != count ||
        model->signal("gated_clock").get() != unsigned(clock && gate))
      throw std::runtime_error(
          "native latch-generated clock edge/count mismatch");
  };
  auto step = [&](bool newClock, bool newPeer, bool newEnable, bool newTest,
                  bool newOpen, bool newNBAOpen, Wide next) {
    bool priorGated = clock && gate;
    bool peerRise = newPeer && !peer;
    clock = newClock;
    peer = newPeer;
    enable = newEnable;
    test = newTest;
    open = newOpen;
    nbaOpen = newNBAOpen;
    data = next;
    if (!clock)
      gate = enable || test;
    if (open)
      q = aggregate = data;
    if (peerRise) {
      sampled = q;
      sampledNBA = nba;
    }
    if (nbaOpen)
      nba = data;
    if (!priorGated && clock && gate)
      ++count;
    model->advance(++steps * 100, [&] {
      model->signal("clock").set(clock);
      model->signal("peer_clock").set(peer);
      model->signal("enable").set(enable);
      model->signal("test_enable").set(test);
      model->signal("open").set(open);
      model->signal("nba_open").set(nbaOpen);
      model->signal("data").assign(
          std::vector<uint8_t>(data.begin(), data.end()));
    });
    check();
    model->advance(steps * 100 + 1);
    check();
  };
  model->advance(0);
  check();
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    uint64_t low = UINT64_C(0x13579bdf2468ace0) * cycle + 7;
    auto first = bits(low, ~low, cycle & 1);
    auto second =
        bits(low ^ UINT64_C(0xabcdeffedcba1234), low + cycle, (cycle >> 1) & 1);
    auto third = bits(~low, low ^ (uint64_t(cycle) << 32), (cycle >> 2) & 1);
    bool armed = cycle % 3 != 0, testMode = cycle % 7 == 0;
    // Transparent data/aggregate writes, simultaneous peer sampling and an
    // NBA latch whose new data must publish after that peer's sample.
    step(false, false, armed, testMode, true, true, first);
    step(false, true, armed, testMode, true, true, second);
    step(true, false, armed, testMode, false, false, second);
    // Changes during the clock's high phase cannot open the clock gate.
    step(true, true, !armed, !testMode, false, false, third);
    step(false, false, !armed, !testMode, true, true, third);
    step(true, true, !armed, !testMode, true, true, first);
  }
  std::printf(
      "PASS: %u native latch steps, original Ibex clock gate, high-phase hold, "
      "test enable, 129-bit/aggregate initialization and transparency, NBA "
      "ordering, peer sampling and repeated stable evaluations\n",
      steps);
}
