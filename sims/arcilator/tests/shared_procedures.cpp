
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
extern "C" void evaluate(uint64_t *, uint64_t, uint64_t *);
struct Packet {
  std::array<uint64_t, 64> data;
  uint64_t *ptr;
  uint64_t low, high, byte, a, b;
};
Packet oracle(uint64_t &state, const std::array<uint64_t, 64> &p, uint64_t low,
              uint64_t high, uint64_t tag, uint64_t *ptr) {
  state = (state + p[0] + tag) ^ ((p[1] & 1) ? tag * 19 : tag * 23);
  Packet r{};
  for (unsigned i = 0; i < 64; ++i)
    r.data[i] = p[i] ^ (state + i * 13 + tag);
  r.ptr = ptr;
  r.low = low ^ state;
  r.high = high ^ (state & 1);
  r.byte = uint8_t(p[2]);
  r.a = uint16_t(p[3]);
  r.b = uint16_t(state);
  return r;
}
int main() {
  unsigned checks = 0;
  uint64_t rng = 0x39761afa9293e777ULL;
  for (unsigned test = 0; test < 2048; ++test) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    std::array<uint64_t, 512> mem{};
    for (unsigned i = 0; i < 512; ++i)
      mem[i] = rng ^ (i * 73 + 19);
    auto expected = mem;
    unsigned si = test % 2 ? 13 : 500, oi = 8;
    auto state = mem[si];
    auto *ptr = mem.data() + si;
    std::array<uint64_t, 64> p{};
    for (unsigned i = 0; i < 64; ++i)
      p[i] = rng + i * 17;
    auto a = oracle(state, p, rng, 0, 17, ptr),
         b = oracle(state, a.data, a.low, a.high, 31, ptr);
    for (unsigned i = 0; i < 64; ++i)
      p[i] = a.data[i] ^ b.data[i];
    auto c = oracle(state, p, b.low, b.high, 47, ptr);
    state = (state + c.data[0] + 59) ^ c.data[1];
    for (unsigned i = 0; i < 64; ++i)
      expected[oi + 230 + i] = c.data[i] ^ state;
    state = (state + c.data[0] + 61) ^ c.data[1];
    for (unsigned i = 0; i < 64; ++i)
      expected[oi + 294 + i] = c.data[i] ^ state;
    expected[si] = state;
    unsigned at = oi;
    for (auto *r : {&a, &b, &c}) {
      for (auto v : r->data)
        expected[at++] = v;
      for (auto v : {uint64_t(reinterpret_cast<uintptr_t>(r->ptr)), r->low,
                     r->high, r->byte, r->a, r->b})
        expected[at++] = v;
    }
    evaluate(ptr, rng, mem.data() + oi);
    if (mem != expected) {
      for (unsigned i = 0; i < 512; ++i)
        if (mem[i] != expected[i])
          std::fprintf(stderr, "test%u word%u expected%llx got%llx\n", test, i,
                       (unsigned long long)expected[i],
                       (unsigned long long)mem[i]);
      throw std::runtime_error(
          "shared procedure result/state/order/alias mismatch");
    }
    checks += 512;
  }
  std::printf("PASS %u independent words, nested tuples, packed fields, i65, "
              "pointers, side effects, multiple returns and aliases\n",
              checks);
}
