#include "native/runtime.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
using namespace chipyard_arc;
extern "C" int multi_set(int, int, int, const svBitVecVal *);
extern "C" int multi_get(int, int, int, svBitVecVal *);
extern "C" void multi_load(const char *, int, int, int);
using Word = std::array<svBitVecVal, 16>;
struct Memory {
  const char *instance;
  unsigned bits, middle, inner;
  int low0, low1, low2;
};
static const Memory memories[] = {{"a", 13, 4, 1, -1, 5, 0},
                                  {"b", 65, 4, 1, 2, -3, 0},
                                  {"c", 9, 2, 4, -2, 3, -4},
                                  {"d", 129, 2, 4, 4, -3, 2}};

Word clipped(Word value, unsigned bits) {
  for (unsigned bit = bits; bit < 512; ++bit)
    value[bit / 32] &= ~(uint32_t(1) << (bit % 32));
  return value;
}
Word value(unsigned seed, unsigned bits) {
  Word result{};
  result[0] = seed * 37 + 19;
  result[1] = (seed * 113) ^ 0xdeadbeef;
  result[2] = 1;
  result[3] = seed * 17 + 3;
  result[4] = 1;
  return clipped(result, bits);
}
std::string hex(const Word &word) {
  std::ostringstream out;
  out << std::hex;
  for (unsigned i = word.size(); i > 0; --i)
    out << std::setw(8) << std::setfill('0') << word[i - 1];
  return out.str();
}
std::string address(int index) {
  std::ostringstream out;
  out << '@' << std::hex << uint32_t(index) << '\n';
  return out.str();
}
int main(int argc, char **argv) {
  auto directory = std::filesystem::absolute(argv[0]).parent_path() /
                   "multidimensional-readmem-inputs";
  std::filesystem::create_directories(directory);
  unsigned checks = 0;
  for (unsigned round = 0; round < 2; ++round) {
    auto model = create_model(argc, argv);
    model->initialize();
    for (const auto &mem : memories) {
      std::string scope =
          std::string("ReadMemMultiHarness/") + mem.instance + "/rtl/rtl/ram";
      if (!svSetScope(svGetScopeFromName(scope.c_str())) && !svGetScope())
        throw std::runtime_error("missing multidimensional memory scope");
      const unsigned row = mem.middle * mem.inner;
      for (unsigned test = 0; test < 18; ++test) {
        // Reference locations use the SV numeric indices. They deliberately
        // have no knowledge of the compiler's packed layout or declarations'
        // ascending/descending directions.
        std::vector<Word> expected(3 * row);
        for (unsigned a = 0; a < 3; ++a)
          for (unsigned b = 0; b < mem.middle; ++b)
            for (unsigned c = 0; c < mem.inner; ++c) {
              unsigned i = a * row + b * mem.inner + c;
              expected[i] = value(i + round + 100, mem.bits);
              if (multi_set(mem.low0 + a, mem.low1 + b, mem.low2 + c,
                            expected[i].data()) != 1)
                throw std::runtime_error(
                    "multidimensional memory reset failed");
            }
        std::string contents;
        unsigned sequence = 1;
        auto put = [&](unsigned a, unsigned subword) {
          auto next = value(sequence++ + test * 19, mem.bits);
          contents += hex(next) + " /* word */\n";
          expected[a * row + subword] = next;
        };
        auto fill = [&](unsigned a) {
          for (unsigned i = 0; i < row; ++i)
            put(a, i);
        };
        int mode = 0, start = 0, finish = 0;
        switch (test) {
        case 0:
          for (unsigned a = 0; a < 3; ++a)
            fill(a);
          break;
        case 1:
          put(0, 0);
          put(0, 1);
          break;
        case 2:
          put(0, 0);
          contents += address(mem.low0 + 1);
          put(1, 0);
          put(1, 1);
          break;
        case 3:
          mode = 2;
          start = mem.low0 + 2;
          finish = mem.low0;
          for (int a = 2; a >= 0; --a)
            fill(a);
          break;
        case 4:
          mode = 1;
          start = mem.low0 + 1;
          fill(1);
          put(2, 0);
          break;
        case 5:
        case 12: {
          mode = test == 5 ? 3 : 4;
          start = mem.low0 + 2;
          finish = mem.low0 + 1;
          contents = "1 10 1010 1111111111111\n";
          unsigned values[] = {1, 2, 10, 8191};
          for (unsigned i = 0; i < 4; ++i) {
            Word next{};
            next[0] = values[i];
            expected[(test == 5 ? 0 : 2 * row) + i] = clipped(next, mem.bits);
          }
          break;
        }
        case 6:
          mode = 5;
          fill(1);
          fill(2);
          break;
        case 7:
          mode = 6;
          start = mem.low0 + 2;
          finish = mem.low0 + 1;
          fill(2);
          fill(1);
          break;
        case 8:
          mode = 7;
          put(1, 0);
          put(1, 1);
          break;
        case 9:
          put(0, 0);
          put(0, 1);
          contents += "Q\n";
          break;
        case 10:
          put(0, 0);
          put(0, 1);
          contents += address(mem.low0 + 3) + "12\n";
          break;
        case 11:
          break;
        case 13:
          for (unsigned a = 0; a < 3; ++a)
            fill(a);
          contents += "123\n";
          break;
        case 14:
          put(0, 0);
          contents += address(mem.low0 + 1);
          put(1, 0);
          put(1, 1);
          contents += address(mem.low0 + 2);
          put(2, 0);
          contents += address(mem.low0 + 1);
          put(1, 0);
          break;
        case 15:
          mode = 2;
          start = mem.low0;
          finish = mem.low0 + 3;
          contents = "1\n";
          break;
        case 16:
          contents = "x_1z 3_zx\n";
          expected[0] = Word{};
          expected[0][0] = 0x10;
          expected[1] = Word{};
          expected[1][0] = 0x300;
          expected[0] = clipped(expected[0], mem.bits);
          expected[1] = clipped(expected[1], mem.bits);
          break;
        case 17:
          mode = 6;
          start = mem.low0;
          finish = mem.low0 + 1;
          contents = "1\n";
          break;
        }
        auto path = directory / "input.txt";
        if (test == 11)
          std::filesystem::remove(path);
        else {
          std::ofstream file(path);
          file << contents;
        }
        multi_load(path.c_str(), mode, start, finish);
        for (unsigned a = 0; a < 3; ++a)
          for (unsigned b = 0; b < mem.middle; ++b)
            for (unsigned c = 0; c < mem.inner; ++c) {
              unsigned i = a * row + b * mem.inner + c;
              Word actual{};
              if (multi_get(mem.low0 + a, mem.low1 + b, mem.low2 + c,
                            actual.data()) != 1 ||
                  actual != expected[i]) {
                std::fprintf(stderr,
                             "multi-readmem scope=%s test=%u location=%u/%u/%u "
                             "got=%x:%x:%x:%x:%x expected=%x:%x:%x:%x:%x\n",
                             scope.c_str(), test, a, b, c, actual[4], actual[3],
                             actual[2], actual[1], actual[0], expected[i][4],
                             expected[i][3], expected[i][2], expected[i][1],
                             expected[i][0]);
                throw std::runtime_error(
                    "multidimensional readmem changed SV words");
              }
              ++checks;
            }
      }
    }
  }
  std::printf(
      "PASS: %u multidimensional readmem words; 2D/3D, mixed directions, "
      "negative bounds, 9/13/65/129-bit words, row addresses, reverse "
      "ranges, slices, partial indexing, binary/hex, holes, partial "
      "failures, truncation, X/Z and model recreation\n",
      checks);
}
