#include "native/runtime.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
static std::vector<std::array<unsigned, 3>> recorded;
extern "C" void local_memory_record(uint32_t before, uint32_t after,
                                    uint32_t prior) {
  recorded.push_back({before, after, prior});
}
int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  model->advance(0);
  using Bits = __uint128_t;
  auto packed = [](Bits value, unsigned bytes) {
    std::vector<uint8_t> out(bytes);
    for (auto &byte : out) {
      byte = value;
      value >>= 8;
    }
    return out;
  };
  auto get = [&](const char *name, unsigned bits) {
    Bits value = 0;
    auto data = model->signal(name).copy();
    for (unsigned i = 0; i < data.size(); ++i)
      value |= Bits(data[i]) << (8 * i);
    return value & ((Bits(1) << bits) - 1);
  };
  uint64_t rng = 1709;
  for (unsigned cycle = 0; cycle < 16384; ++cycle) {
    auto next = [&]() {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      return rng;
    };
    unsigned select = cycle & 1, address = next() % 8, count = next() % 8,
             enable = (cycle >> 1) & 1, advance = (cycle >> 2) & 1;
    Bits seed = (Bits(next() & ((1ULL << 27) - 1)) << 64) | next();
    Bits low = next() % 18;
    if (cycle % 11 == 0)
      low |= Bits(1) << 64;
    if (cycle % 17 == 0)
      low = Bits(1) << 63;
    unsigned data = next() & 131071;
    recorded.clear();
    model->advance(cycle + 1, [&] {
      model->signal("select").set(select);
      model->signal("address").set(address);
      model->signal("count").set(count);
      model->signal("low").assign(packed(low, 9));
      model->signal("data").set(data);
      model->signal("enable").set(enable);
      model->signal("advance").set(advance);
      model->signal("seed").assign(packed(seed, 12));
    });
    Bits a = 0, b = seed;
    for (unsigned i = 0; i < 5; ++i)
      a |= Bits(i + 1) << (13 * i);
    unsigned before = 0, after = 0, scratch = 0, origin = 0, available = 65,
             prior = 0, previousIndex = 0;
    std::vector<std::array<unsigned, 3>> expectedCalls;
    for (unsigned t = 0; t < count; ++t) {
      unsigned index = (address + t) % 8, position = index * 13;
      unsigned valid =
          position < available ? std::min(13u, available - position) : 0;
      unsigned absolute = origin + position;
      Bits &root = select ? b : a;
      Bits mask = valid ? (Bits(1) << valid) - 1 : 0;
      before =
          (before + (valid ? unsigned((root >> absolute) & mask) : 0)) & 8191;
      if (enable && low < valid) {
        unsigned n = std::min(17u, valid - unsigned(low));
        Bits field = ((Bits(1) << n) - 1) << (absolute + unsigned(low));
        root = (root & ~field) |
               ((Bits(data) << (absolute + unsigned(low))) & field);
      }
      after =
          (after + (valid ? unsigned((root >> absolute) & mask) : 0)) & 8191;
      scratch = (scratch + (enable && index < 5 ? (data & 8191) : 0)) & 8191;
      unsigned priorValue =
          t == 0 ? unsigned(a & 8191)
                 : (enable && previousIndex < 5 ? (data & 8191) : 0);
      prior = (prior + priorValue) & 8191;
      expectedCalls.push_back({before, after, priorValue});
      previousIndex = index;
      if (advance) {
        origin += 13;
        available = available > 13 ? available - 13 : 0;
      }
    }
    if (recorded != expectedCalls) {
      std::fprintf(stderr,
                   "local stack DPI cycle %u callback count %zu/%zu or "
                   "values/order changed\n",
                   cycle, recorded.size(), expectedCalls.size());
      return 1;
    }
    if (get("a", 65) != a || get("b", 91) != b || get("before", 13) != before ||
        get("after", 13) != after || get("scratch", 13) != scratch ||
        get("tiny", 65) != (enable && low == 0 ? (data & 1) : 0) ||
        get("prior", 13) != prior) {
      std::fprintf(stderr,
                   "local stack mismatch cycle %u select %u address %u count "
                   "%u enable %u advance %u low %llu outputs before %u/%u "
                   "after %u/%u scratch %u/%u\n",
                   cycle, select, address, count, enable, advance,
                   (unsigned long long)low, unsigned(get("before", 13)), before,
                   unsigned(get("after", 13)), after,
                   unsigned(get("scratch", 13)), scratch);
      return 1;
    }
  }
  std::puts("PASS: 16384 local stack vectors, 65/91-bit roots, cross-root CFG "
            "aliases, loop-carried clipped ranges, 65-bit indices, automatic "
            "arrays, per-iteration initialization, live references to earlier "
            "allocations, sequential visibility and exact DPI callback "
            "values/order/counts");
}
