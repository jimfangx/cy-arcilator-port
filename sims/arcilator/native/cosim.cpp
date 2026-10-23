#include "runtime.h"
#include <array>

extern "C" {
void cospike_set_sysinfo_wrapper(const char *, const char *, int, int,
                                 long long, long long, long long, long long,
                                 long long, long long, int, const char *)
    __attribute__((weak));
void cospike_cosim_wrapper(long long, long long, int, int, long long,
                           unsigned long, int, int, unsigned long long,
                           unsigned long long, int) __attribute__((weak));
void cospike_register_memory_wrapper(long long, long long)
    __attribute__((weak));
void cospike_register_device_wrapper(long long, long long)
    __attribute__((weak));
void cospike_register_csr_wrapper(long long, long long, long long)
    __attribute__((weak));
void cospike_set_target_params_wrapper(int, int, int) __attribute__((weak));
void debug_rob_push_trace(int, char, char, long long, char, long long,
                          long long, int, char, char, long long, long long,
                          long long *) __attribute__((weak));
void debug_rob_push_wb(int, char, long long, long long *) __attribute__((weak));
void debug_rob_pop_trace(int, char *, long long *, long long *, int *, char *,
                         char *, long long *, long long *, long long *)
    __attribute__((weak));
void ingress_tick(long long, const char *, long long, unsigned char,
                  unsigned char *, unsigned char *, unsigned char *,
                  long long *, long long *) __attribute__((weak));
void egress_tick(long long, const char *, long long, unsigned char *,
                 unsigned char, unsigned char, unsigned char, long long,
                 long long, unsigned char *, unsigned char *)
    __attribute__((weak));
}

namespace chipyard_arc {
namespace {
template <class T> void require(T symbol, const char *name) {
  if (!symbol)
    throw std::runtime_error(std::string(name) + " driver is not linked");
}
class Cosim : public Binding {
public:
  using Binding::Binding;
  void initialize() override {
    if (module == "SpikeCosim") {
      require(cospike_set_sysinfo_wrapper, "Cospike");
      require(cospike_cosim_wrapper, "Cospike");
      cospike_set_sysinfo_wrapper(
          text("ISA").c_str(), text("PRIV").c_str(), number("PMPREGIONS"),
          number("MAXPGLEVELS"), number("MEM0_BASE"), number("MEM0_SIZE"),
          number("MEM1_BASE"), number("MEM1_SIZE"), number("MEM2_BASE"),
          number("MEM2_SIZE"), number("NHARTS"), text("BOOTROM").c_str());
    } else if (module == "SpikeCosimRegisterMemory") {
      require(cospike_register_memory_wrapper, "Cospike memory");
      cospike_register_memory_wrapper(number("BASE"), number("SIZE"));
    } else if (module == "SpikeCosimRegisterDevice") {
      require(cospike_register_device_wrapper, "Cospike device");
      cospike_register_device_wrapper(number("BASE"), number("SIZE"));
    } else if (module == "SpikeCosimRegisterCSR") {
      require(cospike_register_csr_wrapper, "Cospike CSR");
      cospike_register_csr_wrapper(number("ADDR"), number("MASK"),
                                   number("INIT"));
    } else if (module == "SpikeCosimTargetParams") {
      require(cospike_set_target_params_wrapper, "Cospike parameters");
      cospike_set_target_params_wrapper(
          number("PADDRBITS"), number("VADDRBITSEXTENDED"), number("NPMPCSRS"));
    }
  }
  void sample() override {
    if (module != "SpikeCosim")
      return;
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge || get("reset"))
      return;
    for (unsigned slot = 0; slot != 4; ++slot) {
      auto prefix = "trace_" + std::to_string(slot) + "_";
      if (get(prefix + "valid") || get(prefix + "exception") ||
          get(prefix + "cause"))
        cospike_cosim_wrapper(
            get("cycle"), get("hartid"), get(prefix + "has_wdata"),
            get(prefix + "valid"), get(prefix + "iaddr"), get(prefix + "insn"),
            get(prefix + "exception"), get(prefix + "interrupt"),
            get(prefix + "cause"), get(prefix + "wdata"), get(prefix + "priv"));
    }
  }

public:
  constexpr BindingCallbackContract<Cosim> native_callback_contract() const {
    return reviewed_base_callback_contract<Cosim>();
  }
};
class DebugROB : public Binding {
  bool previous_reset = false;
  char valid = 0, exception = 0, interrupt = 0;
  long long address = 0, insn = 0, cause = 0, tval = 0;
  int priv = 0;
  std::array<long long, 8> data{};

public:
  using Binding::Binding;
  void initialize() override {
    if (module == "DebugROBPushTrace")
      require(debug_rob_push_trace, "DebugROB trace");
    else if (module == "DebugROBPushWb")
      require(debug_rob_push_wb, "DebugROB writeback");
    else
      require(debug_rob_pop_trace, "DebugROB pop");
  }
  void sample() override {
    bool clock = get("clock"), rising = clock && !old_clock,
         falling = !clock && old_clock;
    old_clock = clock;
    if (module == "DebugROBPopTrace") {
      if (falling) {
        bool reset = get("reset");
        if (!reset && !previous_reset)
          debug_rob_pop_trace(get("hartid"), &valid, &address, &insn, &priv,
                              &exception, &interrupt, &cause, &tval,
                              data.data());
        previous_reset = reset;
      }
      if (rising) {
        stage("trace_valid", uint8_t(valid));
        stage("trace_iaddr", address);
        stage("trace_insn", insn);
        stage("trace_priv", uint32_t(priv));
        stage("trace_exception", uint8_t(exception));
        stage("trace_interrupt", uint8_t(interrupt));
        stage("trace_cause", cause);
        stage("trace_tval", tval);
        auto bytes = reinterpret_cast<uint8_t *>(data.data());
        stage("trace_wdata", std::vector<uint8_t>(bytes, bytes + sizeof(data)));
      }
    } else if (rising && !get("reset")) {
      auto value =
          port(module == "DebugROBPushWb" ? "wb_data" : "trace_wdata").copy();
      if (value.size() != sizeof(data))
        throw std::runtime_error("DebugROB writeback must be 512 bits");
      std::memcpy(data.data(), value.data(), sizeof(data));
      if (module == "DebugROBPushWb")
        debug_rob_push_wb(get("hartid"), get("valid"), get("wb_tag"),
                          data.data());
      else
        debug_rob_push_trace(
            get("hartid"), get("should_wb"), get("has_wb"), get("wb_tag"),
            get("trace_valid"), get("trace_iaddr"), get("trace_insn"),
            get("trace_priv"), get("trace_exception"), get("trace_interrupt"),
            get("trace_cause"), get("trace_tval"), data.data());
    }
  }

public:
  constexpr BindingCallbackContract<DebugROB> native_callback_contract() const {
    return reviewed_base_callback_contract<DebugROB>();
  }
};
class Traffic : public Binding {
  std::string config;
  long long endpoint = 0, unique = 0;
  unsigned char valid = 0, head = 0, tail = 0, ready = 0, success = 0,
                fatal = 0;

public:
  using Binding::Binding;
  void initialize() override {
    config = text("CONFIG_STR");
    if (module == "TrafficEvalIngress")
      require(ingress_tick, "TrafficEval ingress");
    else
      require(egress_tick, "TrafficEval egress");
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    if (module == "TrafficEvalIngress") {
      if (get("reset")) {
        valid = 0;
        stage("flit_out_valid", 0);
        return;
      }
      ingress_tick(number("INGRESS_ID"), config.c_str(), get("current_cycle"),
                   get("flit_out_ready"), &valid, &head, &tail, &endpoint,
                   &unique);
      stage("flit_out_valid", valid);
      stage("flit_out_head", head);
      stage("flit_out_tail", tail);
      stage("flit_out_egress_id", endpoint);
      stage("flit_out_unique_id", unique);
    } else {
      if (get("reset"))
        ready = success = fatal = 0;
      else
        egress_tick(number("EGRESS_ID"), config.c_str(), get("current_cycle"),
                    &ready, get("flit_in_valid"), get("flit_in_head"),
                    get("flit_in_tail"), get("flit_in_ingress_id"),
                    get("flit_in_unique_id"), &success, &fatal);
      stage("flit_in_ready", ready);
      stage("success", success);
      stage("fatal", fatal);
    }
  }

public:
  constexpr BindingCallbackContract<Traffic> native_callback_contract() const {
    return reviewed_base_callback_contract<Traffic>();
  }
};
template <class T> void add(const char *name) {
  register_binding(name, [](Runtime &r, const std::string &n) {
    return make_binding_instance<T>(r, n);
  });
}
} // namespace
void register_cosim_bindings() {
#define ARC_COSIM_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_COSIM_MODEL
}
} // namespace chipyard_arc
