// Original SimNetwork.cc/device.cc/switch.cc, with an empty TAP device name.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace chipyard_arc;
namespace {
Runtime *model;
bool initializing, have_mac;
unsigned initializes, ticks;
int bandwidth;
uint64_t observed_mac;
std::array<uint64_t, 11> before;
constexpr std::array<const char *, 11> names{
    {"net_in_valid", "net_in_bits_data", "net_in_bits_keep", "net_in_bits_last",
     "net_macAddr", "net_rlimit_inc", "net_rlimit_period", "net_rlimit_size",
     "net_pauser_threshold", "net_pauser_quanta", "net_pauser_refresh"}};
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::array<uint64_t, 11> outputs() {
  std::array<uint64_t, 11> result;
  for (unsigned i = 0; i < result.size(); ++i)
    result[i] = model->signal(names[i]).get();
  return result;
}
void phase() {
  check(model == active_runtime() && svGetScope() &&
            std::string(svGetNameFromScope(svGetScope())) ==
                model->scope_name() + "/nic",
        "network callback scope changed");
  if (!initializing)
    check(outputs() == before,
          "network outputs changed before callback sampling ended");
}
struct Flit {
  uint64_t data;
  bool last;
};
struct Harness {
  Runtime &runtime;
  uint64_t time = 0;
  bool override_args;
  std::vector<Flit> expected, received;
  void constants() {
    check(
        runtime.signal("net_in_bits_keep").get() == 255 &&
            runtime.signal("net_rlimit_inc").get() == (override_args ? 5 : 1) &&
            runtime.signal("net_rlimit_period").get() ==
                (override_args ? 31 : 0) &&
            runtime.signal("net_rlimit_size").get() == (override_args ? 1 : 8),
        "original network rate/default/byte metadata changed");
    for (auto name :
         {"net_pauser_threshold", "net_pauser_quanta", "net_pauser_refresh"})
      check(runtime.signal(name).get() == 255,
            "network pauser constant changed");
  }
  void step(bool valid = false, uint64_t word = 0xfedcba9876543210ULL,
            bool last = true, bool reset = false) {
    auto old = outputs();
    unsigned count = ticks;
    runtime.advance(++time, [&] {
      runtime.signal("clock").set(0);
      runtime.signal("reset").set(reset);
      runtime.signal("net_out_valid").set(valid);
      runtime.signal("net_out_bits_data").set(word);
      runtime.signal("net_out_bits_last").set(last);
      // The original wrapper intentionally ignores TX keep.
      runtime.signal("net_out_bits_keep").set(0xa5);
    });
    check(ticks == count && outputs() == old,
          "network changed on falling clock");
    before = outputs();
    runtime.advance(++time, [&] { runtime.signal("clock").set(1); });
    check(ticks == count + unsigned(!reset),
          "network reset/edge callback count changed");
    constants();
    if (reset) {
      check(!runtime.signal("net_in_valid").get() &&
                !runtime.signal("net_in_bits_data").get() &&
                !runtime.signal("net_in_bits_last").get() &&
                runtime.signal("net_macAddr").get() == before[4],
            "network reset data or retained MAC changed");
    } else {
      check(have_mac && runtime.signal("net_macAddr").get() == observed_mac,
            "native 48-bit MAC mapping changed");
      if (runtime.signal("net_in_valid").get()) {
        Flit flit{runtime.signal("net_in_bits_data").get(),
                  bool(runtime.signal("net_in_bits_last").get())};
        check(received.size() < expected.size(),
              "network emitted an unexpected flit");
        auto value = expected[received.size()];
        check(flit.data == value.data && flit.last == value.last,
              "original network loopback packet data/last changed");
        received.push_back(flit);
      } else {
        // The original accessors explicitly return zero/false when empty.
        check(!runtime.signal("net_in_bits_data").get() &&
                  !runtime.signal("net_in_bits_last").get(),
              "original empty network output changed");
      }
    }
    auto current = outputs();
    runtime.advance(++time);
    check(ticks == count + unsigned(!reset) && outputs() == current,
          "network repeated a held-clock callback");
    check(initializes == 1 && !svGetScope(),
          "network reinitialized or leaked scope");
  }
};
} // namespace
extern "C" {
void __real_network_init(const char *, int, char *, char *);
void __real_network_tick(unsigned char, long long, unsigned char,
                         unsigned char *, long long *, unsigned char *,
                         long long *);
void __wrap_network_init(const char *devname, int rate, char *increment,
                         char *period) {
  phase();
  check(initializing && ++initializes == 1 && devname && !*devname &&
            rate == bandwidth,
        "network initialization/device/rate ABI changed");
  // The empty name is checked before the real constructor: it never opens TAP.
  __real_network_init(devname, rate, increment, period);
  check(uint8_t(*increment) == (rate == 10 ? 5 : 1) &&
            uint8_t(*period) == (rate == 10 ? 31 : 0),
        "original network rate arithmetic changed");
  phase();
}
void __wrap_network_tick(unsigned char valid, long long data,
                         unsigned char last, unsigned char *in_valid,
                         long long *in_data, unsigned char *in_last,
                         long long *mac) {
  phase();
  check(!initializing && model->signal("clock").get() &&
            !model->signal("reset").get(),
        "network driver ran during reset or wrong edge");
  check(valid == model->signal("net_out_valid").get() &&
            uint64_t(data) == model->signal("net_out_bits_data").get() &&
            last == model->signal("net_out_bits_last").get(),
        "network 64-bit TX/valid/last ABI changed");
  ++ticks;
  __real_network_tick(valid, data, last, in_valid, in_data, in_last, mac);
  check(uint64_t(*mac) < (uint64_t(1) << 48),
        "original MAC exceeds its source width");
  if (have_mac)
    check(uint64_t(*mac) == observed_mac,
          "original MAC changed after initialization");
  observed_mac = uint64_t(*mac);
  have_mac = true;
  phase();
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 3 && (std::string(argv[1]) == "default" ||
                        std::string(argv[1]) == "override"),
          "expected default/override mode and trace filename");
    bool override_args = std::string(argv[1]) == "override";
    bandwidth = override_args ? 10 : 64;
    char name[] = "network-model-test", rate[] = "+netbw=10",
         burst[] = "+netburst=257";
    char *args[] = {name, rate, burst};
    auto runtime = create_model(override_args ? 3 : 1, args);
    model = runtime.get();
    initializing = true;
    runtime->initialize();
    initializing = false;
    runtime->advance(0, [&] { runtime->signal("reset").set(1); });
    Harness h{*runtime, 0, override_args};
    h.step(true, 0xdeadbeefcafef00dULL, true, true);
    h.step(); // Obtain the original driver's random MAC without changing it.
    uint64_t mac = observed_mac;
    std::vector<uint64_t> a(8), b(11);
    a[0] = (mac << 16) | 0x39a7;
    a[1] = mac | (uint64_t(0xb588) << 48);
    b[0] = (mac << 16) | 0xa97c;
    b[1] = mac | (uint64_t(0x0008) << 48);
    for (unsigned i = 2; i < a.size(); ++i)
      a[i] = 0xfedcba9800000000ULL ^ (uint64_t(i) * 0x0102040810204081ULL);
    for (unsigned i = 2; i < b.size(); ++i)
      b[i] = 0x8123456700000000ULL ^ (uint64_t(i + 17) * 0x0011223344556677ULL);
    for (auto *packet : {&a, &b})
      for (unsigned i = 0; i < packet->size(); ++i)
        h.expected.push_back({(*packet)[i], i + 1 == packet->size()});
    bool tx_reset = false, rx_reset = false;
    for (auto *packet : {&a, &b}) {
      for (unsigned i = 0; i < packet->size(); ++i) {
        h.step(true, (*packet)[i], i + 1 == packet->size());
        if (!tx_reset && packet == &a && i == 1) {
          h.step(true, 0xdeadbeefcafef00dULL, true, true);
          tx_reset = true;
        }
        if (!rx_reset && h.received.size() == 2) {
          h.step(true, 0x123456789abcdef0ULL, true, true);
          rx_reset = true;
        }
      }
    }
    for (unsigned i = 0; i < 64 && h.received.size() != h.expected.size(); ++i)
      h.step();
    check(tx_reset && rx_reset && h.received.size() == 19,
          "network did not complete both packet/reset cases");
    for (unsigned i = 0; i < 3; ++i)
      h.step();
    FILE *trace = std::fopen(argv[2], "wb");
    check(trace, "cannot open owned network trace");
    for (auto flit : h.received) {
      uint8_t last = flit.last;
      check(std::fwrite(&flit.data, 8, 1, trace) == 1 &&
                std::fwrite(&last, 1, 1, trace) == 1,
            "network trace write failed");
    }
    check(!std::fclose(trace), "network trace close failed");
    std::printf("PASS original network loopback: mode=%s mac=%012llx packets=2 "
                "flits=19\n",
                argv[1], (unsigned long long)mac);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
