#include "native/runtime.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <fstream>
#include <iterator>

extern "C" uint8_t *chipyard_arc_sv_stream(uint32_t);
extern "C" uint8_t *__real_chipyard_arc_sv_stream(uint32_t);
extern "C" uint32_t __real_chipyard_arc_sv_fopen(const uint8_t *, uint64_t,
                                                 int32_t);
static unsigned opens = 0, streams = 0;
extern "C" uint32_t __wrap_chipyard_arc_sv_fopen(const uint8_t *path,
                                                 uint64_t size, int32_t mode) {
  auto result = __real_chipyard_arc_sv_fopen(path, size, mode);
  ++opens;
  return result;
}
extern "C" uint8_t *__wrap_chipyard_arc_sv_stream(uint32_t channel) {
  auto result = __real_chipyard_arc_sv_stream(channel);
  ++streams;
  return result;
}

int main(int argc, char **argv) {
  if (chipyard_arc_sv_stream(0x80000001u) !=
          reinterpret_cast<uint8_t *>(&llvm::outs()) ||
      chipyard_arc_sv_stream(0x80000002u) !=
          reinterpret_cast<uint8_t *>(&llvm::errs()))
    throw std::runtime_error("native SV standard descriptor mapping changed");
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  model->advance(0);
  if (model->signal("status").get() != 15)
    throw std::runtime_error(
        "native file open success/failure status mismatch");
  if (opens != 5)
    throw std::runtime_error(
        "native startup file operations did not execute once");
  unsigned expected_opens = 5, expected_streams = 5;
  std::string expected_a = "CONSTANT |    x|é   |\n", expected_m;
  const uint8_t alphabet[] = {0, 'x', 'M', 0xc3, 0xa9, 0xff, 'z', 0};
  for (unsigned cycle = 0; cycle < 128; ++cycle) {
    uint32_t data = cycle * 12345679u + 17;
    bool reset = cycle < 2, enable = cycle % 3 != 0;
    std::vector<uint8_t> bytes(17);
    if (cycle >= 4) {
      for (unsigned i = 0; i < 16; ++i)
        bytes[i] = alphabet[(cycle + i * 3) % 8];
      bytes[16] = cycle & 1;
    }
    std::string text;
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
      if (*it)
        text.push_back(char(*it));
    std::string padding(20 > text.size() ? 20 - text.size() : 0, ' ');
    if (!reset && enable) {
      expected_streams += 2;
      expected_opens += data & 1;
      char prefix[40];
      std::snprintf(prefix, sizeof(prefix), "F %08x |", data);
      expected_a += std::string(prefix) + text + "|" + text + padding + "|" +
                    padding + text + "|\n";
      expected_m += "M " + std::to_string(data) + " |" + text + "|\n";
    }
    model->advance(cycle * 1000 + 1, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("data").set(data);
      model->signal("text").assign(bytes);
    });
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
  }
  model.reset();
  if (opens != expected_opens || streams != expected_streams)
    throw std::runtime_error("native file operation activation counts changed");
  expected_a += "FINAL A\n";
  expected_m += "FINAL M\n";
  auto read = [](const char *path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
      throw std::runtime_error(std::string("missing native file: ") + path);
    return std::string(std::istreambuf_iterator<char>(stream), {});
  };
  if (read("native-file-a.log") != expected_a ||
      read("native-file-b.log") != expected_m ||
      read("native-file-c.log") != expected_m ||
      !read("native-file-empty.log").empty())
    throw std::runtime_error("native file bytes, stream fanout, formatting or "
                             "finalization mismatch");
  std::puts(
      "PASS: 128 native file cycles, FD and MCD channels, shared fanout, "
      "append/reopen, failed and unused opens, flush, finalization, literal "
      "and 129-bit packed strings, zero-byte filtering, UTF-8 and padding");
}
