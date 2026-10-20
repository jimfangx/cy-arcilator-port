#include "runtime.h"

extern "C" {
int debug_tick(unsigned char *, unsigned char, int *, int *, int *, unsigned char, unsigned char *, int, int) __attribute__((weak));
void block_device_init(const char *, int, int *, int *) __attribute__((weak));
void block_device_tick(unsigned char, unsigned char *, unsigned char, int, int, int,
                       unsigned char, unsigned char *, long long, int,
                       unsigned char *, unsigned char, long long *, int *) __attribute__((weak));
void network_init(const char *, int, char *, char *) __attribute__((weak));
void network_tick(unsigned char, long long, unsigned char, unsigned char *, long long *, unsigned char *, long long *) __attribute__((weak));
void initialize_branch_predictor() __attribute__((weak));
void predict_branch(unsigned long long, unsigned long long, unsigned char *) __attribute__((weak));
void update_branch(unsigned long long, unsigned long long, unsigned char) __attribute__((weak));
void initialize_btb() __attribute__((weak));
void predict_target(unsigned long long, unsigned long long, unsigned char *, unsigned long long *, unsigned char *, unsigned char *) __attribute__((weak));
void update_btb(unsigned long long, unsigned long long, unsigned long long, unsigned char, unsigned char) __attribute__((weak));
int trace_rom_init(const char *) __attribute__((weak));
void trace_rom_tick(char *, char, long *, char *, char *) __attribute__((weak));
}

namespace chipyard_arc {
namespace {
class DTM : public Binding {
  bool previous_reset = false;
  unsigned char valid = 0, ready = 0;
  int address = 0, operation = 0, data = 0, exit = 0;
public:
  using Binding::Binding;
  void initialize() override { if (!debug_tick) throw std::runtime_error("SimDTM driver is not linked"); }
  void sample() override {
    bool clock = get("clk");
    if (!clock && old_clock) {
      bool reset = get("reset");
      if (reset || previous_reset) { valid = ready = 0; exit = 0; }
      else exit = debug_tick(&valid, get("debug_req_ready"), &address, &operation, &data,
                             get("debug_resp_valid"), &ready, get("debug_resp_bits_resp"), get("debug_resp_bits_data"));
      previous_reset = reset;
    }
    if (clock && !old_clock) {
      stage("debug_req_valid", valid); stage("debug_req_bits_addr", uint32_t(address));
      stage("debug_req_bits_op", uint32_t(operation)); stage("debug_req_bits_data", uint32_t(data));
      stage("debug_resp_ready", ready); stage("exit", uint32_t(exit));
    }
    old_clock = clock;
  }
};
class BlockDevice : public Binding {
  unsigned char request_ready = 0, data_ready = 0, response_valid = 0;
  long long data = 0;
  int tag = 0;
public:
  using Binding::Binding;
  void initialize() override {
    if (!block_device_init || !block_device_tick) throw std::runtime_error("SimBlockDevice driver is not linked");
    auto path = runtime.option("blkdev=");
    if (!path.empty()) {
      int sectors = 0, length = 0;
      auto tag_bits = number("TAG_BITS", 1);
      if (tag_bits >= 31) throw std::runtime_error("invalid native block-device tag width");
      block_device_init(path.c_str(), 1u << tag_bits, &sectors, &length);
      port("bdev_info_nsectors").set(uint32_t(sectors)); port("bdev_info_max_req_len").set(uint32_t(length));
    }
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock; old_clock = clock;
    if (!edge) return;
    if (get("reset")) { request_ready = data_ready = response_valid = 0; data = tag = 0; }
    else block_device_tick(get("bdev_req_valid"), &request_ready, get("bdev_req_bits_write"),
        get("bdev_req_bits_offset"), get("bdev_req_bits_len"), get("bdev_req_bits_tag"),
        get("bdev_data_valid"), &data_ready, get("bdev_data_bits_data"), get("bdev_data_bits_tag"),
        &response_valid, get("bdev_resp_ready"), &data, &tag);
    stage("bdev_req_ready", request_ready); stage("bdev_data_ready", data_ready);
    stage("bdev_resp_valid", response_valid); stage("bdev_resp_bits_data", data); stage("bdev_resp_bits_tag", uint32_t(tag));
  }
};
class Network : public Binding {
  unsigned char valid = 0, last = 0;
  long long data = 0, mac = 0;
public:
  using Binding::Binding;
  void initialize() override {
    if (!network_init || !network_tick) throw std::runtime_error("SimNetwork driver is not linked");
    char increment = 1, period = 1;
    network_init(runtime.option("netdev=").c_str(), std::stoi(runtime.option("netbw=", "64")), &increment, &period);
    port("net_rlimit_inc").set(uint8_t(increment)); port("net_rlimit_period").set(uint8_t(period));
    port("net_rlimit_size").set(std::stoull(runtime.option("netburst=", "8")));
    port("net_in_bits_keep").set(255);
    for (auto p : {"net_pauser_threshold", "net_pauser_quanta", "net_pauser_refresh"}) port(p).set(255);
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock; old_clock = clock;
    if (!edge) return;
    if (get("reset")) { valid = last = 0; data = 0; }
    else {
      network_tick(get("net_out_valid"), get("net_out_bits_data"), get("net_out_bits_last"), &valid, &data, &last, &mac);
      stage("net_macAddr", uint64_t(mac));
    }
    stage("net_in_valid", valid); stage("net_in_bits_data", uint64_t(data)); stage("net_in_bits_last", last);
  }
};
class BranchPredictor : public Binding {
  unsigned char taken = 0;
public:
  using Binding::Binding;
  void initialize() override {
    if (!initialize_branch_predictor || !predict_branch || !update_branch) throw std::runtime_error("software branch predictor is not linked");
    initialize_branch_predictor();
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock; old_clock = clock;
    if (!edge) return;
    if (get("reset")) taken = 0;
    else {
      if (get("req_valid")) predict_branch(get("req_pc"), get("req_hist"), &taken);
      if (get("update_valid")) update_branch(get("update_pc"), get("update_hist"), get("update_taken"));
    }
    stage("req_taken", taken);
  }
};
class BTB : public Binding {
  unsigned char valid = 0, branch = 0, jump = 0;
  unsigned long long target = 0;
public:
  using Binding::Binding;
  void initialize() override {
    if (!initialize_btb || !predict_target || !update_btb) throw std::runtime_error("software BTB driver is not linked");
    initialize_btb();
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock; old_clock = clock;
    if (!edge) return;
    if (get("reset")) { valid = branch = jump = 0; target = 0; }
    else {
      if (get("req_valid")) predict_target(get("req_pc"), get("req_hist"), &valid, &target, &branch, &jump);
      if (get("update_valid")) update_btb(get("update_pc"), get("update_hist"), get("update_target"), get("update_is_br"), get("update_is_jal"));
    }
    stage("req_target_valid", valid); stage("req_target_pc", target); stage("req_is_br", branch); stage("req_is_jal", jump);
  }
};
class TraceROM : public Binding {
  char valid = 0, keep = 0, last = 0;
  long data = 0;
public:
  using Binding::Binding;
  void initialize() override {
    if (!trace_rom_init || !trace_rom_tick) throw std::runtime_error("TraceROM driver is not linked");
    auto path = runtime.option("trace=");
    if (!path.empty()) port("length").set(uint32_t(trace_rom_init(path.c_str())));
    auto mac = runtime.option("macaddr=");
    if (!mac.empty()) port("macAddr").set(std::stoull(mac, nullptr, 16));
  }
  void sample() override {
    bool clock = get("clock"), edge = clock && !old_clock; old_clock = clock;
    if (!edge) return;
    if (get("reset")) { valid = keep = last = 0; data = 0; }
    else trace_rom_tick(&valid, get("stream_ready"), &data, &keep, &last);
    stage("stream_valid", uint8_t(valid)); stage("stream_bits_data", uint64_t(data));
    stage("stream_bits_keep", uint8_t(keep)); stage("stream_bits_last", uint8_t(last));
  }
};
template <class T> void add(const char *name) {
  register_binding(name, [](Runtime &r, const std::string &n) { return std::make_unique<T>(r, n); });
}
}
void register_peripheral_bindings() {
#define ARC_PERIPHERAL_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_PERIPHERAL_MODEL
}
}
