#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace chipyard_arc;
extern "C" void simutil_memload(const char *);
extern "C" void readmem_enum_file(const char *);
extern "C" int readmem_enum_get(int);
extern "C" void memload_range(const char *, int, int, int);
extern "C" int readmem_get(int, svBitVecVal *);
extern "C" int readmem_set(int, const svBitVecVal *);
using Word = std::array<svBitVecVal, 16>;
struct Memory {
  const char *scope;
  int low;
  unsigned bits;
};
const Memory memories[] = {{"ReadMemHarness/ascending/rtl/rtl/ram", 3, 13},
                           {"ReadMemHarness/descending/rtl/rtl/ram", -2, 65}};
int main(int argc, char **argv) {
  auto directory =
      std::filesystem::absolute(argv[0]).parent_path() / "readmem-inputs";
  std::filesystem::create_directories(directory);
  unsigned checks = 0;
  for (unsigned round = 0; round < 3; round++) {
    auto model = create_model(argc, argv);
    model->initialize();
    for (auto &mem : memories) {
      svSetScope(svGetScopeFromName(mem.scope));
      auto word = [&](uint64_t low, uint32_t high = 0) {
        Word result{};
        result[0] = low;
        result[1] = low >> 32;
        result[2] = high;
        if (mem.bits == 13) {
          result[0] &= 8191;
          result[1] = result[2] = 0;
        } else
          result[2] &= 1;
        return result;
      };
      for (unsigned test = 0; test < 10; test++) {
        std::array<Word, 9> expected;
        for (int n = 0; n < 9; n++) {
          expected[n] = word(n * 17 + 3 + round);
          if (readmem_set(n + mem.low, expected[n].data()) != 1)
            throw std::runtime_error("readmem reset failed");
        }
        std::string contents;
        int mode = 0, start = 0, finish = 0;
        switch (test) {
        case 0:
          contents = "11 /* comment */ 22\n";
          expected[0] = word(0x11);
          expected[1] = word(0x22);
          break;
        case 1:
          contents = "33 44 55\n";
          mode = 2;
          start = mem.low + 4;
          finish = mem.low + 2;
          expected[4] = word(0x33);
          expected[3] = word(0x44);
          expected[2] = word(0x55);
          break;
        case 2:
          contents = "66 77\n";
          mode = 1;
          start = mem.low + 2;
          expected[2] = word(0x66);
          expected[3] = word(0x77);
          break;
        case 3:
          contents = "// preserved holes\n@4\n1_xzF\n";
          expected[4 - mem.low] = word(0x100f);
          break;
        case 4:
          contents = "1_001 10xz\n";
          mode = 3;
          expected[0] = word(9);
          expected[1] = word(8);
          break;
        case 5:
          contents = "77 88 Q\n";
          expected[0] = word(0x77);
          expected[1] = word(0x88);
          break;
        case 6:
          contents = "11 22 33\n";
          mode = 2;
          start = mem.low + 1;
          finish = mem.low + 2;
          expected[1] = word(0x11);
          expected[2] = word(0x22);
          break;
        case 7:
          contents = "123456789abcdef01\n";
          expected[0] = word(0x23456789abcdef01ull, 1);
          break;
        case 8:
          contents = "11 22\n";
          mode = 2;
          start = mem.low - 1;
          finish = mem.low + 2;
          break;
        case 9:
          break;
        }
        auto path = directory / ("case-" + std::to_string(test) + ".txt");
        if (test != 9) {
          std::ofstream file(path);
          file << contents;
        } else
          std::filesystem::remove(path);
        if (test == 0)
          simutil_memload(path.c_str());
        else
          memload_range(path.c_str(), mode, start, finish);
        for (int n = 0; n < 9; n++) {
          Word actual{};
          if (readmem_get(n + mem.low, actual.data()) != 1 ||
              actual != expected[n]) {
            std::fprintf(stderr,
                         "readmem scope=%s test=%u index=%d got=%x:%x:%x "
                         "expected=%x:%x:%x\n",
                         mem.scope, test, n + mem.low, actual[2], actual[1],
                         actual[0], expected[n][2], expected[n][1],
                         expected[n][0]);
            throw std::runtime_error(
                "compiled native readmem changed memory contents");
          }
          checks++;
        }
      }
      auto enum_path = directory / "enum.txt";
      {
        std::ofstream file(enum_path);
        file << "1 2 7 1\n";
      }
      readmem_enum_file(enum_path.c_str());
      for (int n = 0; n < 9; n++) {
        if (readmem_enum_get(mem.low + n) != (n == 0 ? 1 : n == 1 ? 2 : 0))
          throw std::runtime_error("readmem enum validation lost partial "
                                   "writes or admitted an invalid word");
        checks++;
      }
    }
  }
  std::printf(
      "PASS: %u native SV readmem checks across ascending/descending 13/65-bit "
      "memories, nonzero/negative bounds, original-style simutil_memload, "
      "binary/hex, holes, partial failures, wide values, enum validation and "
      "model recreation\n",
      checks);
}
