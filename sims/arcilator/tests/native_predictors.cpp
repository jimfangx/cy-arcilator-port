// Link the original BOOM predictor_sw.cc and btb_sw.cc. Wrappers observe the
// native ABI and callback phase, then call each unchanged implementation.
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
bool initializing;
unsigned bp_initializations, btb_initializations;
std::vector<char> bp_calls, btb_calls;
constexpr std::array<const char *, 5> output_names{
    {"bp_req_taken", "btb_req_target_valid", "btb_req_target_pc",
     "btb_req_is_br", "btb_req_is_jal"}};
std::array<uint64_t, 5> before;
constexpr uint64_t pc_a = 0x8123456789abc0a5ULL;
constexpr uint64_t pc_b = 0xfedcba98765410a5ULL; // Same bimodal table entry.
constexpr uint64_t pc_c = 0x80000000000000c7ULL;
constexpr uint64_t target_a = 0xfedcba9876543210ULL;
constexpr uint64_t target_c = 0xdeadbeef01234568ULL;
constexpr uint64_t target_d = 0xfffffff000001234ULL;
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::array<uint64_t, 5> outputs() {
  std::array<uint64_t, 5> values;
  for (unsigned i = 0; i < values.size(); ++i)
    values[i] = model->signal(output_names[i]).get();
  return values;
}
void phase(const char *instance) {
  check(active_runtime() == model && svGetScope(),
        "predictor lost native context");
  check(std::string(svGetNameFromScope(svGetScope())) ==
            model->scope_name() + "/" + instance,
        "predictor callback used the wrong DPI scope");
  if (!initializing)
    check(outputs() == before,
          "predictor outputs committed before sampling ended");
}
void arguments(const char *instance, const char *prefix, bool update,
               unsigned long long pc, unsigned long long history) {
  phase(instance);
  std::string root = std::string(prefix) + (update ? "_update_" : "_req_");
  check(model->signal(root + "valid").get(), "disabled predictor callback ran");
  check(pc == model->signal(root + "pc").get() &&
            history == model->signal(root + "hist").get(),
        "predictor 64-bit PC/history ABI changed");
}
void evaluate(void *) {}
struct Probe {
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 256, "PredictorProbe",
                           nullptr};
#ifdef ARC_PREDICTOR_COMPILED_MODEL
  std::unique_ptr<Runtime> owner = create_model(0, nullptr);
#else
  std::unique_ptr<Runtime> owner =
      std::make_unique<Runtime>(0, nullptr, info, evaluate);
#endif
  Runtime &runtime = *owner;
  unsigned offset = 24;
  uint64_t time = 0;
  Probe() {
    model = &runtime;
    initializing = true;
    bp_initializations = btb_initializations = 0;
#ifndef ARC_PREDICTOR_COMPILED_MODEL
    signal("clock", 1);
    signal("reset", 1);
    add("BranchPredictorHarness", "bp",
        {{"req_valid", 1},
         {"req_pc", 64},
         {"req_hist", 64},
         {"req_taken", 1},
         {"update_valid", 1},
         {"update_pc", 64},
         {"update_hist", 64},
         {"update_taken", 1}});
    add("BTBHarness", "btb",
        {{"req_valid", 1},
         {"req_pc", 64},
         {"req_hist", 64},
         {"req_target_valid", 1},
         {"req_target_pc", 64},
         {"req_is_br", 1},
         {"req_is_jal", 1},
         {"update_valid", 1},
         {"update_pc", 64},
         {"update_hist", 64},
         {"update_target", 64},
         {"update_is_br", 1},
         {"update_is_jal", 1}});
#endif
    runtime.initialize();
    initializing = false;
    check(bp_initializations == 1 && btb_initializations == 1,
          "predictors were not initialized once per instance");
    check(!svGetScope(), "predictor initialization leaked its DPI scope");
  }
  void signal(const std::string &name, unsigned bits) {
    runtime.add_signal(name, offset, bits, false);
    runtime.signal(name).set(0);
    offset += (bits + 7) / 8;
  }
  void add(const char *module, const char *prefix,
           std::initializer_list<std::pair<const char *, unsigned>> ports) {
    auto b = make_binding(runtime, module);
    check(bool(b), "native predictor factory missing");
    b->instance_name = prefix;
    b->ports["clock"] = runtime.signal("clock");
    b->ports["reset"] = runtime.signal("reset");
    for (auto port : ports) {
      std::string name = std::string(prefix) + "_" + port.first;
      signal(name, port.second);
      b->ports[port.first] = runtime.signal(name);
    }
    runtime.add_binding(std::move(b));
  }
  void set(const char *name, uint64_t value) {
    runtime.signal(name).set(value);
  }
  // Expected learning results are hand-written from the original 2-bit
  // counter and BTB contracts, independent of the native binding code.
  void edge(bool req, bool update, bool taken, bool bt_update,
            bool expected_taken, bool expected_valid, uint64_t target = 0,
            bool branch = false, bool jump = false, bool reset = false,
            uint64_t request_pc = pc_a, uint64_t update_pc = pc_a) {
    bp_calls.clear();
    btb_calls.clear();
    runtime.advance(++time, [&] {
      set("clock", 0);
      set("reset", reset);
      for (auto prefix : {"bp", "btb"}) {
        auto name = [&](const char *port) {
          return std::string(prefix) + "_" + port;
        };
        runtime.signal(name("req_valid")).set(req);
        runtime.signal(name("req_pc")).set(request_pc);
        runtime.signal(name("req_hist")).set(0xfedcba9876543210ULL);
        runtime.signal(name("update_pc")).set(update_pc);
        runtime.signal(name("update_hist")).set(0x8123456789abcdefULL);
      }
      set("bp_update_valid", update);
      set("bp_update_taken", taken);
      set("btb_update_valid", bt_update);
      set("btb_update_target", target);
      set("btb_update_is_br", branch);
      set("btb_update_is_jal", jump);
    });
    check(bp_calls.empty() && btb_calls.empty(),
          "predictor ran on falling/stable clock");
    before = outputs();
    runtime.advance(++time, [&] { set("clock", 1); });
    std::vector<char> bp_expected, btb_expected;
    if (!reset) {
      if (req) {
        bp_expected.push_back('p');
        btb_expected.push_back('p');
      }
      if (update)
        bp_expected.push_back('u');
      if (bt_update)
        btb_expected.push_back('u');
    }
    check(bp_calls == bp_expected && btb_calls == btb_expected,
          "predict/update callback order or enable/reset count changed");
    auto value = outputs();
    if (reset) {
      check(value == std::array<uint64_t, 5>{},
            "predictor reset outputs changed");
    } else if (!req) {
      check(value == before,
            "disabled prediction failed to retain all outputs");
    } else {
      check(value[0] == expected_taken,
            "original bimodal learning result changed");
      check(value[1] == expected_valid, "original BTB hit/miss result changed");
      // The original C driver writes hit payloads. Miss payloads are unwritten;
      // their pre-commit bytes are still checked by every callback observer.
      if (expected_valid)
        check(value[2] == target && value[3] == branch && value[4] == jump,
              "original BTB target/branch/jump result changed");
    }
    bp_calls.clear();
    btb_calls.clear();
    runtime.advance(
        ++time); // Re-evaluation at a held high clock is not an edge.
    check(bp_calls.empty() && btb_calls.empty(),
          "predictor repeated a callback without an edge");
    check(!svGetScope() && bp_initializations == 1 && btb_initializations == 1,
          "predictor scope restoration or reset reinitialized its driver");
  }
};
void run() {
  Probe p;
  p.edge(true, true, true, true, false, false, target_a, true, false, true);
  p.edge(true, false, false, false, false, false);
  p.edge(true, true, true, true, false, false, target_a, true, false);
  p.edge(true, true, true, true, false, true, target_a, true, false);
  p.edge(true, false, false, false, true, true, target_a, true, false);
  p.edge(false, true, false, true, true, true, target_c, false, true);
  p.edge(true, false, false, false, false, true, target_c, false, true);
  p.edge(true, true, true, true, false, false, target_d, true, true, false,
         pc_b, pc_c);
  // The prior step updated a different BP entry and a different BTB address.
  p.edge(true, false, false, false, false, true, target_d, true, true, false,
         pc_c);
  p.edge(true, false, false, false, false, true, target_c, false, true);
  p.edge(true, true, false, true, false, false, target_d, true, true, true);
  p.edge(true, false, false, false, false, true, target_c, false, true);
  for (bool expected : {false, true, true})
    p.edge(true, true, true, false, expected, true, target_c, false, true);
  for (bool expected : {true, true, false, false})
    p.edge(true, true, false, false, expected, true, target_c, false, true);
  p.edge(true, false, false, false, false, true, target_c, false, true);
}
} // namespace

extern "C" {
void __real_initialize_branch_predictor();
void __real_predict_branch(unsigned long long, unsigned long long,
                           unsigned char *);
void __real_update_branch(unsigned long long, unsigned long long,
                          unsigned char);
void __real_initialize_btb();
void __real_predict_target(unsigned long long, unsigned long long,
                           unsigned char *, unsigned long long *,
                           unsigned char *, unsigned char *);
void __real_update_btb(unsigned long long, unsigned long long,
                       unsigned long long, unsigned char, unsigned char);
void __wrap_initialize_branch_predictor() {
  phase("bp");
  ++bp_initializations;
  __real_initialize_branch_predictor();
}
void __wrap_initialize_btb() {
  phase("btb");
  ++btb_initializations;
  __real_initialize_btb();
}
void __wrap_predict_branch(unsigned long long pc, unsigned long long history,
                           unsigned char *taken) {
  arguments("bp", "bp", false, pc, history);
  bp_calls.push_back('p');
  __real_predict_branch(pc, history, taken);
  phase("bp");
}
void __wrap_update_branch(unsigned long long pc, unsigned long long history,
                          unsigned char taken) {
  arguments("bp", "bp", true, pc, history);
  check(taken == model->signal("bp_update_taken").get(),
        "branch update bit ABI changed");
  bp_calls.push_back('u');
  __real_update_branch(pc, history, taken);
  phase("bp");
}
void __wrap_predict_target(unsigned long long pc, unsigned long long history,
                           unsigned char *valid, unsigned long long *target,
                           unsigned char *branch, unsigned char *jump) {
  arguments("btb", "btb", false, pc, history);
  btb_calls.push_back('p');
  __real_predict_target(pc, history, valid, target, branch, jump);
  phase("btb");
}
void __wrap_update_btb(unsigned long long pc, unsigned long long history,
                       unsigned long long target, unsigned char branch,
                       unsigned char jump) {
  arguments("btb", "btb", true, pc, history);
  check(target == model->signal("btb_update_target").get() &&
            branch == model->signal("btb_update_is_br").get() &&
            jump == model->signal("btb_update_is_jal").get(),
        "BTB update target/branch/jump ABI changed");
  btb_calls.push_back('u');
  __real_update_btb(pc, history, target, branch, jump);
  phase("btb");
}
}
int main() {
  try {
    register_builtin_bindings();
    for (unsigned recreation = 0; recreation < 2; ++recreation)
      run();
    std::puts("PASS original BOOM predictors: 64-bit ABI, call order, "
              "learning, reset, scopes, deferred outputs, recreations=2");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
