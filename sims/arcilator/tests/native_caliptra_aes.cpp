#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
static std::vector<uint8_t> bytes(const std::string &hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < hex.size(); i += 2)
    out.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  return out;
}
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("expected AES vector file");
    auto model = chipyard_arc::create_model(argc, argv);
    uint64_t time = 0;
    model->signal("clock").set(0);
    model->signal("reset").set(1);
    model->signal("out_ready").set(1);
    model->initialize();
    model->advance(time);
    auto low = [&] {
      model->advance(++time, [&] { model->signal("clock").set(0); });
    };
    auto high = [&] {
      model->advance(++time, [&] { model->signal("clock").set(1); });
    };
    auto tick = [&] {
      low();
      high();
    };
    std::ifstream stream(argv[1]);
    std::string key, input, expected;
    unsigned mode, checks = 0, resetCount = 0;
    while (stream >> mode >> key >> input >> expected) {
      if (mode == 2) {
        low();
        model->advance(++time, [&] {
          model->signal("reset").set(1);
          model->signal("in_valid").set(0);
          model->signal("out_ready").set(1);
        });
        for (unsigned i = 0; i < 4; ++i)
          tick();
        low();
        model->advance(++time, [&] { model->signal("reset").set(0); });
        for (unsigned i = 0; i < 4; ++i)
          tick();
        ++resetCount;
        continue;
      }
      low();
      model->advance(++time, [&] {
        model->signal("in_valid").set(1);
        model->signal("encrypt").set(mode);
        model->signal("key").assign(bytes(key));
        model->signal("data").assign(bytes(input));
        model->signal("out_ready").set(0);
      });
      unsigned waited = 0;
      while (!model->signal("in_ready").get()) {
        high();
        low();
        if (++waited > 1000)
          throw std::runtime_error("AES input handshake timed out");
      }
      high();
      low();
      model->advance(++time, [&] { model->signal("in_valid").set(0); });
      waited = 0;
      while (!model->signal("out_valid").get()) {
        high();
        low();
        if (++waited > 1000)
          throw std::runtime_error("AES output handshake timed out");
      }
      auto result = model->signal("result").copy();
      if (result != bytes(expected)) {
        std::fprintf(stderr, "AES %u mode %u mismatch input %s key %s\n",
                     checks, mode, input.c_str(), key.c_str());
        for (auto byte : result)
          std::fprintf(stderr, "%02x", byte);
        std::fprintf(stderr, " actual\n%s expected\n", expected.c_str());
        return 1;
      }
      for (unsigned i = 0; i < 3 + checks % 5; ++i) {
        tick();
        if (!model->signal("out_valid").get() ||
            model->signal("result").copy() != result)
          throw std::runtime_error("AES output changed under backpressure");
      }
      low();
      model->advance(++time, [&] { model->signal("out_ready").set(1); });
      high();
      tick();
      ++checks;
    }
    if (checks != 28 || resetCount != 4)
      throw std::runtime_error("AES vector selection incomplete");
    std::printf(
        "PASS: %u original Caliptra AES-256 ECB encrypt/decrypt results match "
        "OpenSSL; %u resets, key changes, mode changes and output "
        "backpressure\n",
        checks, resetCount);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
