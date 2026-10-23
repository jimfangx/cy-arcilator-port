#include "native/runtime.h"
#include <cstdio>
#include <fstream>

std::vector<uint8_t> parse(const std::string &hex, size_t bytes) {
  std::vector<uint8_t> value(bytes);
  if (hex.size() > bytes * 2) throw std::runtime_error("wide shift fixture overflow");
  for (size_t i = 0; i < hex.size(); ++i) {
    char c = hex[hex.size() - 1 - i];
    unsigned digit = c <= '9' ? c - '0' : c - 'a' + 10;
    value[i / 2] |= digit << ((i % 2) * 4);
  }
  return value;
}

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  if (argc != 2) throw std::runtime_error("missing wide shift reference vectors");
  std::ifstream file(argv[1]);
  if (!file) throw std::runtime_error("cannot read wide shift reference vectors");
  std::string data, amount, left, right, sign;
  unsigned count = 0;
  size_t bytes = runtime->signal("data").bytes();
  while (file >> data >> amount >> left >> right >> sign) {
    runtime->signal("data").assign(parse(data, bytes));
    runtime->signal("amount").assign(parse(amount, bytes));
    // Padding above the declared bit width must never affect either operand.
    runtime->signal("data").data[bytes - 1] |= 0xf8;
    runtime->signal("amount").data[bytes - 1] |= 0xf8;
    runtime->advance(count * 3, [&] {
      runtime->signal("clock").set(0);
      runtime->signal("reset").set(count % 17 == 0);
    });
    if (runtime->signal("left").copy() != parse(left, bytes) ||
        runtime->signal("right").copy() != parse(right, bytes) ||
        runtime->signal("signed").copy() != parse(sign, bytes))
      throw std::runtime_error("native wide shift mismatch at vector " + std::to_string(count));
    auto expected = count % 17 == 0 ? std::vector<uint8_t>(bytes) : parse(left, bytes);
    runtime->advance(count * 3 + 1, [&] { runtime->signal("clock").set(1); });
    if (runtime->signal("latched").copy() != expected)
      throw std::runtime_error("clocked native wide shift result mismatch");
    runtime->advance(count * 3 + 2, [&] { runtime->signal("data").data[0] ^= 1; });
    if (runtime->signal("latched").copy() != expected)
      throw std::runtime_error("native wide shift sampled after its clock edge");
    ++count;
  }
  if (!count) throw std::runtime_error("empty wide shift reference");
  std::printf("PASS: %u independent wide shift vectors, padding, reset, and clock-edge sampling\n", count);
}
