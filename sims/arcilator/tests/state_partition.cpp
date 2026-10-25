// Independent complete-state and foreign-effect oracle for state partitioning.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void state_partition_fixture_eval(void *);
extern "C" void state_partition_array_eval(void *);
extern "C" void state_partition_alias_eval(void *);
extern "C" void state_partition_versions_eval(void *);
extern "C" void state_partition_effects_eval(void *);
extern "C" void state_partition_atomic_eval(void *);
extern "C" void state_partition_crosshelper_eval(void *);

namespace {
using Image = std::array<uint64_t, 24>;
struct Event {
  uint64_t tag, left, right, a, b, memory;
  bool operator==(const Event &x) const {
    return tag == x.tag && left == x.left && right == x.right && a == x.a &&
           b == x.b && memory == x.memory;
  }
};
Image *active;
std::vector<Event> actualEvents;
std::array<uint64_t, 8> *activeArray;
std::array<uint64_t, 8> *activeGuard;

uint64_t rotate(uint64_t x) { return (x << 7) | (x >> 57); }
uint64_t mix(uint64_t x, uint64_t input, uint64_t b, unsigned rounds) {
  for (unsigned i = 0; i < rounds; ++i) {
    x = rotate((x ^ input) + (uint64_t(0x13579) + i * 97));
    x ^= b;
  }
  return x;
}

uint64_t event(Image &state, std::vector<Event> &events, uint64_t tag,
               uint64_t left, uint64_t right) {
  const unsigned index = (state[2] >> (tag == 1 ? 2 : 4)) & 3;
  const uint64_t a = state[0], b = state[1], memory = state[20 + index];
  events.push_back({tag, left, right, a, b, memory});
  if (tag == 1) {
    state[0] += 0x101;
    state[1] ^= 0x55;
    state[20 + index] ^= 0x91b273c4d5e6f708ULL;
  } else if (tag == 2) {
    state[0] ^= 0x12345678abcdef01ULL;
    state[20 + index] += 17;
  }
  return (left + rotate(right)) ^ a ^ memory ^ tag;
}

void reference(Image &s, std::vector<Event> &events) {
  const uint64_t oldA = s[0], oldB = s[1], input = s[2], mask = s[3];
  const uint64_t x = mix(oldA, input, oldB, 24);
  const bool edge = s[4] && !s[5];
  const unsigned index = input & 3;
  s[5] = s[4];
  s[8] = oldA;
  s[10] = x;
  s[11] = s[20 + index];
  s[13] = oldA;
  if (edge) {
    if (s[7]) {
      s[0] = 0;
      s[1] = 0x6a09e667f3bcc909ULL;
    } else if (s[6]) {
      s[0] = oldA + input;
      s[14] = s[0];
      s[20 + index] = (s[20 + index] & ~mask) | (x & mask);
      s[12] = s[20 + index];
      // This use intentionally occurs after overwriting a's storage.
      s[18] = mix(oldA, input, oldB, 12);
      s[16] = event(s, events, 1, oldA, s[14]);
      s[0] += 9;
      s[15] = s[0];
      s[1] = oldB ^ x;
      s[19] = event(s, events, 2, oldB, s[1]);
      s[9] = s[0];
      s[12] = s[20 + index];
    }
  }
  s[17] = event(s, events, 3, s[0], s[1]);
}

uint64_t randomWord(uint64_t &seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}
} // namespace

extern "C" uint64_t state_partition_event(uint64_t tag, uint64_t left,
                                           uint64_t right) {
  if (!active) std::abort();
  return event(*active, actualEvents, tag, left, right);
}

extern "C" void state_partition_array_event() {
  if (!activeArray) std::abort();
  (*activeArray)[0] ^= 0x8e19a7c23d45f601ULL;
  (*activeArray)[1] += 0xabc123;
}

extern "C" void state_partition_guard_event() {
  if (!activeGuard) std::abort();
  (*activeGuard)[0] ^= 0xe195a7d2b4c68f03ULL;
}

int main() {
  uint64_t seed = 0x93a7c5e19b2d406fULL, hash = 0;
  unsigned steps = 0, callbackChecks = 0;
  for (unsigned recreation = 0; recreation != 3; ++recreation) {
    Image actual;
    for (auto &word : actual) word = randomWord(seed);
    actual[4] = actual[5] = 0;
    actual[6] = actual[7] = 0;
    Image expected = actual;
    for (unsigned sample = 0; sample != 2048; ++sample) {
      actual[2] = expected[2] = randomWord(seed);
      actual[3] = expected[3] = sample % 7 == 0 ? 0 :
          sample % 11 == 0 ? ~uint64_t(0) : randomWord(seed);
      actual[4] = expected[4] = (sample / 3 + sample) & 1;
      actual[6] = expected[6] = sample % 5 != 0;
      actual[7] = expected[7] = sample % 29 == 0;
      actualEvents.clear();
      std::vector<Event> expectedEvents;
      reference(expected, expectedEvents);
      active = &actual;
      state_partition_fixture_eval(actual.data());
      active = nullptr;
      if (actual != expected || actualEvents != expectedEvents) {
        std::fprintf(stderr, "oracle mismatch recreation=%u sample=%u\n",
                     recreation, sample);
        for (unsigned i = 0; i != actual.size(); ++i)
          if (actual[i] != expected[i])
            std::fprintf(stderr, "word[%u] actual=%016llx expected=%016llx\n", i,
                         (unsigned long long)actual[i],
                         (unsigned long long)expected[i]);
        if (actualEvents != expectedEvents)
          std::fprintf(stderr, "callback trace mismatch: actual=%zu expected=%zu\n",
                       actualEvents.size(), expectedEvents.size());
        return 1;
      }
      for (uint64_t word : actual) hash = rotate(hash) ^ word;
      callbackChecks += actualEvents.size();
      ++steps;
    }
  }
  for (unsigned sample = 0; sample != 2048; ++sample) {
    std::array<uint64_t, 8> actual;
    for (auto &word : actual) word = randomWord(seed);
    auto expected = actual;
    expected[0] ^= 0x8e19a7c23d45f601ULL;
    expected[1] += 0xabc123;
    expected[2] = expected[0] ^ 0xabcdef;
    expected[3] = expected[0] + expected[1];
    activeArray = &actual;
    state_partition_array_eval(actual.data());
    activeArray = nullptr;
    if (actual != expected) {
      std::fprintf(stderr, "array reference oracle mismatch sample=%u\n", sample);
      return 1;
    }
    for (auto &word : actual) word = randomWord(seed);
    expected = actual;
    const auto old = expected[0];
    // Deliberately overlapping storage views: the second byte aliases i64 a.
    reinterpret_cast<unsigned char *>(expected.data())[1] = 0xa5;
    expected[1] = old;
    expected[2] = expected[0];
    state_partition_alias_eval(actual.data());
    if (actual != expected) {
      std::fprintf(stderr, "partial alias oracle mismatch sample=%u\n", sample);
      return 1;
    }
    for (auto &word : actual) word = randomWord(seed);
    expected = actual;
    expected[0] += expected[1];
    expected[2] = mix(actual[0], actual[1], 7, 12);
    expected[3] = expected[0];
    state_partition_versions_eval(actual.data());
    if (actual != expected) {
      std::fprintf(stderr, "version guard oracle mismatch sample=%u\n", sample);
      return 1;
    }
    for (auto &word : actual) word = randomWord(seed);
    expected = actual;
    expected[0] ^= 0xe195a7d2b4c68f03ULL;
    expected[2] = mix(actual[0], actual[1], 7, 12);
    expected[3] = expected[0];
    activeGuard = &actual;
    state_partition_effects_eval(actual.data());
    activeGuard = nullptr;
    if (actual != expected) {
      std::fprintf(stderr, "effect epoch oracle mismatch sample=%u\n", sample);
      return 1;
    }
    for (auto &word : actual) word = randomWord(seed);
    expected = actual;
    expected[0] += expected[1];
    expected[2] = actual[0] ^ expected[0];
    expected[3] = expected[0];
    state_partition_atomic_eval(actual.data());
    if (actual != expected) {
      std::fprintf(stderr, "one-helper old/new oracle mismatch sample=%u\n", sample);
      return 1;
    }
    for (auto &word : actual) word = randomWord(seed);
    expected = actual;
    expected[0] += expected[1];
    expected[2] = mix(actual[0], actual[1], 7, 12);
    expected[3] = actual[0] ^ expected[0];
    state_partition_crosshelper_eval(actual.data());
    if (actual != expected) {
      std::fprintf(stderr, "cross-helper old/new oracle mismatch sample=%u\n", sample);
      return 1;
    }
  }
  std::printf("PASS steps=%u state_bytes=%zu callback_checks=%u "
              "array_alias_checks=4096 version_effect_checks=4096 "
              "write_helper_checks=4096 hash=%016llx\n", steps, sizeof(Image),
              callbackChecks, (unsigned long long)hash);
}
