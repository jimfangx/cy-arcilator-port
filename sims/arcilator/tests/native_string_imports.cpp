#include "native/runtime.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace chipyard_arc;
static char returned_buffer[80], produced_buffer[80], history_buffer[80];
static std::string expected_history = "seed";
static unsigned calls = 0, clobbers = 0;
static std::string prefix_for(unsigned key) {
  switch (key % 4) {
  case 0:
    return "";
  case 1:
    return "alpha";
  case 2:
    return "Grüße";
  default:
    return "Z";
  }
}
extern "C" const char *native_string_identity(const char *text) { return text; }
extern "C" int native_string_size(const char *text) {
  return int(std::strlen(text));
}

extern "C" const char *native_string_mix(const char *prefix, uint32_t key,
                                         const char **produced,
                                         const char **history) {
  if (!prefix || !history || !*history || expected_history != *history ||
      std::string(prefix) != prefix_for(key))
    throw std::runtime_error("DPI string input/inout copy-in mismatch");
  ++calls;
  std::snprintf(returned_buffer, sizeof(returned_buffer), "%s:R%u", prefix,
                key);
  std::snprintf(produced_buffer, sizeof(produced_buffer), "O%u", key);
  std::snprintf(history_buffer, sizeof(history_buffer), "H%u", key);
  *produced = key % 7 ? produced_buffer : nullptr;
  *history = history_buffer;
  expected_history = history_buffer;
  return key % 5 ? returned_buffer : nullptr;
}
extern "C" void native_string_clobber() {
  ++clobbers;
  std::memset(returned_buffer, 'x', sizeof(returned_buffer) - 1);
  std::memset(produced_buffer, 'y', sizeof(produced_buffer) - 1);
  std::memset(history_buffer, 'z', sizeof(history_buffer) - 1);
}
int main(int argc, char **argv) {
  for (unsigned instance = 0; instance < 2; ++instance) {
    calls = clobbers = 0;
    expected_history = "seed";
    auto m = create_model(argc, argv);
    m->initialize();
    uint32_t lengths = 0, firsts = 0, old_length = 0;
    std::string saved;
    unsigned expected_calls = 0;
    for (unsigned cycle = 0; cycle < 256; ++cycle) {
      bool reset = cycle % 31 == 0, enable = cycle % 6 != 0;
      m->advance(cycle * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
        m->signal("key").set(cycle);
      });
      if (reset) {
        expected_history = "seed";
        saved.clear();
        lengths = firsts = old_length = 0;
      } else if (enable) {
        ++expected_calls;
        std::string prefix = prefix_for(cycle);
        std::string result =
            cycle % 5 ? prefix + ":R" + std::to_string(cycle) : "";
        std::string output = cycle % 7 ? "O" + std::to_string(cycle) : "";
        std::string history = "H" + std::to_string(cycle);
        old_length = saved.size();
        saved = result;
        lengths = result.size() | (output.size() << 8) | (history.size() << 16);
        firsts = (result.empty() ? 0 : uint8_t(result[0])) |
                 ((output.empty() ? 0 : uint8_t(output[0])) << 8) |
                 (uint8_t(history[0]) << 16);
      }
      m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
      if (m->signal("lengths").get() != lengths ||
          m->signal("firsts").get() != firsts ||
          m->signal("old_length").get() != old_length ||
          calls != expected_calls || clobbers != calls)
        throw std::runtime_error("DPI string return/output ownership or "
                                 "clock/reset gating mismatch");
    }
  }
  std::puts(
      "PASS: 1536 DPI string observations, input/output/inout/return ABI, "
      "empty/null strings, mutable C buffers, retained prior values and "
      "exact activation counts across model recreation");
}
