#include "runtime.h"
#include <algorithm>
#include <array>
#include <cstdio>

extern "C" {
int debug_tick(unsigned char *, unsigned char, int *, int *, int *,
               unsigned char, unsigned char *, int, int) __attribute__((weak));
int dromajo_init(const char *, const char *, const char *)
    __attribute__((weak));
int dromajo_step(int, long long, int, long long, long long, bool)
    __attribute__((weak));
void dromajo_raise_trap(int, long long) __attribute__((weak));
void memtrace_init(const char *, bool) __attribute__((weak));
void memtrace_query(unsigned char, unsigned long, int, unsigned char *,
                    unsigned long *, unsigned char *, unsigned char *,
                    unsigned long *, unsigned char *) __attribute__((weak));
int memtracelogger_init(int, const char *, const char *) __attribute__((weak));
void memtracelogger_log(int, unsigned char, unsigned long, int, int,
                        unsigned long, unsigned char, int, unsigned long,
                        unsigned char *) __attribute__((weak));
void emulator_init(int) __attribute__((weak));
void emulator_tick(uint8_t *, uint8_t *, long long *, uint8_t *, int *,
                   long long *, uint8_t *, uint8_t *, uint8_t *, int *,
                   long long *, uint8_t, uint8_t *) __attribute__((weak));
void memfuzz_init(int) __attribute__((weak));
void memfuzz_generate(uint8_t *, uint8_t *, long long *, uint8_t *, int *,
                      long long *, uint8_t *, uint8_t *, uint8_t *, int *,
                      uint8_t, uint8_t *) __attribute__((weak));
}
namespace chipyard_arc {
namespace {
template <class T> void require(T symbol, const char *name) {
  if (!symbol)
    throw std::runtime_error(std::string(name) + " driver is not linked");
}
uint64_t slice(Signal signal, unsigned start, unsigned bits) {
  if (!bits || bits > 64 || start > signal.bits || bits > signal.bits - start)
    throw std::runtime_error("invalid native packed lane");
  uint64_t value = 0;
  for (unsigned bit = 0; bit < bits; ++bit)
    value |=
        uint64_t((signal.data[(start + bit) / 8] >> ((start + bit) % 8)) & 1)
        << bit;
  return value;
}
// The older Sodor wrapper samples on the rising edge and uses 100 ps
// continuous-assignment delays. TestchipSimDTM uses the separate DTM class.
class LegacyDTM : public Binding {
  struct Delayed {
    const char *name;
    uint32_t value = 0, next = 0;
    uint64_t at = never;
  };
  std::array<Delayed, 4> inputs{{{"debug_req_ready"},
                                 {"debug_resp_valid"},
                                 {"debug_resp_bits_resp"},
                                 {"debug_resp_bits_data"}}};
  std::array<Delayed, 6> outputs{{{"debug_req_valid"},
                                  {"debug_req_bits_addr"},
                                  {"debug_req_bits_op"},
                                  {"debug_req_bits_data"},
                                  {"debug_resp_ready"},
                                  {"exit"}}};
  bool previous_reset = false;
  unsigned char valid = 0, ready = 0;
  int address = 0, operation = 0, data = 0, exit = 0;
  void schedule(Delayed &signal, uint32_t value) {
    if (value != signal.next) {
      constexpr uint64_t delay_fs = 100000;
      if (delay_fs >= never - runtime.time_fs())
        throw std::runtime_error("native SimDTM delay exceeds time range");
      signal.next = value;
      signal.at = runtime.time_fs() + delay_fs;
    }
  }

public:
  using Binding::Binding;
  void initialize() override {
    if (!debug_tick)
      throw std::runtime_error("SimDTM driver is not linked");
  }
  bool combinational() override {
    for (auto &signal : inputs)
      schedule(signal, get(signal.name));
    return false;
  }
  void sample() override {
    bool clock = get("clk"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    bool reset = get("reset");
    if (reset || previous_reset) {
      valid = ready = 0;
      exit = 0;
    } else
      exit =
          debug_tick(&valid, inputs[0].value, &address, &operation, &data,
                     inputs[1].value, &ready, inputs[2].value, inputs[3].value);
    previous_reset = reset;
    const uint32_t values[] = {
        valid, uint32_t(address) & 127, uint32_t(operation) & 3, uint32_t(data),
        ready, uint32_t(exit)};
    for (unsigned i = 0; i < outputs.size(); ++i)
      schedule(outputs[i], values[i]);
  }
  uint64_t next_time_fs() const override {
    uint64_t next = never;
    for (auto &signal : inputs)
      next = std::min(next, signal.at);
    for (auto &signal : outputs)
      next = std::min(next, signal.at);
    return next;
  }
  uint64_t next_time() const override {
    auto at = next_time_fs();
    return at == never ? never : at / 1000 + bool(at % 1000);
  }
  void timed() override {
    for (auto &signal : inputs)
      if (signal.at == runtime.time_fs()) {
        signal.value = signal.next;
        signal.at = never;
      }
    for (auto &signal : outputs)
      if (signal.at == runtime.time_fs()) {
        signal.value = signal.next;
        port(signal.name).set(signal.value);
        signal.at = never;
      }
  }

public:
  constexpr BindingCallbackContract<LegacyDTM>
  native_callback_contract() const {
    return reviewed_base_callback_contract<LegacyDTM>();
  }
};

class Lanes : public Binding {
protected:
  unsigned lanes = 0;
  using Binding::Binding;
  void initialize_lanes() {
    auto count = number("NUM_LANES", 4);
    if (!count || count > 65536)
      throw std::runtime_error("invalid native lane count");
    lanes = count;
  }
  unsigned lane_bits(const std::string &name) const {
    auto signal = port(name);
    if (signal.bits % lanes || signal.bits / lanes == 0 ||
        signal.bits / lanes > 64)
      throw std::runtime_error("invalid native lane width for " + name);
    return signal.bits / lanes;
  }
  uint64_t lane(const std::string &name, unsigned index) const {
    unsigned bits = lane_bits(name);
    return slice(port(name), index * bits, bits);
  }
  template <class T> void packed(const std::string &name, const T &values) {
    auto signal = port(name);
    unsigned bits = lane_bits(name);
    std::vector<uint8_t> bytes(signal.bytes());
    for (unsigned index = 0; index < lanes; ++index)
      for (unsigned bit = 0; bit < bits; ++bit)
        bytes[(index * bits + bit) / 8] |=
            ((uint64_t(values[index]) >> bit) & 1)
            << ((index * bits + bit) % 8);
    stage(name, bytes);
  }

public:
  constexpr BindingCallbackContract<Lanes> native_callback_contract() const {
    return reviewed_base_callback_contract<Lanes>();
  }
};
class Dromajo : public Binding {
  unsigned width = 0, xlen = 0, instruction_bits = 0;

public:
  using Binding::Binding;
  void initialize() override {
    require(dromajo_init, "Dromajo");
    require(dromajo_step, "Dromajo");
    require(dromajo_raise_trap, "Dromajo");
    width = number("COMMIT_WIDTH");
    xlen = number("XLEN");
    instruction_bits = number("INST_BITS");
    if (!width || !xlen || xlen > 64 || !instruction_bits ||
        instruction_bits > 64 || port("valid").bits != width ||
        port("pc").bits != uint64_t(width) * xlen ||
        port("wdata").bits != uint64_t(width) * xlen ||
        port("mstatus").bits != uint64_t(width) * xlen ||
        port("inst").bits != uint64_t(width) * instruction_bits ||
        port("check").bits != width)
      throw std::runtime_error("invalid native Dromajo boundary widths");
    if (dromajo_init(runtime.option("drj_rom=").c_str(),
                     runtime.option("drj_dtb=").c_str(),
                     runtime.option("drj_bin=").c_str()))
      throw std::runtime_error("Dromajo initialization failed");
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge || get("reset"))
      return;
    for (unsigned slot = 0; slot < width; ++slot)
      if (slice(port("valid"), slot, 1)) {
        auto status = dromajo_step(
            get("hartid"), slice(port("pc"), slot * xlen, xlen),
            slice(port("inst"), slot * instruction_bits, instruction_bits),
            slice(port("wdata"), slot * xlen, xlen),
            slice(port("mstatus"), slot * xlen, xlen),
            slice(port("check"), slot, 1));
        if (status) {
          runtime.fail("Dromajo failed with exit code " +
                       std::to_string(status));
          return;
        }
      }
    if (get("int_xcpt"))
      dromajo_raise_trap(get("hartid"), get("cause"));
  }

public:
  constexpr BindingCallbackContract<Dromajo> native_callback_contract() const {
    return reviewed_base_callback_contract<Dromajo>();
  }
};
class MemoryTrace : public Lanes {
public:
  using Lanes::Lanes;
  void initialize() override {
    initialize_lanes();
    require(memtrace_init, "memory trace");
    require(memtrace_query, "memory trace");
    memtrace_init(text("FILENAME", "undefined").c_str(), number("HAS_SOURCE"));
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge || (!get("reset") && !get("trace_read_ready")))
      return;
    std::vector<unsigned char> valid(lanes), store(lanes), size(lanes);
    std::vector<unsigned long> address(lanes), data(lanes);
    unsigned char finished = 0;
    if (!get("reset"))
      for (unsigned index = 0; index < lanes; ++index)
        memtrace_query(get("trace_read_ready"), get("trace_read_cycle"), index,
                       &valid[index], &address[index], &store[index],
                       &size[index], &data[index], &finished);
    packed("trace_read_valid", valid);
    packed("trace_read_address", address);
    packed("trace_read_is_store", store);
    packed("trace_read_size", size);
    packed("trace_read_data", data);
    stage("trace_read_finished", finished);
  }

public:
  constexpr BindingCallbackContract<MemoryTrace>
  native_callback_contract() const {
    return reviewed_base_callback_contract<MemoryTrace>();
  }
};
class MemoryLogger : public Lanes {
  int handle = 0;
  uint64_t cycle = 0;
  unsigned char ready = 0;

public:
  using Lanes::Lanes;
  void initialize() override {
    initialize_lanes();
    require(memtracelogger_init, "memory trace logger");
    require(memtracelogger_log, "memory trace logger");
    handle = memtracelogger_init(number("IS_RESPONSE"),
                                 text("FILENAME_BASE", "undefined").c_str(),
                                 text("FILENAME_SUFFIX", ".log").c_str());
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    if (get("reset")) {
      ready = 1;
      cycle = 0;
    } else {
      for (unsigned index = 0; index < lanes; ++index)
        memtracelogger_log(
            handle, lane("trace_log_valid", index), cycle, index,
            lane("trace_log_source", index), lane("trace_log_address", index),
            lane("trace_log_is_store", index), lane("trace_log_size", index),
            lane("trace_log_data", index), &ready);
      ++cycle;
    }
    stage("trace_log_ready", ready);
  }

public:
  constexpr BindingCallbackContract<MemoryLogger>
  native_callback_contract() const {
    return reviewed_base_callback_contract<MemoryLogger>();
  }
};
class LaneDriver : public Lanes {
  // SimDefaults.vh defines static DPI array dimensions independently of
  // NUM_LANES.
  static constexpr unsigned max_lanes = 32;
  std::array<uint8_t, max_lanes> a_ready{}, a_valid{}, a_store{}, d_ready{},
      d_valid{}, d_store{};
  std::array<int, max_lanes> a_size{}, d_size{};
  std::array<long long, max_lanes> a_address{}, a_data{}, d_data{};
  uint8_t finished = 0;

public:
  using Lanes::Lanes;
  void initialize() override {
    initialize_lanes();
    if (lanes > max_lanes)
      throw std::runtime_error(
          "native lane count exceeds the original static DPI array dimension");
    if (module == "SimEmulator") {
      require(emulator_init, "emulator");
      require(emulator_tick, "emulator");
      emulator_init(lanes);
    } else {
      require(memfuzz_init, "memory fuzzer");
      require(memfuzz_generate, "memory fuzzer");
      memfuzz_init(lanes);
    }
  }
  void sample() override {
    bool clock = get("clock");
    bool edge =
        module == "SimEmulator" ? clock && !old_clock : !clock && old_clock;
    old_clock = clock;
    if (!edge)
      return;
    if (get("reset")) {
      a_valid.fill(0);
      a_address.fill(0);
      a_store.fill(0);
      a_size.fill(0);
      a_data.fill(0);
      d_ready.fill(0);
      finished = 0;
    } else {
      a_ready.fill(0);
      d_valid.fill(0);
      d_store.fill(0);
      d_size.fill(0);
      d_data.fill(0);
      for (unsigned index = 0; index < lanes; ++index) {
        a_ready[index] = lane("a_ready", index);
        d_valid[index] = lane("d_valid", index);
        d_store[index] = lane("d_is_store", index);
        d_size[index] = lane("d_size", index);
        if (module == "SimEmulator")
          d_data[index] = lane("d_data", index);
      }
      if (module == "SimEmulator")
        emulator_tick(a_ready.data(), a_valid.data(), a_address.data(),
                      a_store.data(), a_size.data(), a_data.data(),
                      d_ready.data(), d_valid.data(), d_store.data(),
                      d_size.data(), d_data.data(), get("inflight"), &finished);
      else {
        memfuzz_generate(a_ready.data(), a_valid.data(), a_address.data(),
                         a_store.data(), a_size.data(), a_data.data(),
                         d_ready.data(), d_valid.data(), d_store.data(),
                         d_size.data(), get("inflight"), &finished);
        for (unsigned index = 0; index < lanes; ++index)
          std::printf("verilog: %04llu a_valid[%d]=%d, a_address[%d]=0x%llx, "
                      "d_ready[%d]=%d\n",
                      (unsigned long long)((runtime.time() + 500) / 1000),
                      index, a_valid[index], index,
                      (unsigned long long)a_address[index], index,
                      d_ready[index]);
        if (finished)
          runtime.finish();
      }
    }
    packed("a_valid", a_valid);
    packed("a_address", a_address);
    packed("a_is_store", a_store);
    packed("a_size", a_size);
    packed("a_data", a_data);
    packed("d_ready", d_ready);
    stage("finished", finished);
  }

public:
  constexpr BindingCallbackContract<LaneDriver>
  native_callback_contract() const {
    return reviewed_base_callback_contract<LaneDriver>();
  }
};
template <class T> void add(const char *name) {
  register_binding(name, [](Runtime &r, const std::string &n) {
    return make_binding_instance<T>(r, n);
  });
}
} // namespace
void register_additional_bindings() {
#define ARC_ADDITIONAL_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_ADDITIONAL_MODEL
}
} // namespace chipyard_arc
