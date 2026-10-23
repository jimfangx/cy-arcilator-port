#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using Bits = unsigned __int128;
static std::vector<uint8_t> packed(Bits value, unsigned n = 11) {
  std::vector<uint8_t> out(n);
  for (auto &byte : out) {
    byte = value;
    value >>= 8;
  }
  return out;
}
int main(int argc, char **argv) {
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    Bits initial = (Bits(0x80123) << 64) | 0x456789abcdef1357ULL;
    Bits q = initial, before = 0, after = 0;
    uint64_t rng = 0xa7351ef6d81c462bULL;
    auto random = [&] {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      return rng;
    };
    auto setbit = [](Bits &value, uint64_t bit, bool data) {
      value = (value & ~(Bits(1) << bit)) | (Bits(data) << bit);
    };
    auto check = [&](unsigned cycle) {
      for (auto pair :
           {std::make_pair("q", q), std::make_pair("before", before),
            std::make_pair("after", after)})
        if (model->signal(pair.first).copy() != packed(pair.second)) {
          std::fprintf(stderr, "packed aggregate cycle %u %s mismatch\n", cycle,
                       pair.first);
          for (auto x : model->signal(pair.first).copy())
            std::fprintf(stderr, "%02x", x);
          std::fprintf(stderr, " actual\n");
          for (auto x : packed(pair.second))
            std::fprintf(stderr, "%02x", x);
          std::fprintf(stderr, " expected\n");
          throw std::runtime_error("packed reference projection mismatch");
        }
    };
    model->initialize();
    model->advance(0);
    check(0);
    for (unsigned cycle = 0; cycle < 2048; ++cycle) {
      bool reset = cycle == 0 || cycle == 223 || cycle == 700,
           enable = cycle % 11 != 0;
      unsigned mode = cycle % 4, data = random() & 131071;
      Bits index = cycle % 3 ? random() % 128 : cycle % 84,
           offset = cycle % 3 ? random() % 128 : cycle % 65;
      if (cycle % 13 == 0)
        index = (Bits(1) << 64) | 5;
      if (cycle % 17 == 0)
        offset = (Bits(1) << 64) | 3;
      if (cycle % 19 == 0)
        index = 0x100000005ULL;
      if (cycle % 23 == 0)
        offset = 0x8000000000000003ULL;
      model->advance(cycle * 10 + 1, [&] {
        model->signal("clock").set(0);
        model->signal("reset").set(reset);
        model->signal("enable").set(enable);
        model->signal("mode").set(mode);
        model->signal("data").set(data);
        model->signal("index").assign(packed(index, 9));
        model->signal("offset").assign(packed(offset, 9));
      });
      check(cycle);
      Bits updated = q;
      std::vector<std::pair<unsigned, bool>> pending;
      auto write = [&](bool nba, Bits position, unsigned base, unsigned width,
                       unsigned count, unsigned dataShift) {
        for (unsigned j = 0; j < count; ++j)
          if (position + j < width) {
            unsigned bit = base + unsigned(position) + j;
            bool b = (data >> (j + dataShift)) & 1;
            if (nba)
              pending.emplace_back(bit, b);
            else
              setbit(updated, bit, b);
          }
      };
      if (reset) {
        q = initial;
        before = after = 0;
      } else if (enable) {
        before = q;
        if (mode == 0) {
          write(false, index, 0, 84, 1, 0);
          write(true, offset, 13, 65, 17, 0);
        } else if (mode == 1) {
          write(true, index, 0, 84, 1, 0);
          write(false, offset, 13, 65, 17, 0);
        } else if (mode == 2) {
          write(false, index, 0, 84, 4, 0);
          write(true, offset, 13, 65, 3, 4);
          write(false, 83, 0, 84, 1, 6);
          write(true, 0, 0, 84, 1, 7);
        } else {
          write(false, offset, 0, 13, 13, 0);
          write(true, index, 0, 84, 17, 0);
        }
        after = updated;
        for (auto [bit, b] : pending)
          setbit(updated, bit, b);
        q = updated;
      }
      model->advance(cycle * 10 + 5, [&] { model->signal("clock").set(1); });
      check(cycle);
    }
    std::puts("PASS: 2048 packed aggregate projection cycles, constant/dynamic "
              "bit and range writes, 65-bit indices, clipping, field "
              "boundaries, loops, reset, blocking/NBA ordering");
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
