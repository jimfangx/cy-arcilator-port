#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Image {
  uint64_t field[128];
  uint64_t nested[4][2];
};
static_assert(sizeof(Image) == 136 * sizeof(uint64_t));
extern "C" void aggregate_store(void *, void *, const void *, void *, uint64_t,
                                bool);
extern "C" void aggregate_phi(void *, const void *, uint64_t, bool);
static Image before_probe;
static void *probe_out, *probe_in;
static uint64_t probes = 0;
extern "C" uint64_t external_probe(void *out, void *in, uint64_t seed) {
  if (out != probe_out || in != probe_in ||
      std::memcmp(out, &before_probe, sizeof(Image))) {
    std::fprintf(stderr,
                 "aggregate writes became visible before original store\n");
    std::abort();
  }
  uint64_t word;
  std::memcpy(&word, static_cast<uint8_t *>(in) + 7 * sizeof(word),
              sizeof(word));
  word ^= 0xa511e9b3c4d28760ULL;
  std::memcpy(static_cast<uint8_t *>(in) + 7 * sizeof(word), &word,
              sizeof(word));
  ++probes;
  return seed ^ 0x0538c91ae74d26bfULL;
}
static void compare(const void *got, const Image &expected, uint64_t &checks) {
  std::array<uint64_t, 136> actual, wanted;
  std::memcpy(actual.data(), got, sizeof(Image));
  std::memcpy(wanted.data(), &expected, sizeof(Image));
  for (unsigned i = 0; i < actual.size(); ++i, ++checks)
    if (actual[i] != wanted[i]) {
      std::fprintf(stderr, "aggregate lane %u mismatch: got %llx want %llx\n",
                   i, static_cast<unsigned long long>(actual[i]),
                   static_cast<unsigned long long>(wanted[i]));
      std::abort();
    }
}
int main() {
  uint64_t checks = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    const uint64_t seed =
        (uint64_t(trial) * 0x94d049bb133111ebULL) ^ 0x0123456789abcdefULL;
    const bool enabled = (trial & 1) != 0;
    Image input{}, output{}, snapshot{}, replacement{};
    for (unsigned i = 0; i < 128; ++i) {
      input.field[i] = seed ^ (uint64_t(i + 1) * 0x9e3779b97f4a7c15ULL);
      output.field[i] = ~input.field[i];
    }
    for (unsigned i = 0; i < 4; ++i)
      for (unsigned j = 0; j < 2; ++j) {
        input.nested[i][j] = seed + i * 97 + j;
        output.nested[i][j] = ~input.nested[i][j];
        replacement.nested[i][j] = seed ^ (i * 307 + j * 173);
      }
    for (unsigned mode = 0; mode < 5; ++mode) {
      std::array<uint8_t, sizeof(Image) + 8> in_bytes, out_bytes,
          snapshot_bytes;
      const unsigned misalign = mode == 4 ? 1 : 0;
      void *in = in_bytes.data() + misalign;
      void *out = mode == 1 ? in : out_bytes.data() + misalign;
      void *snap = mode == 2 ? out : snapshot_bytes.data() + misalign;
      std::memcpy(in, &input, sizeof(Image));
      if (out != in)
        std::memcpy(out, &output, sizeof(Image));
      const void *tail =
          (mode == 2 || mode == 3)
              ? static_cast<const void *>(static_cast<uint8_t *>(out) +
                                          offsetof(Image, nested))
              : static_cast<const void *>(replacement.nested);
      Image expected = input, expected_snapshot = input;
      uint64_t tail_copy[4][2];
      std::memcpy(tail_copy, tail, sizeof(tail_copy));
      for (unsigned i = 0; i < 32; ++i) {
        const unsigned index = (i * 37 + 13) % 128;
        if (enabled)
          expected.field[index] =
              seed ^ (uint64_t(i + 1) * 0xd1342543de82ef95ULL);
        if (i == 15)
          expected_snapshot = expected;
      }
      expected.field[1] = seed ^ 0x0538c91ae74d26bfULL;
      std::memcpy(expected.nested, tail_copy, sizeof(tail_copy));
      expected.nested[2][1] = (seed ^ 0x0538c91ae74d26bfULL) + 19;
      before_probe =
          snap == out ? expected_snapshot : (out == in ? input : output);
      probe_out = out;
      probe_in = in;
      aggregate_store(out, in, tail, snap, seed, enabled);
      compare(out, expected, checks);
      if (snap != out)
        compare(snap, expected_snapshot, checks);
      // A PHI root must keep its snapshot semantics for both incoming edges.
      Image phi_expected = enabled ? input : Image{};
      for (unsigned i = 0; i < 12; ++i)
        phi_expected.field[i] = seed + i;
      std::memcpy(in, &input, sizeof(Image));
      aggregate_phi(out, in, seed, enabled);
      compare(out, phi_expected, checks);
    }
  }
  if (probes != 640)
    std::abort();
  std::printf("PASS: %llu aggregate-store lanes, 640 ordered callbacks\n",
              static_cast<unsigned long long>(checks));
}
