#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace chipyard_arc;
static std::vector<uint8_t> wide(uint64_t low, unsigned high) {
  std::vector<uint8_t> result(9, 0);
  for (unsigned i = 0; i < 8; ++i)
    result[i] = low >> (i * 8);
  result[8] = high & 1;
  return result;
}
int main(int argc, char **argv) {
  for (unsigned round = 0; round < 3; ++round) {
    auto model = create_model(argc, argv);
    model->initialize();
    bool startup = model->flag("global-startup");
    unsigned total = startup ? 348 : 341, shadow = startup ? 348 : 291;
    unsigned qa = 0, qb = 0, hi = 1;
    uint64_t lo = 0x0123456789abcdefull;
    std::array<unsigned, 3> memory{17, startup ? 500u : 19u,
                                   startup ? 348u : 23u};
    auto check = [&](unsigned cycle, const char *phase) {
      auto scalar = [&](const char *name, unsigned expected) {
        auto actual = model->signal(name).get();
        if (actual != expected) {
          std::fprintf(
              stderr, "round %u cycle %u %s %s: got %llu expected %u\n", round,
              cycle, phase, name, (unsigned long long)actual, expected);
          throw std::runtime_error("native global state mismatch");
        }
      };
      scalar("qa", qa);
      scalar("qb", qb);
      scalar("shadow", shadow);
      scalar("m0", memory[0]);
      scalar("m1", memory[1]);
      scalar("m2", memory[2]);
      if (model->signal("wide").copy() != wide(lo, hi))
        throw std::runtime_error("global wide partial write mismatch");
    };
    const uint64_t indices[] = {0, 1, 12, 63, 64, 65, 128, 0};
    for (unsigned cycle = 0; cycle < 4096; ++cycle) {
      bool ea = cycle % 3 != 0, eb = cycle % 5 != 0;
      unsigned da = (cycle * 71 + 19 + round) & 8191,
               db = (cycle * 157 + 31 + round * 7) & 8191;
      unsigned ia = cycle % 8, ib = (cycle + 3) % 8, aa = cycle % 4,
               ab = (cycle / 3) % 4;
      model->advance(cycle * 12 + 1, [&] {
        model->signal("clock_a").set(0);
        model->signal("clock_b").set(0);
        model->signal("enable_a").set(ea);
        model->signal("enable_b").set(eb);
        model->signal("data_a").set(da);
        model->signal("data_b").set(db);
        model->signal("index_a").assign(wide(indices[ia], ia == 7));
        model->signal("index_b").assign(wide(indices[ib], ib == 7));
        model->signal("address_a").set(aa);
        model->signal("address_b").set(ab);
      });
      check(cycle, "falling");
      auto update = [&](bool enable, unsigned data, unsigned tag,
                        unsigned index, unsigned address, unsigned &q) {
        if (!enable)
          return;
        total = (total + ((data + tag) & 8191)) & 8191;
        shadow = q = total;
        if (index != 7 && indices[index] < 65) {
          if (indices[index] == 64)
            hi = data & 1;
          else {
            auto mask = uint64_t(1) << indices[index];
            lo = (lo & ~mask) | ((data & 1) ? mask : 0);
          }
        }
        if (address < 3)
          memory[address] = data;
      };
      update(ea, da, 3, ia, aa, qa);
      model->advance(cycle * 12 + 5, [&] { model->signal("clock_a").set(1); });
      check(cycle, "a rising");
      update(eb, db, 9, ib, ab, qb);
      model->advance(cycle * 12 + 9, [&] { model->signal("clock_b").set(1); });
      check(cycle, "b rising");
    }
  }
  for (bool conflictingWidth : {false, true}) {
    uint64_t handles[4]{};
    auto model = create_model(argc, argv);
    for (unsigned i = 0; i < 2; ++i) {
      auto binding = make_binding(*model, "ArcSignalMemory");
      binding->instance_name = i ? "conflicting_second" : "conflicting_first";
      binding->ports.emplace(
          "handle", Signal{reinterpret_cast<uint8_t *>(&handles[i * 2]), 64});
      binding->ports.emplace(
          "version",
          Signal{reinterpret_cast<uint8_t *>(&handles[i * 2 + 1]), 64});
      binding->parameters = {{"KEY", "conflict_probe"},
                             {"BITS", i && conflictingWidth ? "65" : "13"},
                             {"INIT", i && !conflictingWidth ? "2" : "1"}};
      model->add_binding(std::move(binding));
    }
    bool rejected = false;
    try {
      model->initialize();
    } catch (const std::runtime_error &error) {
      if (std::string(error.what())
              .find("conflicting native global signal layout") ==
          std::string::npos)
        throw;
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error("inconsistent shared layout accepted");
  }
  std::puts("PASS: 12288 native global cycles, separate imported variants, "
            "shared function statics, passive readers, wide blocking writes, "
            "array NBA writes, invalid indices, model recreation and "
            "conflicting layout rejection");
}
