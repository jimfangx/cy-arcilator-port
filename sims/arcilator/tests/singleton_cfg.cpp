#include <cstdint>
#include <cstdio>
extern "C" uint64_t singleton_cfg(uint64_t, uint8_t, bool);
int main() {
  uint64_t random = 0x617263696c61746full;
  unsigned count = 0;
  for (unsigned v = 0; v < 2048; v++) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    for (unsigned limit = 0; limit < 16; limit++)
      for (unsigned mode = 0; mode < 2; mode++) {
        uint64_t payload = (random & 8191) ^ 37;
        for (unsigned i = 0; i < limit; i++)
          if (!(random >> 63))
            payload = ((mode ? 0 : payload) + 17 + i +
                       ((limit & 3) == 0   ? 1
                        : (limit & 3) == 1 ? 3
                                           : 7)) &
                      8191;
        auto expected = payload | (uint64_t(limit) << 16);
        auto actual = singleton_cfg(random, limit, mode);
        if (actual != expected) {
          std::fprintf(stderr, "singleton CFG %llx %u %u: %llx != %llx\n",
                       (unsigned long long)random, limit, mode,
                       (unsigned long long)actual,
                       (unsigned long long)expected);
          return 1;
        }
        count++;
      }
  }
  std::printf(
      "PASS: %u singleton CFG checks, nested aggregate constants, wide "
      "indices, bounded get/inject, switches, zero-width branch arguments and "
      "function attributes\n",
      count);
}
