#include "native/runtime.h"
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t counter = 0, sampled = 0, low = 0, selector = 0, payload = 0;
  constexpr uint64_t mask = (UINT64_C(1) << 44) - 1;
  const std::vector<uint8_t> seed{0xef, 0xcd, 0xab, 0x89, 0x67, 0x45,
                                  0x23, 0x01, 0x10, 0x32, 0x54, 0x76,
                                  0x98, 0xba, 0xdc, 0xfe, 0x01};
  auto check = [&] {
    uint64_t a = 0, b = 0;
    switch (selector) {
    case 0:
      a = payload;
      b = payload ^ 0x12345;
      break;
    case 1:
      a = payload + 0x1abcd;
      b = ~payload;
      break;
    case 2:
      a = payload ^ 0x15555;
      b = payload + 0x11111;
      break;
    case 3:
      a = ~payload;
      b = payload ^ 0x1aaaa;
      break;
    }
    constexpr uint64_t field = (1u << 17) - 1;
    auto merged =
        ((a & field) << 34) | ((b & field) << 17) | ((payload + 0x123) & field);
    if (model->signal("counter").get() != counter ||
        model->signal("sampled").get() != sampled ||
        model->signal("floating").copy() != std::vector<uint8_t>(33) ||
        model->signal("bundle").copy() != std::vector<uint8_t>(8) ||
        model->signal("seeded").copy() != seed ||
        model->signal("partial").get() != low ||
        model->signal("merged").get() != merged)
      throw std::runtime_error("native undriven value or equivalent-register "
                               "coherence mismatch");
  };
  model->advance(0);
  check();
  for (uint64_t cycle = 0; cycle < 64; ++cycle) {
    auto step = (UINT64_C(1) << 42) + cycle * 0x1234567 + 7;
    bool reset = cycle == 0 || cycle == 17 || cycle == 49;
    low = uint8_t(cycle * 41 + 3);
    selector = cycle & 3;
    payload = (cycle * 0x12345 + 0x15555) & ((1u << 17) - 1);
    model->advance(cycle * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("step").set(step);
      model->signal("low").set(low);
      model->signal("selector").set(selector);
      model->signal("payload").set(payload);
    });
    check();
    sampled = counter;
    counter = reset ? 0 : (counter + step) & mask;
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check();
    low ^= 255;
    selector ^= 3;
    payload ^= (1u << 17) - 1;
    model->advance(cycle * 1000 + 750, [&] {
      model->signal("step").set(mask ^ step);
      model->signal("low").set(low);
      model->signal("selector").set(selector);
      model->signal("payload").set(payload);
    });
    check();
  }
  std::puts(
      "PASS: equivalent register drives, 44-bit counter feedback, pre-edge "
      "sampling, reset, undriven 257-bit/aggregate/partial nets and nonzero "
      "129-bit constant initialization; complete branch-merged struct "
      "fields without false old-value dependencies");
}
