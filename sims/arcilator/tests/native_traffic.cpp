// Native TrafficEval bindings with the unchanged Constellation DPI driver.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
using namespace chipyard_arc;
namespace {
constexpr const char *config = "warmup 0\nmeasurement 6\ndrain 32\n"
                               "flits_per_packet 2\nflow 0 0 2.0\n";
Runtime *model;
std::array<unsigned, 2> calls{};
constexpr std::array<const char *, 8> names{{"tx_valid", "tx_head", "tx_tail",
                                             "tx_egress", "tx_unique",
                                             "rx_ready", "success", "fatal"}};
using Outputs = std::array<uint64_t, names.size()>;
Outputs before;
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
Outputs outputs() {
  Outputs result;
  for (unsigned i = 0; i < names.size(); ++i)
    result[i] = model->signal(names[i]).get();
  return result;
}
void phase(const char *instance) {
  check(model == active_runtime() && svGetScope() &&
            std::string(svGetNameFromScope(svGetScope())) ==
                model->scope_name() + "/" + instance,
        "TrafficEval callback scope changed");
  check(outputs() == before,
        "TrafficEval output changed before deferred commit");
  check(model->signal("clock").get() && !model->signal("reset").get(),
        "TrafficEval callback edge/reset gate changed");
}
struct Flit {
  uint64_t cycle, unique, endpoint;
  uint8_t head, tail;
};
struct Harness {
  Runtime &runtime;
  uint64_t time = 0;
  unsigned flits, measurement;
  bool timeout, failure;
  unsigned consumed = 0;
  std::deque<Flit> queued;
  std::vector<Flit> emitted;
  bool step(uint64_t cycle, bool reset = false) {
    auto old = outputs();
    auto count = calls;
    bool deliver = !queued.empty();
    Flit input =
        deliver ? queued.front() : Flit{0, 0xfedcba9876543210ULL, 0, 1, 1};
    runtime.advance(++time, [&] {
      runtime.signal("clock").set(0);
      runtime.signal("reset").set(reset);
      runtime.signal("current_cycle").set(cycle);
      runtime.signal("tx_ready").set(cycle != 1 && cycle != 3);
      runtime.signal("rx_valid").set(deliver || reset);
      runtime.signal("rx_head").set(input.head);
      runtime.signal("rx_tail").set(input.tail);
      runtime.signal("rx_ingress").set(0);
      runtime.signal("rx_unique").set(input.unique);
    });
    check(calls == count && outputs() == old,
          "TrafficEval changed on falling/setup phase");
    before = outputs();
    runtime.advance(++time, [&] { runtime.signal("clock").set(1); });
    check(calls[0] == count[0] + unsigned(!reset) &&
              calls[1] == count[1] + unsigned(!reset),
          "TrafficEval callback counts changed");
    bool terminal = false;
    if (reset) {
      auto value = outputs();
      check(!value[0] && !value[5] && !value[6] && !value[7],
            "TrafficEval reset flags changed");
      for (unsigned i = 1; i < 5; ++i)
        check(value[i] == old[i],
              "TrafficEval reset must retain ingress payload");
    } else {
      check(runtime.signal("rx_ready").get() == 1,
            "original TrafficEval ready changed");
      if (deliver) {
        queued.pop_front();
        ++consumed;
      }
      if (runtime.signal("tx_valid").get()) {
        unsigned id = emitted.size();
        Flit value{cycle, runtime.signal("tx_unique").get(),
                   runtime.signal("tx_egress").get(),
                   uint8_t(runtime.signal("tx_head").get()),
                   uint8_t(runtime.signal("tx_tail").get())};
        check(!timeout && id < 12 && value.unique == id && !value.endpoint &&
                  value.head == (id % flits == 0) &&
                  value.tail == (id % flits == flits - 1),
              "TrafficEval flit identity/head/tail mapping changed");
        emitted.push_back(value);
        queued.push_back(value);
      }
      // The original ingress leaves payload outputs unwritten when invalid.
      // Only valid payloads enter the oracle; all pre-commit values are
      // observed.
      terminal = timeout || (cycle >= measurement && emitted.size() == 12 &&
                             queued.empty());
      check(runtime.signal("success").get() == (terminal && !failure) &&
                runtime.signal("fatal").get() == (terminal && failure),
            "TrafficEval completion success/fatal mapping changed");
    }
    auto settled = outputs();
    auto settled_count = calls;
    runtime.advance(++time);
    check(outputs() == settled && calls == settled_count && !svGetScope(),
          "TrafficEval repeated held-clock callback or leaked scope");
    return terminal;
  }
};
} // namespace
extern "C" {
void __real_ingress_tick(long long, const char *, long long, unsigned char,
                         unsigned char *, unsigned char *, unsigned char *,
                         long long *, long long *);
void __real_egress_tick(long long, const char *, long long, unsigned char *,
                        unsigned char, unsigned char, unsigned char, long long,
                        long long, unsigned char *, unsigned char *);
void __wrap_ingress_tick(long long id, const char *text, long long cycle,
                         unsigned char ready, unsigned char *valid,
                         unsigned char *head, unsigned char *tail,
                         long long *endpoint, long long *unique) {
  phase("ingress");
  check(id == 0 && text && std::string(text) == config &&
            ready == model->signal("tx_ready").get(),
        "TrafficEval ingress ID/config/ready ABI changed");
  check(uint64_t(cycle) == model->signal("current_cycle").get(),
        "TrafficEval 64-bit cycle ABI changed");
  ++calls[0];
  __real_ingress_tick(id, text, cycle, ready, valid, head, tail, endpoint,
                      unique);
  phase("ingress");
}
void __wrap_egress_tick(long long id, const char *text, long long cycle,
                        unsigned char *ready, unsigned char valid,
                        unsigned char head, unsigned char tail,
                        long long ingress, long long unique,
                        unsigned char *success, unsigned char *fatal) {
  phase("egress");
  check(id == 0 && text && std::string(text) == config &&
            valid == model->signal("rx_valid").get() &&
            head == model->signal("rx_head").get() &&
            tail == model->signal("rx_tail").get() &&
            uint64_t(ingress) == model->signal("rx_ingress").get() &&
            uint64_t(unique) == model->signal("rx_unique").get(),
        "TrafficEval egress flit/config ABI changed");
  check(uint64_t(cycle) == model->signal("current_cycle").get(),
        "TrafficEval 64-bit cycle ABI changed");
  ++calls[1];
  __real_egress_tick(id, text, cycle, ready, valid, head, tail, ingress, unique,
                     success, fatal);
  phase("egress");
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 4, "expected mode, owned trace file and parameter file");
    std::string mode = argv[1];
    bool override = mode == "override" || mode == "throughput";
    bool timeout = mode == "timeout", failure = timeout || mode == "throughput";
    check(override || timeout || mode == "default", "unknown TrafficEval mode");
    std::string option = "+eval_params=" + std::string(argv[3]);
    char name[] = "traffic-model-test";
    char *args[] = {name, option.data()};
    auto runtime = create_model(override ? 2 : 1, args);
    model = runtime.get();
    runtime->initialize();
    runtime->advance(0, [&] { runtime->signal("reset").set(1); });
    Harness h{*runtime,           0,       override ? 3u : 2u,
              override ? 4u : 6u, timeout, failure};
    h.step(0, true);
    unsigned cycles = 0;
    bool done = false;
    if (timeout) {
      done = h.step(0x100000001ULL);
      cycles = 1;
    } else
      for (; cycles < 64; ++cycles) {
        done = h.step(cycles);
        if (cycles == 2)
          h.step(cycles + 1, true);
        if (done) {
          ++cycles;
          break;
        }
      }
    check(done && cycles == (timeout ? 1u : 15u) &&
              calls == std::array<unsigned, 2>{{cycles, cycles}} &&
              h.emitted.size() == (timeout ? 0u : 12u) &&
              h.consumed == h.emitted.size() && h.queued.empty(),
          "TrafficEval complete scenario counts changed");
    FILE *file = std::fopen(argv[2], "wb");
    check(file, "cannot open owned TrafficEval trace");
    for (auto value : h.emitted) {
      for (auto word : {value.cycle, value.unique, value.endpoint})
        for (unsigned i = 0; i < 8; ++i)
          check(std::fputc(uint8_t(word >> (i * 8)), file) != EOF,
                "TrafficEval trace write failed");
      check(std::fputc(value.head, file) != EOF &&
                std::fputc(value.tail, file) != EOF,
            "TrafficEval trace flag write failed");
    }
    check(!std::fclose(file), "TrafficEval trace close failed");
    std::printf("PASS original TrafficEval: mode=%s cycles=%u flits=%zu "
                "success=%u fatal=%u\n",
                mode.c_str(), cycles, h.emitted.size(), unsigned(!failure),
                unsigned(failure));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
