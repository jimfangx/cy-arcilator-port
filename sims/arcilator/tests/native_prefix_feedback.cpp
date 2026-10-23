#include "native/runtime.h"
#include <algorithm>
#include <cstdio>

using namespace chipyard_arc;
using Bytes = std::vector<uint8_t>;
static Bytes reference(const Bytes &a, const Bytes &b, unsigned width,
                       unsigned step, unsigned operation, bool reverse) {
  Bytes out((width + 7) / 8);
  auto bit = [](const Bytes &data, unsigned i) {
    return (data[i / 8] >> (i % 8)) & 1;
  };
  for (unsigned index = 0; index < width; ++index) {
    unsigned i = reverse ? width - 1 - index : index;
    int prior = reverse ? int(i + step) : int(i) - int(step);
    bool value = prior >= 0 && unsigned(prior) < width ? bit(out, prior)
                                                       : operation == 1;
    if (operation == 0)
      value ^= bit(a, i) ^ bit(b, i);
    else if (operation == 1)
      value &= bit(a, i) & bit(b, i);
    else
      value |= bit(a, i) | bit(b, i);
    out[i / 8] |= uint8_t(value) << (i % 8);
  }
  return out;
}
int main(int argc, char **argv) {
  auto model = create_model(argc, argv);
  model->initialize();
  Bytes expected_q(17), data(17), other(17);
  uint64_t random = 1979, time = 0;
  bool old_clock = false;
  unsigned checks = 0;
  auto evaluate = [&](bool clock, bool reset) {
    auto next = reference(data, other, 129, 1, 2, false);
    if (clock && !old_clock)
      expected_q = reset ? Bytes(17) : next;
    model->advance(++time, [&] {
      model->signal("clock").set(clock);
      model->signal("reset").set(reset);
      model->signal("data").assign(data);
      model->signal("other").assign(other);
    });
    for (unsigned width : {1, 4, 5, 9, 65, 129}) {
      for (unsigned operation = 0; operation < 3; ++operation) {
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
          for (unsigned step : {1U, std::min(3U, width)}) {
            if (width == 1 && step != 1)
              continue;
            auto name = "prefix_" + std::to_string(width) + "_" +
                        std::to_string(operation) + "_" +
                        std::to_string(reverse) + "_" + std::to_string(step);
            if (model->signal(name).copy() !=
                reference(data, other, width, step, operation, reverse))
              throw std::runtime_error(
                  "native shifted prefix differs from bit recurrence: " + name);
            ++checks;
            if (width == 1)
              break;
          }
        }
      }
    }
    if (model->signal("q").copy() != expected_q)
      throw std::runtime_error(
          "shifted-prefix register sampled at the wrong edge");
    old_clock = clock;
  };
  for (unsigned cycle = 0; cycle < 256; ++cycle) {
    for (unsigned i = 0; i < data.size(); ++i) {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      data[i] = cycle < 2 ? (cycle ? 255 : 0) : uint8_t(random);
      other[i] = uint8_t(random >> 8);
    }
    data.back() &= 1;
    other.back() &= 1;
    bool reset = cycle % 17 == 0;
    evaluate(false, reset);
    evaluate(true, reset);
    for (auto &byte : data)
      byte ^= 255;
    data.back() &= 1;
    evaluate(true, !reset);
    evaluate(true, !reset);
  }
  std::printf(
      "PASS: %u native shifted-prefix checks, 66 recurrences, 1..129 bits, "
      "OR/AND/XOR, both directions, unit/nonunit strides, reset and "
      "clock sampling\n",
      checks);
}
