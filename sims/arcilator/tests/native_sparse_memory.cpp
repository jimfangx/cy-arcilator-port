#include "native/runtime.h"
#include <array>
#include <cstdio>
using namespace chipyard_arc;
using Word = std::array<uint8_t, 9>;
static Word data(unsigned cycle, unsigned salt) {
  Word w{};
  for (unsigned i = 0; i < 9; i++)
    w[i] = uint8_t(cycle * 73 + i * 37 + salt);
  w[8] &= 1;
  return w;
}
static void write(Word &dst, const Word &src, unsigned mask) {
  for (unsigned lane = 0; lane < 5; lane++)
    if (mask & (1u << lane))
      for (unsigned bit = lane * 13; bit < (lane + 1) * 13; bit++) {
        unsigned m = 1u << (bit % 8);
        dst[bit / 8] = (dst[bit / 8] & ~m) | (src[bit / 8] & m);
      }
}
static void huge_memory(Runtime &runtime) {
  constexpr uint64_t depth = (uint64_t(1) << 40) + 7;
  uint8_t clock = 0, enabled = 1, mask = 31;
  uint64_t address = 0;
  Word input{}, output{};
  auto memory = make_binding(runtime, "ArcSparseMemory");
  memory->parameters = {{"DEPTH", std::to_string(depth)},
                        {"BITS", "65"},
                        {"MASKS", "5"},
                        {"READS", "1"},
                        {"WRITES", "1"},
                        {"READ_LATENCY", "0"},
                        {"READ_NEW", "0"}};
  memory->ports = {{"r0_clock", {&clock, 1}},
                   {"r0_address", {reinterpret_cast<uint8_t *>(&address), 41}},
                   {"r0_enable", {&enabled, 1}},
                   {"r0_data", {output.data(), 65}},
                   {"w0_clock", {&clock, 1}},
                   {"w0_address", {reinterpret_cast<uint8_t *>(&address), 41}},
                   {"w0_enable", {&enabled, 1}},
                   {"w0_data", {input.data(), 65}},
                   {"w0_mask", {&mask, 5}}};
  memory->initialize();
  for (auto index : {(uint64_t(1) << 35) + 3, depth - 1}) {
    address = index;
    memory->combinational();
    if (output != Word{})
      throw std::runtime_error("unwritten large address is nonzero");
    clock = 0;
    memory->sample();
    memory->commit();
    input = data(unsigned(index), 123);
    clock = 1;
    memory->sample();
    memory->commit();
    memory->combinational();
    if (output != input)
      throw std::runtime_error("large memory address was truncated");
    address = uint32_t(index);
    memory->combinational();
    if (output != Word{})
      throw std::runtime_error("large address aliases its low 32 bits");
    address = index;
    memory->combinational();
    if (output != input)
      throw std::runtime_error("large address lost its stored value");
  }
  address = depth + 1;
  clock = 0;
  memory->sample();
  memory->commit();
  clock = 1;
  memory->sample();
  if (memory->commit())
    throw std::runtime_error("invalid write changed sparse memory");
  memory->combinational();
  if (output != Word{})
    throw std::runtime_error("invalid read is not native zero");
  std::puts("PASS: native sparse capacity above 1 trillion words, 41-bit "
            "addresses and bounds");
}

int main(int argc, char **argv) {
  auto m = create_model(argc, argv);
  m->initialize();
  if (argc < 2 || std::string(argv[1]) != "--skip-huge-native")
    huge_memory(*m);
  unsigned checks = 0;
  std::array<Word, 9> expected{};
  std::array<unsigned, 9> small{};
  Word old{}, fresh{};
  unsigned rw = 0;
  bool q_valid = false, rw_valid = false;
  bool old_clock = false, old_peer = false;
  for (unsigned step = 0; step < 2048; step++) {
    bool clock = step % 4 >= 2, peer = step % 6 >= 3,
         rise = clock && !old_clock, peer_rise = peer && !old_peer;
    old_clock = clock;
    old_peer = peer;
    unsigned address = (step * 7) % 16, wa0 = (step * 3) % 16,
             wa1 = step % 3 ? ((step * 5) % 16) : wa0;
    bool en0 = step % 5 != 0, en1 = step % 7 != 0, ren = step % 11 != 0,
         mode = (step / 3) % 2, rwen = step % 13 != 0;
    unsigned mask0 = (step * 7) & 31, mask1 = (step * 13) & 31;
    auto d0 = data(step, 5), d1 = data(step, 89);
    if (rise && ren) {
      old = address < 9 ? expected[address] : Word{};
      q_valid = true;
    } else if (rise)
      q_valid = false;
    if (rise) {
      if (en0 && wa0 < 9)
        write(expected[wa0], d0, mask0);
      if (en1 && wa1 < 9)
        write(expected[wa1], d1, mask1);
      if (ren)
        fresh = address < 9 ? expected[address] : Word{};
    }
    if (peer_rise && rwen) {
      if (mode) {
        if (wa0 < 9)
          small[wa0] = unsigned(d0[0]) | (unsigned(d0[1] & 31) << 8);
        rw_valid = false;
      } else {
        rw = wa0 < 9 ? small[wa0] : 0;
        rw_valid = true;
      }
    } else if (peer_rise)
      rw_valid = false;
    auto b0 = std::vector<uint8_t>(d0.begin(), d0.end()),
         b1 = std::vector<uint8_t>(d1.begin(), d1.end());
    m->advance(step, [&] {
      m->signal("clock").set(clock);
      m->signal("peer").set(peer);
      m->signal("we0").set(en0);
      m->signal("we1").set(en1);
      m->signal("re").set(ren);
      m->signal("ra").set(address);
      m->signal("wa0").set(wa0);
      m->signal("wa1").set(wa1);
      m->signal("data0").assign(b0);
      m->signal("data1").assign(b1);
      m->signal("mask0").set(mask0);
      m->signal("mask1").set(mask1);
      m->signal("mode").set(mode);
      m->signal("rw_enable").set(rwen);
    });
    auto check = [&](const char *name, const Word &w) {
      auto got = m->signal(name).copy();
      if (got != std::vector<uint8_t>(w.begin(), w.end())) {
        std::fprintf(stderr, "step=%u output=%s ra=%u w0=%u w1=%u rise=%u\n",
                     step, name, address, wa0, wa1, rise);
        for (unsigned i = 0; i < 9; i++)
          std::fprintf(stderr, "%u:%02x/%02x ", i, got[i], w[i]);
        std::fputc('\n', stderr);
        throw std::runtime_error("FIRRTL memory data mismatch");
      }
      ++checks;
    };
    if (q_valid && ren) {
      check("old_data", old);
      check("new_data", fresh);
    }
    if (ren)
      check("async_data", address < 9 ? expected[address] : Word{});
    if (rw_valid && rwen && !mode) {
      if (m->signal("rw_data").get() != rw)
        throw std::runtime_error("read-write port mismatch");
      ++checks;
    }
  }
  std::printf("PASS: %u native memory comparisons\n", checks);
}
