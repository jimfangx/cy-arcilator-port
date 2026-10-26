// Independent oracle for a closed prefix followed by an unavoidable frontier.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void graph_low_frontier_eval(void *);
using State = std::array<uint64_t, 16>;
namespace {
State *active;
uint64_t observedPrefix, observedOld;
uint64_t randomWord(uint64_t &seed) {
  seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
  return seed;
}
}
extern "C" void state_partition_low_frontier_event() {
  if (!active) std::abort();
  observedPrefix = (*active)[2];
  observedOld = (*active)[0];
  (*active)[0] ^= 0x918e7d6c5b4a3902ULL;
  ++(*active)[15];
}
int main() {
  uint64_t seed = 0x73a9e51c2846bd0fULL, hash = 0;
  State actual;
  for (auto &word : actual) word = randomWord(seed);
  auto expected = actual;
  for (unsigned n = 0; n < 8192; ++n) {
    actual[1] = expected[1] = randomWord(seed);
    const uint64_t a = expected[0], x = expected[1];
    uint64_t prefix = a;
    for (unsigned i = 0; i < 4; ++i) prefix = (prefix + a) ^ x;
    const std::array<uint64_t, 5> v = {
      prefix + x, (prefix + x) ^ x, ((prefix + x) ^ x) + a,
      (((prefix + x) ^ x) + a) ^ x, ((((prefix + x) ^ x) + a) ^ x) + x};
    expected[2] = prefix;
    expected[0] ^= 0x918e7d6c5b4a3902ULL;
    ++expected[15];
    for (unsigned i = 0; i < v.size(); ++i) expected[4+i] = v[i];
    expected[9] = v[0] + v[1] + v[2] + v[3] + v[4];
    active = &actual;
    graph_low_frontier_eval(actual.data());
    active = nullptr;
    if (actual != expected || observedPrefix != prefix || observedOld != a) {
      std::fprintf(stderr, "low-frontier oracle mismatch frame=%u\n", n);
      return 1;
    }
    for (uint64_t word : actual) hash = (hash << 9 | hash >> 55) ^ word;
  }
  std::printf("PASS low_frontier_frames=8192 state_bytes=%zu callbacks=8192 hash=%016llx\n",
              sizeof(State), (unsigned long long)hash);
}
