#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <stdexcept>
using namespace chipyard_arc;
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  unsigned ca = 341, cb = 341, sa = 77, sb = 77, held = 291;
  std::array<unsigned, 8> expected{};
  auto check = [&](unsigned cycle, const char *phase) {
    const char *names[] = {"a0", "a1", "b0", "b1", "as0", "as1", "bs0", "bs1"};
    for (unsigned i = 0; i < 8; i++) {
      auto value = model->signal(names[i]).get();
      if (value != expected[i]) {
        std::fprintf(stderr, "cycle %u %s %s: got %llu expected %u\n", cycle,
                     phase, names[i], (unsigned long long)value, expected[i]);
        throw std::runtime_error("static function storage changed");
      }
    }
    if (model->signal("latched").get() != held)
      throw std::runtime_error("static block storage changed");
  };
  for (unsigned cycle = 0; cycle < 4096; cycle++) {
    bool reset = cycle % 67 == 0, ea = cycle % 3 != 0, eb = cycle % 5 != 0,
         le = cycle % 7 < 3;
    unsigned da = (cycle * 71 + 19) & 8191, db = (cycle * 157 + 31) & 8191,
             ld = (cycle * 337 + 29) & 8191;
    held = reset ? 291 : le ? ld : held;
    model->advance(cycle * 10 + 1, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("enable_a").set(ea);
      model->signal("enable_b").set(eb);
      model->signal("latch_enable").set(le);
      model->signal("data_a").set(da);
      model->signal("data_b").set(db);
      model->signal("latch_data").set(ld);
    });
    check(cycle, "falling");
    if (model->signal("fresh_seen").get() != (reset ? 0 : le ? ld : 0))
      throw std::runtime_error("automatic local lifetime changed");
    auto step = [](unsigned &state, bool reset, bool en, unsigned amount,
                   unsigned init) {
      state = reset ? init : en ? (state + amount) & 8191 : state;
      return state;
    };
    expected[0] = step(ca, reset, ea, da, 341);
    expected[1] = step(ca, reset, ea, da, 341);
    expected[2] = step(cb, reset, eb, db, 341);
    expected[3] = step(cb, reset, eb, db, 341);
    expected[4] = step(sa, reset, ea, (da + 1) & 8191, 77);
    expected[5] = step(sa, reset, ea, (da + 3) & 8191, 77);
    expected[6] = step(sb, reset, eb, (db + 1) & 8191, 77);
    expected[7] = step(sb, reset, eb, (db + 3) & 8191, 77);
    model->advance(cycle * 10 + 5, [&] { model->signal("clock").set(1); });
    check(cycle, "rising");
  }
  std::puts("PASS: 4096 cycles, shared static function locals, per-instance "
            "state, block statics, automatic locals");
}
