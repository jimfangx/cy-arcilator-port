#include "native/runtime.h"
#include "svdpi.h"
#include <cstdio>
#include <map>
#include <vector>
using namespace chipyard_arc;
static unsigned init_calls, tick_calls, data_bytes, expected_word;
static void check(bool good, const char *message) {
  if (!good) throw std::runtime_error(message);
}
extern "C" void *memory_init(int chip, long long size, long long word,
    long long line, long long ids, long long hz, long long base, int addresses) {
  ++init_calls;
  check(chip == 3 && size == 268435456 && line == 64 && ids == 7 &&
        hz == 123456789 && base == 0x180000000LL && addresses == 42,
        "SimDRAM memory_init parameter order, values or 64-bit widths changed");
  if (word != expected_word) {
    std::fprintf(stderr, "WORD_SIZE mismatch: expected=%u actual=%lld bus_bytes=%u\n",
                 expected_word, word, data_bytes);
    throw std::runtime_error("original SimDRAM WORD_SIZE argument was replaced");
  }
  return &init_calls;
}
extern "C" void memory_tick(void *channel, unsigned char reset,
    unsigned char ar_valid, unsigned char *ar_ready, long long ar_addr,
    int ar_id, int ar_size, int ar_len,
    unsigned char aw_valid, unsigned char *aw_ready, long long aw_addr,
    int aw_id, int aw_size, int aw_len,
    unsigned char w_valid, unsigned char *w_ready, int w_strb,
    const svOpenArrayHandle w_data, unsigned char w_last,
    unsigned char *r_valid, unsigned char r_ready, int *r_id, int *r_resp,
    const svOpenArrayHandle r_data, unsigned char *r_last,
    unsigned char *b_valid, unsigned char b_ready, int *b_id, int *b_resp) {
  ++tick_calls;
  check(channel == &init_calls && !reset && init_calls == 1,
        "SimDRAM channel lifecycle/reset callback contract changed");
  check(ar_valid && uint64_t(ar_addr) == 0x8000000001234567ULL && ar_id == 3 &&
        ar_size == 3 && ar_len == 1 && aw_valid &&
        uint64_t(aw_addr) == 0x8000000002345678ULL && aw_id == 4 &&
        aw_size == 3 && aw_len == 2 && w_valid &&
        unsigned(w_strb) == (1u << data_bytes) - 1 && w_last && r_ready &&
        !b_ready, "SimDRAM scalar callback input or width changed");
  check(svSize(w_data, 1) == int(data_bytes) &&
        svSize(r_data, 1) == int(data_bytes),
        "WORD_SIZE must not change DATA_BITS-sized DPI byte arrays");
  for (unsigned i = 0; i < data_bytes; ++i) {
    check(*static_cast<const uint8_t *>(svGetArrElemPtr1(w_data, i)) == 17 + i,
          "SimDRAM write byte order changed");
    *static_cast<uint8_t *>(svGetArrElemPtr1(r_data, i)) = 0xee - i;
  }
  *ar_ready = *aw_ready = *w_ready = *r_valid = *r_last = *b_valid = 1;
  *r_id = 5; *r_resp = 2; *b_id = 6; *b_resp = 1;
}
static void empty_model(void *) {}
static void run_case(unsigned bits, unsigned word, bool explicit_word) {
  init_calls = tick_calls = 0; data_bytes = bits / 8; expected_word = word;
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 16, "DRAMParameters", nullptr};
  Runtime runtime(0, nullptr, info, empty_model);
  auto binding = make_binding(runtime, "SimDRAM");
  binding->parameters = {{"CHIP_ID", "3"}, {"MEM_SIZE", "268435456"},
    {"DATA_BITS", std::to_string(bits)}, {"LINE_SIZE", "64"},
    {"ID_BITS", "7"}, {"CLOCK_HZ", "123456789"},
    {"MEM_BASE", "6442450944"}, {"ADDR_BITS", "42"}};
  if (explicit_word) binding->parameters["WORD_SIZE"] = std::to_string(word);
  std::map<std::string, std::vector<uint8_t>> storage;
  auto port = [&](const char *name, unsigned width, uint64_t value = 0) {
    auto &bytes = storage[name]; bytes.resize((width + 7) / 8);
    binding->ports.emplace(name, Signal{bytes.data(), width});
    if (width <= 64) binding->port(name).set(value);
  };
  port("clock", 1); port("reset", 1, 1);
  port("axi_ar_valid", 1, 1); port("axi_ar_ready", 1, 1);
  port("axi_ar_bits_addr", 64, 0x8000000001234567ULL);
  port("axi_ar_bits_id", 7, 3); port("axi_ar_bits_size", 3, 3);
  port("axi_ar_bits_len", 8, 1);
  port("axi_aw_valid", 1, 1); port("axi_aw_ready", 1, 1);
  port("axi_aw_bits_addr", 64, 0x8000000002345678ULL);
  port("axi_aw_bits_id", 7, 4); port("axi_aw_bits_size", 3, 3);
  port("axi_aw_bits_len", 8, 2);
  port("axi_w_valid", 1, 1); port("axi_w_ready", 1, 1);
  port("axi_w_bits_data", bits); port("axi_w_bits_strb", data_bytes, (1u << data_bytes)-1);
  port("axi_w_bits_last", 1, 1);
  for (unsigned i = 0; i < data_bytes; ++i) storage["axi_w_bits_data"][i] = 17 + i;
  port("axi_r_valid", 1, 1); port("axi_r_ready", 1, 1);
  port("axi_r_bits_id", 7, 99); port("axi_r_bits_resp", 2, 3);
  port("axi_r_bits_data", bits); port("axi_r_bits_last", 1, 1);
  port("axi_b_valid", 1, 1); port("axi_b_ready", 1);
  port("axi_b_bits_id", 7, 98); port("axi_b_bits_resp", 2, 3);
  binding->initialize();
  auto sample = [&](unsigned clock) {
    binding->port("clock").set(clock);binding->sample();binding->commit();
  };
  sample(0);sample(1);
  check(init_calls == 0 && tick_calls == 0 &&
    binding->get("axi_ar_ready") == 0 && binding->get("axi_r_valid") == 0 &&
    binding->get("axi_b_valid") == 0 && binding->get("axi_r_bits_id") == 99 &&
    binding->get("axi_b_bits_id") == 98, "SimDRAM reset lifecycle/output retention changed");
  binding->port("reset").set(0);sample(1);
  check(init_calls == 0, "SimDRAM initialized without a rising edge");
  sample(0);sample(1);sample(1);
  check(init_calls == 1 && tick_calls == 1 && binding->get("axi_ar_ready") == 1 &&
    binding->get("axi_r_valid") == 1 && binding->get("axi_r_bits_id") == 5 &&
    binding->get("axi_r_bits_resp") == 2 && binding->get("axi_b_bits_id") == 6 &&
    binding->get("axi_b_bits_resp") == 1, "SimDRAM edge/response contract changed");
  for (unsigned i = 0; i < data_bytes; ++i)
    check(storage["axi_r_bits_data"][i] == 0xee - i, "SimDRAM read byte order changed");
  sample(0);sample(1);
  binding->port("reset").set(1);sample(0);sample(1);
  check(init_calls == 1 && tick_calls == 2 && !binding->get("axi_r_valid"),
        "SimDRAM reset invoked/reinitialized the original driver");
  binding->port("reset").set(0);sample(0);sample(1);
  check(init_calls == 1 && tick_calls == 3, "SimDRAM channel was not retained across reset");
  std::printf("PASS DATA_BITS=%u WORD_SIZE=%u explicit=%u callbacks=%u\n",
              bits, word, explicit_word, tick_calls);
}
int main() {
  try {
    register_builtin_bindings();
    run_case(64, 8, false);
    run_case(64, 4, true);
    run_case(64, 16, true);
    run_case(128, 16, false);
    std::puts("PASS original SimDRAM parameter ABI, lazy channel/reset/edge handling, 64-bit addresses and byte arrays");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what()); return 1;
  }
}
