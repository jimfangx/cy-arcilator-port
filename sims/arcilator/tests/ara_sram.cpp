#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace chipyard_arc;
extern "C" void simutil_memload(const char *);
extern "C" int simutil_get_mem(int, svBitVecVal *);
extern "C" int simutil_set_mem(int, const svBitVecVal *);
using Word = std::array<svBitVecVal, 16>;
struct Memory {
  const char *scope;
  unsigned bits;
};
const Memory memories[] = {{"OriginalSramHarness/narrow/rtl/rtl/mem", 13},
                           {"OriginalSramHarness/wide/rtl/rtl/mem", 65}};
static Word word(uint64_t low, uint32_t high, unsigned bits) {
  Word value{};
  value[0] = low;
  value[1] = low >> 32;
  value[2] = high & 1;
  if (bits == 13) {
    value[0] &= 8191;
    value[1] = value[2] = 0;
  }
  return value;
}
static std::vector<uint8_t> bytes(const Word &value, unsigned bits) {
  std::vector<uint8_t> result((bits + 7) / 8);
  for (unsigned i = 0; i < result.size(); i++)
    result[i] = value[i / 4] >> ((i % 4) * 8);
  if (bits % 8)
    result.back() &= (1u << (bits % 8)) - 1;
  return result;
}
int main(int argc, char **argv) {
  auto file =
      std::filesystem::absolute(argv[0]).parent_path() / "original-sram.hex";
  {
    std::ofstream out(file);
    out << "123456789abcdef01\n@4\n0_abc\n@7\nx_1z5\n";
  }
  unsigned checks = 0;
  for (unsigned round = 0; round < 3; round++) {
    auto model = create_model(argc, argv);
    model->initialize();
    model->advance(0, [&] {
      model->signal("clock").set(0);
      model->signal("reset_n").set(0);
    });
    model->advance(5, [&] { model->signal("clock").set(1); });
    std::array<std::array<Word, 9>, 2> expected{};
    unsigned read_address[2] = {0, 0};
    for (unsigned cycle = 0; cycle < 256; cycle++) {
      unsigned address = cycle * 3 % 9;
      bool write = cycle % 5 == 0;
      uint64_t masks[2] = {uint64_t(cycle % 4), uint64_t(cycle * 37) & 511};
      Word data[2] = {
          word(cycle * 73 + round, 0, 13),
          word(cycle * 0x123456789abcdefull + round, cycle & 1, 65)};
      model->advance(10 + cycle * 10, [&] {
        model->signal("clock").set(0);
        model->signal("reset_n").set(1);
        model->signal("req").set(1);
        model->signal("we").set(write);
        model->signal("address").set(address);
        model->signal("narrow_data").assign(bytes(data[0], 13));
        model->signal("wide_data").assign(bytes(data[1], 65));
        model->signal("narrow_mask").set(masks[0]);
        model->signal("wide_mask").set(masks[1]);
      });
      Word output[2];
      for (unsigned n = 0; n < 2; n++) {
        auto &mem = memories[n];
        svSetScope(svGetScopeFromName(mem.scope));
        if (cycle % 31 == 0) {
          unsigned index = (cycle * 7 + 1) % 9;
          expected[n][index] = word(cycle * 971 + round, cycle & 1, mem.bits);
          if (simutil_set_mem(index, expected[n][index].data()) != 1)
            throw std::runtime_error(
                "original simutil_set_mem rejected a valid address");
        }
        if (cycle % 64 == 0) {
          simutil_memload(file.c_str());
          expected[n][0] = word(0x23456789abcdef01ull, 1, mem.bits);
          expected[n][4] = word(0xabc, 0, mem.bits);
          expected[n][7] = word(0x105, 0, mem.bits);
        }
        for (unsigned index = 0; index < 9; index++) {
          Word actual{};
          if (simutil_get_mem(index, actual.data()) != 1 ||
              actual != expected[n][index])
            throw std::runtime_error("original SRAM export data mismatch");
          checks++;
        }
        if (simutil_set_mem(-1, data[n].data()) ||
            simutil_set_mem(9, data[n].data()))
          throw std::runtime_error("original SRAM invalid write was admitted");
        output[n] = expected[n][write ? read_address[n] : address];
        if (write) {
          auto &value = expected[n][address];
          for (unsigned bit = 0; bit < mem.bits; bit++)
            if ((masks[n] >> (bit / 8)) & 1) {
              uint32_t mask = uint32_t(1) << (bit % 32);
              value[bit / 32] =
                  (value[bit / 32] & ~mask) | (data[n][bit / 32] & mask);
            }
        } else
          read_address[n] = address;
      }
      model->advance(15 + cycle * 10, [&] { model->signal("clock").set(1); });
      for (unsigned n = 0; n < 2; n++) {
        auto &mem = memories[n];
        if (model->signal(n ? "wide" : "narrow").copy() !=
            bytes(output[n], mem.bits))
          throw std::runtime_error(
              "original SRAM hardware read differs from pre-edge memory");
        checks++;
        svSetScope(svGetScopeFromName(mem.scope));
        Word actual{};
        if (simutil_get_mem(address, actual.data()) != 1 ||
            actual != expected[n][address]) {
          std::fprintf(stderr,
                       "original SRAM round=%u cycle=%u width=%u address=%u "
                       "got=%x:%x:%x want=%x:%x:%x\n",
                       round, cycle, mem.bits, address, actual[2], actual[1],
                       actual[0], expected[n][address][2],
                       expected[n][address][1], expected[n][address][0]);
          throw std::runtime_error(
              "original SRAM byte write/C export coherence mismatch");
        }
        checks++;
      }
    }
  }
  std::printf("PASS: %u original Ara SRAM comparisons, all three original "
              "exports, 13/65-bit words, masked RTL writes, read latency, file "
              "holes, invalid indices and model recreation\n",
              checks);
}
