#include "native/runtime.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {
std::array<unsigned, 2> seeds{}, steps{}, observers{}, ticks{}, discards{},
    handles{}, handle_uses{};
std::array<uint64_t, 2> handle_storage{};
bool startup;
unsigned index(int tag) {
  if (tag != 17 && tag != 29)
    throw std::runtime_error("declaration tag ABI changed");
  return tag == 29;
}
uint64_t seed(int tag) {
  return 0xfedcba9800000000ULL | (uint64_t(tag) * 0x10203);
}
uint64_t derived(int tag) {
  return seed(tag) ^ 0x8000000100000011ULL ^ uint64_t(tag);
}
void require(bool condition, const char *why) {
  if (!condition)
    throw std::runtime_error(why);
}
} // namespace
extern "C" uint64_t declaration_seed(int tag) {
  unsigned i = index(tag);
  require(startup && ++seeds[i] == 1 && steps[i] == 0,
          "declaration seed repeated or wrong startup order");
  return seed(tag);
}
extern "C" uint64_t declaration_step(uint64_t value, int tag) {
  unsigned i = index(tag);
  require(startup && seeds[i] == 1 && ++steps[i] == 1 && value == seed(tag),
          "declaration dependency/order/64-bit ABI mismatch");
  return derived(tag);
}
extern "C" void *declaration_handle(int tag) {
  unsigned i = index(tag);
  require(startup && steps[i] == 1 && ++handles[i] == 1,
          "chandle initializer repeated or reordered");
  return &handle_storage[i];
}
extern "C" uint64_t declaration_handle_value(void *handle, int tag) {
  unsigned i = index(tag);
  require(startup && handles[i] == 1 && ++handle_uses[i] == 1 &&
              handle == &handle_storage[i],
          "declaration chandle dependency/ABI changed");
  return 0x9876543210000000ULL + uint64_t(tag);
}
extern "C" uint64_t declaration_discard(int tag) {
  unsigned i = index(tag);
  require(startup && handle_uses[i] == 1 && ++discards[i] == 1,
          "unused declaration effect dropped, repeated or reordered");
  return 0x8000000012345678ULL;
}
extern "C" void declaration_observe(int tag, uint64_t value, uint64_t next) {
  unsigned i = index(tag);
  require(seeds[0] == 1 && seeds[1] == 1 && steps[0] == 1 && steps[1] == 1 &&
              discards[0] == 1 && discards[1] == 1 && ++observers[i] == 1 &&
              value == seed(tag) && next == derived(tag),
          "time-zero process saw uninitialized declaration");
}
extern "C" uint32_t declaration_tick(int tag, uint32_t data) {
  require(!startup && observers[index(tag)] == 1,
          "clocked DPI ran before initialization");
  ++ticks[index(tag)];
  return data ^ uint32_t(tag);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected binary trace path");
    FILE *trace = std::fopen(argv[1], "wb");
    require(trace, "cannot open declaration trace");
    const std::array<const char *, 10> names{
        {"first", "first_derived", "first_shared", "first_observed",
         "first_live", "second", "second_derived", "second_shared",
         "second_observed", "second_live"}};
    for (unsigned recreation = 0; recreation < 2; ++recreation) {
      seeds = steps = observers = ticks = discards = handles = handle_uses = {};
      startup = true;
      auto model = chipyard_arc::create_model(argc, argv);
      model->initialize();
      require(seeds == std::array<unsigned, 2>{{1, 1}} && steps == seeds &&
                  discards == seeds && handles == seeds &&
                  handle_uses == seeds && handles == seeds &&
                  handle_uses == seeds,
              "DPI declarations did not initialize exactly once");
      startup = false;
      unsigned expected_ticks = 0;
      bool previous = false;
      for (unsigned sample = 0; sample < 24; ++sample) {
        bool clock = bool(sample & 1), reset = sample >= 5 && sample < 10;
        uint32_t data = 0xf1234567U + sample * 0x10203U;
        expected_ticks += clock && !previous && !reset;
        model->advance(sample * 1000, [&] {
          model->signal("clock").set(clock);
          model->signal("reset").set(reset);
          model->signal("data").set(data);
        });
        require(observers == std::array<unsigned, 2>{{1, 1}} &&
                    ticks == std::array<unsigned, 2>{{expected_ticks,
                                                      expected_ticks}},
                "ordinary initial/clocked DPI execution count changed");
        for (unsigned held = 0; held < 2; ++held) {
          if (held)
            model->advance(sample * 1000 + 1, [&] {
              model->signal("data").set(data ^ 0xffffffffU);
            });
          require(seeds == std::array<unsigned, 2>{{1, 1}} && steps == seeds &&
                      discards == seeds && handles == seeds &&
                      handle_uses == seeds &&
                      ticks == std::array<unsigned, 2>{{expected_ticks,
                                                        expected_ticks}},
                  "held evaluation repeated declaration/clocked DPI");
          for (auto name : names) {
            auto bytes = model->signal(name).copy();
            require(std::fwrite(bytes.data(), 1, bytes.size(), trace) ==
                        bytes.size(),
                    "cannot write declaration trace");
          }
        }
        previous = clock;
      }
    }
    require(std::fclose(trace) == 0, "cannot close declaration trace");
    std::puts("PASS native DPI declarations: once-only startup, dependencies, "
              "time-zero observers, clocked calls and recreation");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
