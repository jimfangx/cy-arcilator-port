#include "native/runtime.h"
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  std::vector<uint8_t> wide(17), set(17), clear(17), wire(17), wireSample(17);
  uint64_t low = UINT64_C(0x0123456789abcdef),
           high = UINT64_C(0xfedcba9876543210);
  std::memcpy(wide.data(), &low, 8);
  std::memcpy(wide.data() + 8, &high, 8);
  wide[16] = 1;
  unsigned q = 1, sampled = 0;
  uint64_t time = 0;
  auto check = [&] {
    if (model->signal("q").get() != q || model->signal("wide").copy() != wide ||
        model->signal("sampled").get() != sampled ||
        model->signal("unstable").get() != 0 ||
        model->signal("wire_q").copy() != wire ||
        model->signal("wire_sample").copy() != wireSample ||
        model->signal("wire_unstable").get() != 0)
      throw std::runtime_error(
          "native feedback initialization/settling mismatch");
  };
  model->advance(time++);
  check();
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    unsigned setBit = cycle % 5 == 0, clearBit = cycle % 7 == 0;
    // Interleave updates with hold cycles; the wide feedback must retain all
    // 129 initializer/state bits rather than reseeding each evaluation.
    for (unsigned i = 0; i < wide.size(); ++i) {
      set[i] = cycle % 3 ? uint8_t(cycle * 37 + i * 17) : 0;
      clear[i] = cycle % 3 ? uint8_t(cycle * 71 + i * 13) : 0;
      if (i == 16) {
        set[i] &= 1;
        clear[i] &= 1;
      }
      wide[i] = (wide[i] | set[i]) & ~clear[i];
      wire[i] = (wire[i] | set[i]) & ~clear[i];
    }
    q = (q | setBit) & !clearBit;
    model->advance(time++, [&] {
      model->signal("clock").set(0);
      model->signal("set_i").set(setBit);
      model->signal("clear_i").set(clearBit);
      model->signal("set_bits").assign(set);
      model->signal("clear_bits").assign(clear);
    });
    check();
    sampled = q;
    wireSample = wire;
    model->advance(time++, [&] { model->signal("clock").set(1); });
    check();
    model->advance(time++);
    check();
  }
  bool rejected = false;
  try {
    model->advance(time++, [&] { model->signal("oscillate").set(1); });
  } catch (const std::runtime_error &error) {
    rejected =
        std::string(error.what()).find("did not converge") != std::string::npos;
  }
  if (!rejected)
    throw std::runtime_error("native feedback oscillator was not rejected");
  std::puts(
      "PASS: 512 native feedback cycles, explicit 1/129-bit initializers, "
      "hold/set/clear, stable reevaluation, peer sampling and oscillator "
      "rejection");
}
