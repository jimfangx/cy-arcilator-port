#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  std::vector<uint8_t> q(16);
  std::array<std::vector<uint8_t>, 2> sampled{q, q};
  unsigned select = 0;
  uint64_t data = 0, rng = 0x56f47b519ace321dULL;
  auto random = [&] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  auto expected = [&](unsigned xlen) {
    auto result = q;
    unsigned begin = 0, count = 0;
    switch (select) {
    case 0:
      count = 8;
      break;
    case 1:
      begin = 4;
      count = xlen == 32 ? 4 : 0;
      break;
    case 2:
      begin = 8;
      count = 8;
      break;
    case 3:
      begin = 12;
      count = xlen == 32 ? 4 : 0;
      break;
    }
    for (unsigned i = 0; i < count; ++i)
      if (!(q[begin + i] & 128))
        result[begin + i] = uint8_t(data >> (i * 8));
    return result;
  };
  auto check = [&] {
    for (unsigned i = 0; i < 2; ++i) {
      const char *suffix = i ? "64" : "32";
      if (model->signal(std::string("d") + suffix).copy() !=
              expected(i ? 64 : 32) ||
          model->signal(std::string("sampled") + suffix).copy() != sampled[i])
        throw std::runtime_error(
            "native CFG loop result or pre-edge sampling mismatch");
    }
  };
  auto drive = [&] {
    for (auto &byte : q)
      byte = uint8_t(random());
    data = random();
    select = unsigned(random() & 7);
    model->signal("q").assign(q);
    model->signal("data").set(data);
    model->signal("select").set(select);
  };
  model->advance(0);
  check();
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    model->advance(cycle * 1000 + 1, [&] {
      model->signal("clock").set(0);
      drive();
    });
    check();
    sampled = {expected(32), expected(64)};
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check();
    model->advance(cycle * 1000 + 750, drive);
    check();
  }
  std::puts(
      "PASS: 1024 cycles of RV32/RV64 constant-disabled CFG loops, locked "
      "lanes, dynamic writes, all select branches and pre-edge sampling");
}
