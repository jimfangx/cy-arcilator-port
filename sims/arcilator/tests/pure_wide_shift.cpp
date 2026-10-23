#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "pure_wide_layout.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" void ArcModel_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_ArcModel;
static std::vector<uint8_t> parse(const std::string &hex, unsigned bytes) {
  std::vector<uint8_t> result(bytes);
  if (hex.size() > bytes * 2)
    throw std::runtime_error("reference overflow");
  for (size_t i = 0; i < hex.size(); ++i) {
    char c = hex[hex.size() - 1 - i];
    result[i / 2] |= (c <= '9' ? c - '0' : c - 'a' + 10) << ((i % 2) * 4);
  }
  return result;
}
int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("missing reference");
  ArcState *states[2];
  for (auto &state : states) {
    state = arcRuntimeAllocateInstance(&arcRuntimeModel_ArcModel, nullptr);
    initialize_model(state->modelState);
    arcRuntimeOnInitialized(state);
  }
  auto evaluate = [](ArcState *state) {
    arcRuntimeOnEval(state);
    ArcModel_eval(state->modelState);
  };
  std::ifstream file(argv[1]);
  unsigned index, count = 0, checks = 0;
  std::string data, amount, left, right, sign, chain;
  while (file >> index >> data >> amount >> left >> right >> sign >> chain) {
    const auto &port = ports[index];
    auto *state = states[count % 2];
    auto *storage = state->modelState;
    auto put = [&](unsigned offset, const std::string &hex) {
      auto bytes = parse(hex, port.bytes);
      std::memcpy(storage + offset, bytes.data(), bytes.size());
      if (port.bits % 8)
        storage[offset + port.bytes - 1] |= uint8_t(255u << (port.bits % 8));
    };
    auto check = [&](unsigned offset, const std::string &hex) {
      auto expected = parse(hex, port.bytes);
      auto actual =
          std::vector<uint8_t>(storage + offset, storage + offset + port.bytes);
      if (port.bits % 8)
        actual.back() &= (1u << (port.bits % 8)) - 1;
      if (actual != expected)
        throw std::runtime_error(
            "pure-wide value or sampling mismatch at vector " +
            std::to_string(count));
      ++checks;
    };
    put(port.data, data);
    put(port.amount, amount);
    storage[clockOffset] = 0;
    storage[resetOffset] = count % 17 == 0;
    evaluate(state);
    check(port.left, left);
    check(port.right, right);
    check(port.sign, sign);
    check(port.chain, chain);
    evaluate(state);
    check(port.left, left);
    check(port.chain, chain);
    storage[clockOffset] = 1;
    evaluate(state);
    const std::string expected = count % 17 == 0 ? "0" : left;
    check(port.latched, expected);
    storage[port.data] ^= 1;
    evaluate(state);
    check(port.latched, expected);
    ++count;
  }
  if (count != 792)
    throw std::runtime_error("incomplete independent reference");
  for (auto *state : states)
    arcRuntimeDeleteInstance(state);
  std::printf("PASS: %u pure-wide vectors, %u all-output/stable/sampling "
              "checks, six widths, dirty padding and two model instances\n",
              count, checks);
}
