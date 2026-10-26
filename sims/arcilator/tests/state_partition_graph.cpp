// Independent executable oracle for graph scheduling of versioned state.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" void graph_interleaved_eval(void *);
extern "C" void graph_unrelated_eval(void *);
extern "C" void graph_raw_eval(void *);
extern "C" void graph_war_eval(void *);
extern "C" void graph_waw_eval(void *);
extern "C" void graph_branch_eval(void *);
extern "C" void graph_shared_eval(void *);
extern "C" void graph_barriers_eval(void *);
#ifdef GRAPH_LINK_PROBES
extern "C" uint64_t state_partition_graph_link_probe(uint64_t);
extern "C" uint64_t state_partition_graph_hidden_probe(uint64_t);
extern "C" uint64_t state_partition_graph_mutable;
extern "C" const uint64_t state_partition_graph_published;
#endif

namespace {
using State = std::array<uint64_t, 32>;
struct Event {
  uint64_t tag, left, right, a, b, c, phase;
  bool operator==(const Event &x) const {
    return tag == x.tag && left == x.left && right == x.right &&
           a == x.a && b == x.b && c == x.c && phase == x.phase;
  }
};
State *active;
std::vector<Event> actualTrace;
uint64_t rotate(uint64_t x) { return (x << 7) | (x >> 57); }
uint64_t work(uint64_t x, uint64_t y, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    x = rotate(x + y + (0x1739 + i * 71)) ^ y;
  return x;
}
uint64_t event(State &s, std::vector<Event> &trace, uint64_t tag,
               uint64_t left, uint64_t right) {
  trace.push_back({tag, left, right, s[0], s[1], s[2], s[30]});
  const uint64_t result = rotate(left ^ right) + s[0] + tag;
  s[30] += 1;
  if (tag & 1) {
    s[0] ^= rotate(left + tag);
    s[1] += right;
  } else {
    s[2] ^= left;
  }
  return result;
}
void reference(unsigned model, State &s, std::vector<Event> &trace) {
  const uint64_t a = s[0], b = s[1], c = s[2], d = s[3], input = s[4];
  switch (model) {
  case 0: {
    uint64_t x = a, y = b, z = c, q = d;
    for (unsigned i = 0; i < 12; ++i) {
      const uint64_t k = 0x1729 + i * 97;
      x = rotate(x + input + k) ^ input;
      y = rotate(y ^ input) + k;
      z = (z * 3) ^ (input + k);
      q = rotate(q + (input ^ k));
    }
    s[0] = x; s[1] = y; s[2] = z; s[3] = q;
    s[6] = x ^ a; s[7] = y + b; s[8] = z ^ c; s[9] = q + d;
    break;
  }
  case 1:
    s[1] = input;
    s[6] = work(a, input, 20);
    s[7] = a;
    break;
  case 2:
    s[0] = work(input, b, 3);
    s[6] = work(s[0], c, 20);
    s[7] = a ^ s[0];
    for (unsigned i = 0; i < 6; ++i) s[16+i] = s[10+i] ^ 0xabcdef;
    break;
  case 3:
    s[0] = work(input, b, 20);
    s[6] = work(a, c, 3);
    s[7] = a;
    for (unsigned i = 0; i < 6; ++i) s[16+i] = s[10+i] ^ 0xabcdef;
    break;
  case 4:
    s[0] = c + input;
    s[0] = work(b, input, 12);
    s[6] = s[0];
    s[7] = input ^ s[0];
    for (unsigned i = 0; i < 6; ++i) s[16+i] = s[10+i] ^ 0xabcdef;
    break;
  case 5:
    if (s[5] & 1) {
      s[0] = input;
      s[6] = work(a, b, 4);
    } else {
      s[0] = input ^ 0xabcdef;
      s[6] = work(a, c, 4);
    }
    s[7] = a ^ s[0];
    s[15] = event(s, trace, 17, a, s[0]);
    break;
  case 6: {
    const auto shared = work(a, b, 12);
    s[0] = input;
    s[6] = shared + a;
    s[1] = shared ^ input;
    s[7] = shared + b;
    s[8] = shared;
    if (s[5] & 1) s[9] = shared ^ c;
    break;
  }
  case 7:
    s[0] = a + input;
    s[15] = event(s, trace, 19, a, s[0]);
    s[6] = work(a, s[0], 8);
    s[7] = s[0];
    s[14] = event(s, trace, 23, a, s[0]);
    break;
  default:
    std::abort();
  }
}
uint64_t randomWord(uint64_t &seed) {
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}
}

extern "C" uint64_t state_partition_graph_event(uint64_t tag, uint64_t left,
                                                 uint64_t right) {
  if (!active) std::abort();
  return event(*active, actualTrace, tag, left, right);
}

int main() {
  using Eval = void (*)(void *);
  const std::array<Eval, 8> models = {
    graph_interleaved_eval, graph_unrelated_eval, graph_raw_eval, graph_war_eval,
    graph_waw_eval, graph_branch_eval, graph_shared_eval, graph_barriers_eval};
  uint64_t seed = 0xf1a34e907d2bc685ULL, hash = 0;
  unsigned frames = 0, callbacks = 0;
  for (unsigned model = 0; model < models.size(); ++model) {
    State actual;
    for (auto &value : actual) value = randomWord(seed);
    auto expected = actual;
    for (unsigned n = 0; n < 4096; ++n) {
      actual[4] = expected[4] = randomWord(seed);
      actual[5] = expected[5] = n & 1;
      actualTrace.clear();
      std::vector<Event> expectedTrace;
      reference(model, expected, expectedTrace);
      active = &actual;
      models[model](actual.data());
      active = nullptr;
      if (actual != expected || actualTrace != expectedTrace) {
        std::fprintf(stderr, "graph oracle mismatch model=%u frame=%u\n", model, n);
        for (unsigned word = 0; word < actual.size(); ++word)
          if (actual[word] != expected[word])
            std::fprintf(stderr, "word[%u] actual=%016llx expected=%016llx\n",
                         word, (unsigned long long)actual[word],
                         (unsigned long long)expected[word]);
        if (actualTrace != expectedTrace)
          std::fprintf(stderr, "graph callback trace mismatch\n");
        return 1;
      }
      for (uint64_t value : actual) hash = rotate(hash) ^ value;
      callbacks += actualTrace.size();
      ++frames;
    }
  }
  std::printf("PASS models=%zu frames=%u state_bytes=%zu callbacks=%u hash=%016llx\n",
              models.size(), frames, sizeof(State), callbacks,
              (unsigned long long)hash);
#ifdef GRAPH_LINK_PROBES
  const std::array<uint64_t, 4> salts = {17, 0x123456789ULL,
      0x7fffffffffffffffULL, 0x9e3779b97f4a7c15ULL};
  for (unsigned n = 0; n < 2048; ++n) {
    const uint64_t input = randomWord(seed), initial = randomWord(seed);
    state_partition_graph_mutable = initial;
    const auto updated = (initial + input) ^ salts[input & 3];
    const auto output = state_partition_graph_link_probe(input);
    if (output != (updated ^ 0x13579bdfULL) ||
        state_partition_graph_mutable != updated ||
        state_partition_graph_hidden_probe(input) != (input ^ 0x2468ace0ULL) ||
        state_partition_graph_published != 0x13579bdfULL) {
      std::fprintf(stderr, "ThinLTO public/global/hidden oracle mismatch sample=%u\n", n);
      return 1;
    }
  }
  std::printf("PASS link_checks=2048 constant_globals=2 mutable_globals=1 "
              "public_and_hidden_functions=2\n");
#endif
}
