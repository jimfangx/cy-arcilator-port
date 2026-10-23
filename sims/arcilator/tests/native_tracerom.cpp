#include "native/runtime.h"
#include <cstdio>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    auto model = chipyard_arc::create_model(argc, argv);
    auto clock = model->signal("clock"), reset = model->signal("reset"), stall = model->signal("stall");
    clock.set(0); reset.set(1); stall.set(1); model->initialize(); model->settle();
    model->advance(1000, [&] { clock.set(1); });
    model->advance(1500, [&] { clock.set(0); reset.set(0); });
    const uint64_t words[] = {0xfedcba9876543210ULL, 0x0123456789abcdefULL, 0x8000000000001357ULL};
    const uint8_t masks[] = {255, 255, 3};
    uint64_t checksum = 0;
    unsigned received = 0;
    std::vector<uint8_t> sample(10, 0);
    for (unsigned cycle = 0; cycle < 25; ++cycle) {
      uint64_t time = 3000 + cycle * 2000;
      model->advance(time - 500, [&] { stall.set(cycle % 3 == 0); });
      if (model->signal("ready").get()) {
        if (received >= 3 || !model->signal("valid").get() || model->signal("data").get() != words[received] ||
            model->signal("keep").get() != masks[received] || model->signal("last").get() != (received == 2))
          throw std::runtime_error("native TraceROM DPI payload or backpressure mismatch");
        std::memcpy(sample.data(), &words[received], 8);
        sample[8] = masks[received]; sample[9] = received == 2;
        checksum ^= words[received++];
      }
      model->advance(time, [&] { clock.set(1); });
      if (model->signal("count").get() != received || model->signal("sample").copy() != sample ||
          model->signal("checksum").get() != checksum || model->signal("length").get() != 3 ||
          model->signal("mac").get() != 0x102030405060ULL)
        throw std::runtime_error("native TraceROM consumer state or initialization mismatch");
      model->advance(time + 500, [&] { clock.set(0); });
    }
    if (received != 3 || model->signal("valid").get()) throw std::runtime_error("native TraceROM did not drain three beats");
    std::puts("PASS: Verilator-free original TraceROM DPI stream, masks, last and native backpressure");
    return 0;
  } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
