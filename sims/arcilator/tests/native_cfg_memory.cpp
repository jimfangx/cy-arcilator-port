#include "native/runtime.h"
#include <array>
#include <cstdio>
static unsigned beforeSum, afterSum, calls;
extern "C" void cfg_memory_record(uint32_t before, uint32_t after) {
  beforeSum = before;
  afterSum = after;
  ++calls;
}
int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  std::array<std::array<unsigned, 5>, 2> memory{
      {{1, 2, 3, 4, 5}, {11, 12, 13, 14, 15}}};
  auto packed = [](const auto &m) {
    std::vector<uint8_t> out(9);
    for (unsigned i = 0; i < 5; ++i)
      for (unsigned j = 0; j < 13; ++j)
        if ((m[i] >> j) & 1)
          out[(13 * i + j) / 8] |= 1u << ((13 * i + j) % 8);
    return out;
  };
  uint64_t rng = 0xa7351ef6d81c462bULL;
  auto random = [&] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return unsigned(rng);
  };
  model->initialize();
  model->advance(0);
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    unsigned select = random() & 1, address = random() & 7,
             count = random() & 7, low = random() & 15, data = random() & 8191;
    bool enable = cycle % 7 != 0, partialEnable = cycle % 2,
         advanceRef = cycle % 3 == 0;
    model->advance(cycle * 10 + 1, [&] {
      model->signal("clock").set(0);
      model->signal("select").set(select);
      model->signal("address").set(address);
      model->signal("count").set(count);
      model->signal("low").set(low);
      model->signal("data").set(data);
      model->signal("enable").set(enable);
      model->signal("partial_enable").set(partialEnable);
      model->signal("advance_ref").set(advanceRef);
    });
    auto peers = memory;
    auto next = memory;
    unsigned expectBefore = 0, expectAfter = 0;
    for (unsigned i = 0; i < count; ++i) {
      unsigned index = ((address + i) & 7) + (advanceRef ? i : 0);
      unsigned old = index < 5 ? memory[select][index] : 0;
      expectBefore = (expectBefore + old) & 8191;
      unsigned updated = old;
      if (index < 5 && enable) {
        unsigned mask = partialEnable && low < 13 ? ((255u << low) & 8191) : 0;
        updated = (old & ~mask) | ((data << low) & mask);
        memory[select][index] = updated;
        next[select][index] = data;
      }
      expectAfter = (expectAfter + updated) & 8191;
    }
    // Blocking writes and peer reads share the SV active region. Either
    // ordering is allowed; NBA writes must remain invisible until commit.
    // With partialEnable false, both allowed snapshots equal the old state.
    auto blocking = memory;
    model->advance(cycle * 10 + 5, [&] { model->signal("clock").set(1); });
    memory = next;
    if (model->signal("sensitivity").get() != (select ? data : 0)) {
      std::fprintf(
          stderr,
          "cycle %u selected root sensitivity actual %llu expected %u\n", cycle,
          (unsigned long long)model->signal("sensitivity").get(),
          select ? data : 0);
      return 1;
    }
    if (calls != cycle + 1 || beforeSum != expectBefore ||
        afterSum != expectAfter) {
      std::fprintf(stderr,
                   "cycle %u calls %u before %u/%u after %u/%u address %u "
                   "count %u low %u enable %u\n",
                   cycle, calls, beforeSum, expectBefore, afterSum, expectAfter,
                   address, count, low, enable);
      return 1;
    }
    for (unsigned n = 0; n < 2; ++n) {
      std::string s = n ? "b" : "a";
      if (model->signal(s).copy() != packed(memory[n]) ||
          (model->signal("peer_" + s).copy() != packed(peers[n]) &&
           model->signal("peer_" + s).copy() != packed(blocking[n]))) {
        std::fprintf(stderr,
                     "cycle %u memory/peer %s mismatch bank %u peer %u address "
                     "%u count %u low %u data %u enable %u\n",
                     cycle, s.c_str(),
                     model->signal(s).copy() != packed(memory[n]),
                     model->signal("peer_" + s).copy() != packed(peers[n]),
                     address, count, low, data, enable);
        for (auto port : {s, "peer_" + s}) {
          for (auto x : model->signal(port).copy())
            std::fprintf(stderr, "%02x", x);
          std::fprintf(stderr, " actual %s\n", port.c_str());
        }
        for (auto m : {memory[n], peers[n]}) {
          for (auto x : packed(m))
            std::fprintf(stderr, "%02x", x);
          std::fprintf(stderr, " expected\n");
        }
        return 1;
      }
    }
  }
  std::puts("PASS: 4096 CFG memory cycles, cross-root branches, loop-carried "
            "references, i0 outer index, invalid indices, clipped partial "
            "writes, blocking/NBA order, peer sampling and root sensitivity");
}
