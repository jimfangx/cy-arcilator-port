#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>

static unsigned emulator_calls = 0, fuzzer_calls = 0, commits = 0, traps = 0;
static bool run_fuzzer = false;
static void check(bool good, const char *message) { if (!good) throw std::runtime_error(message); }
extern "C" int dromajo_init(const char *rom, const char *dtb, const char *binary) {
  check(std::string(rom) == "boot.rom" && std::string(dtb) == "tree.dtb" && std::string(binary) == "program.elf", "Dromajo initialization arguments");
  return 0;
}
extern "C" int dromajo_step(int hart, long long pc, int insn, long long data, long long mstatus, bool verify) {
  unsigned slot = commits++ ? 2 : 0;
  check(commits <= 2 && hart == 17 && uint64_t(pc) == 0x8000000000001000ULL + slot * 4 &&
        uint32_t(insn) == 0x80000013u + slot && uint64_t(data) == 0xfedcba9876543210ULL + slot &&
        uint64_t(mstatus) == 0xff00000000000000ULL + slot && verify == (slot == 2), "Dromajo commit lane/width/order");
  return 0;
}
extern "C" void dromajo_raise_trap(int hart, long long cause) {
  check(commits == 2 && hart == 17 && uint64_t(cause) == 0x8000000000000007ULL, "Dromajo trap ordering"); ++traps;
}
extern "C" void emulator_init_rs(int lanes) { check(lanes == 3, "emulator initialization"); }
extern "C" void memfuzz_init_rs(int lanes) { check(lanes == 3, "fuzzer initialization"); }
static void lane_driver(bool emulator, uint8_t *ready, uint8_t *valid, long long *address, uint8_t *store,
                        int *size, long long *data, uint8_t *response_ready, uint8_t *response_valid,
                        uint8_t *response_store, int *response_size, long long *response_data,
                        uint8_t inflight, uint8_t *finished) {
  unsigned count = emulator ? ++emulator_calls : ++fuzzer_calls;
  for (unsigned lane = 0; lane < 32; ++lane) {
    bool used = lane < 3 && !run_fuzzer;
    check(ready[lane] == (used && lane != 1) && response_valid[lane] == (used && lane == 1) &&
          response_store[lane] == (used && lane == 1) && response_size[lane] == (used ? 0x80 + lane : 0), "static DPI input arrays/unused lanes");
    if (emulator) check(uint64_t(response_data[lane]) == (used ? 0x8123456789abc000ULL + lane : 0), "emulator response payload");
    valid[lane] = lane != 1; address[lane] = 0xf000000000001000ULL + lane + count * 16;
    store[lane] = lane == 2; size[lane] = 0x100 + lane + count; data[lane] = 0x8877665544332200ULL + lane;
    response_ready[lane] = lane == 1;
  }
  check(inflight == !run_fuzzer, "lane driver inflight");
  *finished = run_fuzzer || count == 3;
}
extern "C" void emulator_tick_rs(uint8_t *ar, uint8_t *av, long long *aa, uint8_t *aw, int *as, long long *ad,
                                  uint8_t *dr, uint8_t *dv, uint8_t *dw, int *ds, long long *dd, uint8_t inflight, uint8_t *finished) {
  lane_driver(true, ar, av, aa, aw, as, ad, dr, dv, dw, ds, dd, inflight, finished);
}
extern "C" void memfuzz_generate_rs(uint8_t *ar, uint8_t *av, long long *aa, uint8_t *aw, int *as, long long *ad,
                                     uint8_t *dr, uint8_t *dv, uint8_t *dw, int *ds, uint8_t inflight, uint8_t *finished) {
  lane_driver(false, ar, av, aa, aw, as, ad, dr, dv, dw, ds, nullptr, inflight, finished);
}
int main(int argc, char **argv) {
  for (int i = 0; i < argc; ++i) run_fuzzer |= std::string(argv[i]) == "+run-fuzzer";
  auto r = chipyard_arc::create_model(argc, argv);
  if (run_fuzzer) return r->run();
  r->initialize();
  auto scalar = [&](const char *name, uint64_t expected) { check(r->signal(name).get() == expected, name); };
  auto words = [&](const char *name, std::array<uint64_t, 3> values, bool drive = false) {
    auto pointer = reinterpret_cast<uint8_t *>(values.data());
    std::vector<uint8_t> bytes(pointer, pointer + sizeof(values));
    if (drive) r->signal(name).assign(bytes); else check(r->signal(name).copy() == bytes, name);
  };
  auto tick = [&](uint64_t time, bool clock) { r->advance(time, [&] { r->signal("clock").set(clock); }); };
  r->advance(0, [&] { r->signal("reset").set(1); });
  tick(500, true); tick(750, false);
  r->advance(900, [&] {
    r->signal("reset").set(0);
    for (auto prefix : {"emu", "fuzz"}) {
      r->signal(std::string(prefix) + "_a_ready").set(5); r->signal(std::string(prefix) + "_d_valid").set(2);
      r->signal(std::string(prefix) + "_d_is_store").set(2); r->signal(std::string(prefix) + "_d_size").set(0x828180);
      r->signal(std::string(prefix) + "_inflight").set(1);
    }
    words("emu_d_data", {0x8123456789abc000ULL, 0x8123456789abc001ULL, 0x8123456789abc002ULL}, true);
    r->signal("drj_valid").set(5); r->signal("drj_hartid").set(17); r->signal("drj_check").set(4);
    r->signal("drj_int_xcpt").set(1); r->signal("drj_cause").set(0x8000000000000007ULL);
    words("drj_pc", {0x8000000000001000ULL, 0x8000000000001004ULL, 0x8000000000001008ULL}, true);
    std::array<uint32_t, 3> insns{0x80000013, 0x80000014, 0x80000015};
    auto p = reinterpret_cast<uint8_t *>(insns.data()); r->signal("drj_inst").assign(std::vector<uint8_t>(p, p + sizeof(insns)));
    words("drj_wdata", {0xfedcba9876543210ULL, 0xfedcba9876543211ULL, 0xfedcba9876543212ULL}, true);
    words("drj_mstatus", {0xff00000000000000ULL, 0xff00000000000001ULL, 0xff00000000000002ULL}, true);
    r->signal("logger_trace_log_valid").set(5); r->signal("logger_trace_log_is_store").set(4);
    r->signal("logger_trace_log_size").set(0x030201);
    std::array<uint32_t, 3> sources{33, 34, 35}; p = reinterpret_cast<uint8_t *>(sources.data());
    r->signal("logger_trace_log_source").assign(std::vector<uint8_t>(p, p + sizeof(sources)));
    words("logger_trace_log_address", {0x8000000000001111ULL, 2, 0x123456789abcdefULL}, true);
    words("logger_trace_log_data", {0xfedcba9876543210ULL, 4, 0x8877665544332211ULL}, true);
  });
  tick(1000, true); scalar("trace_trace_read_valid", 0); scalar("logger_trace_log_ready", 1);
  check(commits == 2 && traps == 1 && emulator_calls == 1 && !fuzzer_calls, "optional driver edge/callback counts");
  scalar("emu_a_valid", 5); scalar("emu_a_size", 0x030201); scalar("emu_d_ready", 2);
  words("emu_a_address", {0xf000000000001010ULL, 0xf000000000001011ULL, 0xf000000000001012ULL});
  r->advance(1200, [&] { r->signal("trace_trace_read_ready").set(1); r->signal("drj_valid").set(0); r->signal("drj_int_xcpt").set(0); });
  tick(1250, false); scalar("fuzz_a_valid", 5); check(fuzzer_calls == 1, "fuzzer falling edge");
  tick(1500, true); scalar("trace_trace_read_valid", 5); scalar("trace_trace_read_is_store", 1);
  scalar("trace_trace_read_size", 0x020003); scalar("trace_trace_read_finished", 0);
  words("trace_trace_read_address", {0x8000000000001111ULL, 0, 0x123456789abcdefULL});
  words("trace_trace_read_data", {0xfedcba9876543210ULL, 0, 0x8877665544332211ULL});
  r->advance(1600, [&] { r->signal("trace_trace_read_ready").set(0); r->signal("trace_trace_read_cycle").set(1); });
  tick(1750, false); tick(2000, true); scalar("trace_trace_read_valid", 5); scalar("trace_trace_read_finished", 0);
  r->advance(2100, [&] { r->signal("trace_trace_read_ready").set(1); });
  tick(2250, false); scalar("fuzz_finished", 1);
  tick(2500, true); scalar("trace_trace_read_valid", 2); scalar("trace_trace_read_is_store", 2);
  scalar("trace_trace_read_size", 0x000100); scalar("trace_trace_read_finished", 1);
  words("trace_trace_read_address", {0, 0xfffffff001020304ULL, 0});
  words("trace_trace_read_data", {0, 0x123456789abcdefULL, 0});
  r->advance(2600, [&] { r->signal("reset").set(1); });
  tick(2750, false); tick(3000, true);
  scalar("trace_trace_read_valid", 0); scalar("emu_a_valid", 0); scalar("fuzz_a_valid", 0);
  std::fflush(nullptr);
  auto path = r->option("memtracefile=") + ".native-log";
  std::ifstream log(path); std::string text((std::istreambuf_iterator<char>(log)), {});
  check(text.find("0 LOAD 0 0 33 0x8000000000001111 0xfedcba9876543210 2\n") != std::string::npos, "original logger payload/source packing");
  check(text.find("3 STORE 0 2 35 0x123456789abcdef 0x8877665544332211 8\n") != std::string::npos, "original logger counter/lane ordering");
  std::puts("PASS: original Radiance trace/logger and emulator/fuzzer wrappers, wide lanes, reset, backpressure, and Dromajo ABI");
}
