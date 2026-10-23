#include "native/runtime.h"
#include <array>
#include <cstdio>
using namespace chipyard_arc;
extern "C" int string_probe(const char *, int);
extern "C" const char *string_transform(const char *, svBit);
extern "C" int string_order(const char *, const char *);
extern "C" int string_compare(const char *, const char *);
extern "C" int string_update(const char **, const char **);
extern "C" void string_save(const char *);
extern "C" const char *string_restore();
extern "C" const char *string_unpack(const svBitVecVal *);
extern "C" void string_pack(const char *, svBitVecVal *);
extern "C" int update_counter(int, int *);
extern "C" int clear_counter();
extern "C" int no_op();
extern "C" int secret_set(int);
extern "C" int secret_get();
extern "C" int word_info(int);
extern "C" int64_t mix_abi(int8_t, uint16_t, svBit, svLogic, svLogicVecVal *,
                           int32_t *);

extern "C" int set_word_nba(int, const svBitVecVal *);
extern "C" int set_word(int, const svBitVecVal *);
extern "C" int get_word(int, svBitVecVal *);
extern "C" int callback_write(int, const svBitVecVal *);
static unsigned host_calls = 0;
static svScope expected_host_scope = nullptr;
extern "C" int native_host_update(int index, const svBitVecVal *value) {
  auto scope = svGetScope();
  if (index == INT32_MIN) {
    svSetScope(svGetScopeFromName("ExportHarness/wide/rtl/rtl/ram"));
    throw std::runtime_error("nested import exception");
  }
  if (!scope)
    throw std::runtime_error("context import lost its export scope");
  int status = set_word(index, value);
  svBitVecVal observed[16]{};
  if (get_word(index, observed) != status || svGetScope() != scope)
    throw std::runtime_error("nested context callback lost scope or result");
  ++host_calls;
  expected_host_scope = scope;
  auto name = std::string(svGetNameFromScope(scope));
  auto other = name.find("/wide/") != std::string::npos
                   ? "ExportHarness/narrow/rtl/rtl/ram"
                   : "ExportHarness/wide/rtl/rtl/ram";
  svSetScope(svGetScopeFromName(other));
  return status ? 11 : 19;
}
extern "C" int native_host_probe(int index) {
  return svGetScope() == expected_host_scope ? 5 : -1000;
}
struct RamBinding {
  const char *scope;
  unsigned width, words;
};
static const RamBinding ram_bindings[] = {
    {"ExportHarness/narrow/rtl/rtl/ram", 13, 9},
    {"ExportHarness/wide/rtl/rtl/ram", 65, 7}};
struct Word {
  uint64_t lo;
  uint8_t hi;
};
static std::vector<uint8_t> bytes(Word x, unsigned width) {
  std::vector<uint8_t> out((width + 7) / 8, 0);
  for (unsigned i = 0; i < std::min<unsigned>(8, out.size()); ++i)
    out[i] = x.lo >> (8 * i);
  if (width == 65)
    out[8] = x.hi;
  return out;
}
class ExportCallback : public Binding {
  bool entered = false, sampled = false, previous_clock = false;
  unsigned &calls;

public:
  ExportCallback(Runtime &r, unsigned &calls)
      : Binding(r, "export-callback"), calls(calls) {}
  void sample() override {
    // Like a device driver, sample on an input event. An unconditional call
    // on every settle delta can retrigger itself: a static output formal is
    // assigned zero and then the requested word during each getter call.
    bool clock = runtime.signal("clock").get();
    if (sampled && clock == previous_clock)
      return;
    sampled = true;
    previous_clock = clock;
    if (entered)
      throw std::runtime_error("export recursively evaluated the model");
    entered = true;
    auto previous =
        svSetScope(svGetScopeFromName(ram_bindings[calls % 2].scope));
    if (word_info(123) != (123 ^ int(ram_bindings[calls % 2].width +
                                     ram_bindings[calls % 2].words * 100)))
      throw std::runtime_error("nested export selected wrong specialization");
    svBitVecVal bits[16]{};
    if (get_word(0, bits) != 1)
      throw std::runtime_error("nested captured export failed");
    svSetScope(previous);
    ++calls;
    entered = false;
  }
};
int main(int argc, char **argv) {
  for (unsigned round = 0; round < 3; round++) {
    unsigned callbacks = 0;
    auto model = create_model(argc, argv);
    model->add_binding(std::make_unique<ExportCallback>(*model, callbacks));
    model->initialize();
    // The first public export must complete SV initial blocks before reading.
    svSetScope(svGetScopeFromName("ExportHarness/narrow/rtl/rtl/ram"));
    svBitVecVal initial_value[16]{};
    if (get_word(0, initial_value) != 1 || initial_value[0] != 3)
      throw std::runtime_error("first export missed SV initialization");
    model->advance(0);
    auto selected = svGetScope();
    try {
      callback_write(INT32_MIN, initial_value);
      throw 42;
    } catch (const std::runtime_error &e) {
      if (std::string(e.what()) != "nested import exception")
        throw;
    }
    if (svGetScope() != selected)
      throw std::runtime_error("compiled callback exception leaked scope");
    for (auto &ram : ram_bindings)
      if (!svGetScopeFromName(ram.scope))
        throw std::runtime_error("missing SV scope");
    svSetScope(nullptr);
    try {
      svBitVecVal out[16]{};
      get_word(0, out);
      throw 42;
    } catch (const std::runtime_error &) {
    }
    svSetScope(svGetScopeFromName("ExportHarness"));
    try {
      svBitVecVal out[16]{};
      get_word(0, out);
      throw 42;
    } catch (const std::runtime_error &) {
    }

    std::array<std::vector<Word>, 2> expected;
    for (unsigned n = 0; n < 2; n++)
      for (unsigned i = 0; i < ram_bindings[n].words; i++)
        expected[n].push_back({i * 17 + 3, 0});
    auto exported_get = [&](unsigned n, int index) {
      auto &ram = ram_bindings[n];
      svSetScope(svGetScopeFromName(ram.scope));
      std::array<svBitVecVal, 16> output{};
      int status = get_word(index, output.data());
      bool valid = index >= 0 && unsigned(index) < ram.words;
      if (status != int(valid))
        throw std::runtime_error("exported getter status mismatch");
      if (valid)
        for (unsigned chunk = 0; chunk < 8; chunk++) {
          auto actual = uint64_t(output[chunk * 2]) |
                        (uint64_t(output[chunk * 2 + 1]) << 32);
          auto want = chunk == 0   ? expected[n][index].lo
                      : chunk == 1 ? uint64_t(expected[n][index].hi)
                                   : 0;
          if (actual != want) {
            std::fprintf(
                stderr,
                "get model %u index %d chunk %u: got %llx expected %llx\n", n,
                index, chunk, (unsigned long long)actual,
                (unsigned long long)want);
            throw std::runtime_error("exported getter data mismatch");
          }
        }
    };
    unsigned counters[2] = {99, 99};
    for (unsigned cycle = 0; cycle < 4096; cycle++) {
      unsigned address = cycle % 13;
      Word hardware[2] = {
          {(cycle * 43 + round) & 8191, 0},
          {cycle * 0x123456789abcull + round, uint8_t(cycle & 1)}};
      bool we = cycle % 4 == 1;
      for (unsigned n = 0; n < 2; n++) {
        auto &ram = ram_bindings[n];
        svSetScope(svGetScopeFromName(ram.scope));
        if (cycle % 7 == 0 && no_op() != 0)
          throw std::runtime_error(
              "normal exported task completion status mismatch");
        if (cycle % 13 == 0) {
          if (clear_counter() != 0)
            throw std::runtime_error("exported clear task failed");
          counters[n] = 99;
        }
        int delta = int(cycle * 137 + round + n * 17) - 32768, task_value = -1;
        if (update_counter(delta, &task_value) != 0)
          throw std::runtime_error("exported counter task failed");
        counters[n] = (counters[n] + delta) & 8191;
        if (task_value != int(counters[n]))
          throw std::runtime_error("exported task storage/output mismatch");
        int value = cycle * 37 + round + n * 17;
        auto input = cycle % 17 == 0 ? std::string()
                                     : std::string(cycle % 3 == 0 ? "\xc3\x89-H"
                                                   : cycle % 2    ? "aBc-"
                                                                  : "ZxY-") +
                                           std::to_string(cycle);
        if (cycle == 0 && std::string(string_restore()) != "")
          throw std::runtime_error(
              "retained string was not reset with the model");
        auto expected_string = input;
        for (char &c : expected_string)
          if (c >= 'a' && c <= 'z')
            c -= 32;
        expected_string = "PREFIX:" + expected_string + ":END";
        if (std::string(string_transform(input.c_str(), 1)) != expected_string)
          throw std::runtime_error("native exported string transform mismatch");
        expected_string = input;
        for (char &c : expected_string)
          if (c >= 'A' && c <= 'Z')
            c += 32;
        expected_string = "prefix:" + expected_string + ":end";
        if (std::string(string_transform(input.c_str(), 0)) != expected_string)
          throw std::runtime_error("native exported string lowercase mismatch");
        for (int index :
             {-1, 0, 2, int(input.size()) - 1, int(input.size()), INT32_MAX}) {
          int expected =
              int(input.size()) * 257 +
              (index >= 0 && size_t(index) < input.size() ? int8_t(input[index])
                                                          : 0);
          if (string_probe(input.c_str(), index) != expected)
            throw std::runtime_error(
                "native exported string get/length mismatch");
        }
        for (auto other :
             {std::string(), input, input + "x", std::string("\x80")}) {
          int expected = input < other ? -1 : input == other ? 0 : 1;
          if (string_order(input.c_str(), other.c_str()) != expected)
            throw std::runtime_error(
                "native exported string comparison mismatch");
          int difference = string_compare(input.c_str(), other.c_str());
          if ((difference < 0 ? -1 : difference == 0 ? 0 : 1) != expected)
            throw std::runtime_error("native exported string compare mismatch");
        }
        const char *inout = input.c_str(), *out = nullptr;
        if (string_update(&inout, &out) != 0)
          throw std::runtime_error("exported string task failed");
        if (std::string(inout) != input + "-updated" ||
            std::string(out) != input)
          throw std::runtime_error("native exported string inout mismatch");
        auto expected_saved_text = input;
        string_save(input.c_str());
        input.assign("overwritten caller buffer");
        if (std::string(string_restore()) != expected_saved_text)
          throw std::runtime_error("retained SV string aliases caller buffer");
        const svBitVecVal string_input[5] = {0x00430000, 0x00410042, 0, 0, 1};
        if (std::string(string_unpack(string_input)) != std::string("\1ABC"))
          throw std::runtime_error("wide integer string conversion mismatch");
        svBitVecVal packed_string[3]{};
        string_pack("0123456789", packed_string);
        if (packed_string[0] != 0x36373839u ||
            packed_string[1] != 0x32333435u || packed_string[2] != 1u)
          throw std::runtime_error("native string packing mismatch");

        if (word_info(value) != (value ^ int(ram.words * 100 + ram.width)))
          throw std::runtime_error("stateless export specialization mismatch");
        if (cycle == 0 && secret_get() != 123)
          throw std::runtime_error(
              "C-only storage was not reset with the model");
        if (secret_set(value) != (value & 8191) ||
            secret_get() != (value & 8191))
          throw std::runtime_error("C-only export storage mismatch");
        int8_t a = cycle;
        uint16_t b = cycle * 197;
        svBit mode = cycle & 1;
        svLogic gate = cycle % 4;
        std::array<svLogicVecVal, 3> bits = {
            {{uint32_t(value), uint32_t(cycle * 123)},
             {uint32_t(value * 137), uint32_t(cycle * 91)},
             {uint32_t(cycle), uint32_t(cycle / 2)}}};
        auto want = bits;
        uint32_t mask[3] = {0x9abcdef0, 0x12345678, 1};
        for (unsigned i = 0; i < 3; i++) {
          want[i].aval = (want[i].aval & ~want[i].bval) ^ mask[i];
          want[i].bval = 0;
        }
        want[2].aval &= 1;
        int32_t score = 0, expected_score = int(a) - int(b);
        int64_t result = mix_abi(a, b, mode, gate, bits.data(), &score);
        if (score != expected_score ||
            result != int64_t(expected_score) * 37 + (mode ? 7 : 0) +
                          (gate == 1 ? 11 : 0) ||
            std::memcmp(bits.data(), want.data(), sizeof(bits)))
          throw std::runtime_error(
              "scalar/inout/four-state export ABI mismatch");
      }
      model->advance(cycle * 10 + 1, [&] {
        model->signal("clock").set(0);
        model->signal("we").set(we);
        model->signal("address").set(address);
        model->signal("narrow_data").set(hardware[0].lo);
        model->signal("wide_data").assign(bytes(hardware[1], 65));
      });
      for (unsigned n = 0; n < 2; n++) {
        int index = int((cycle * 5 + n * 7) % 17) - 3;
        if (cycle % 3 == 0) {
          auto &ram = ram_bindings[n];
          std::array<uint64_t, 8> value{};
          for (unsigned i = 0; i < 8; i++)
            value[i] = (uint64_t(cycle + 1) * 0xfedcba987654321ull) ^
                       uint64_t(i * 197 + round);
          Word word{n ? value[0] : value[0] & 8191,
                    uint8_t(n ? (value[1] & 1) : 0)};
          svSetScope(svGetScopeFromName(ram.scope));
          std::array<svBitVecVal, 16> input{};
          for (unsigned i = 0; i < 8; i++) {
            input[i * 2] = value[i];
            input[i * 2 + 1] = value[i] >> 32;
          }
          bool valid = index >= 0 && unsigned(index) < ram.words;
          int status;
          if (cycle % 2) {
            int expected_return = valid ? int32_t(expected[n][index].lo) : -1;
            status = set_word_nba(index, input.data());
            if (status != expected_return)
              throw std::runtime_error(
                  "exported NBA setter did not return pre-update data");
            status = valid;
          } else {
            status = callback_write(index, input.data());
            if (status != (valid ? 16 : 24))
              throw std::runtime_error(
                  "nested context export write/scope mismatch");
            status = valid;
          }
          if (status != int(valid))
            throw std::runtime_error("exported setter status mismatch");
          if (valid)
            expected[n][index] = word;
        }
        exported_get(n, index);
      }
      model->settle();
      auto observe = [&] {
        for (unsigned n = 0; n < 2; n++) {
          auto word =
              address < expected[n].size() ? expected[n][address] : Word{0, 0};
          if (model->signal(n ? "wide" : "narrow").copy() !=
              bytes(word, n ? 65 : 13))
            throw std::runtime_error(
                "SV hardware did not observe C export write");
        }
      };
      observe();
      model->advance(cycle * 10 + 5, [&] { model->signal("clock").set(1); });
      for (unsigned n = 0; n < 2; n++)
        counters[n] = (counters[n] + unsigned(we)) & 8191;
      for (unsigned n = 0; n < 2; n++) {
        if (we && address < expected[n].size())
          expected[n][address] = hardware[n];
        exported_get(n, address);
      }
      observe();
    }
    if (callbacks != 8192)
      throw std::runtime_error("missing nested DPI callback coverage");
  }
  if (svGetScopeFromName(ram_bindings[0].scope))
    throw std::runtime_error("stale export scope survived model destruction");
  if (host_calls < 4000)
    throw std::runtime_error("nested context imports were not exercised");
  std::puts(
      "PASS: 12288 public C/SV exported task/function cycles with "
      "owned strings, byte operations, packed conversions and "
      "scalar/vector/inout ABI, stateless scopes and C-only storage, two RTL "
      "specializations, "
      "13/65-bit words, packed 512-bit ABI, invalid indices, C/SV write "
      "coherence and model recreation");
}
