#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
using namespace chipyard_arc;
extern "C" void Test_eval(void *);
static_assert(sizeof(Binding) == 224);
static_assert(sizeof(Signal) == 16);
static_assert(sizeof(Runtime) == 1544);
extern "C" void Test_stable_eval(void *);
extern "C" uint32_t Test_idle_clock(void *);
extern "C" uint32_t Test_stable_combinational(void *);
extern "C" void __real_arcRuntimeOnEval(ArcState *);
static unsigned evaluations, hooks, attempts, accepted, queries, query_accepted;
static unsigned fast_evaluations, sdk_mode, sdk_mutation_fallbacks;
static bool must_use_full;
extern "C" void __wrap_arcRuntimeOnEval(ArcState *state) {
  ++hooks;
  __real_arcRuntimeOnEval(state);
  if (hooks == 1 && sdk_mode == 16) {
    state->modelState[28] ^= 1;
    must_use_full = true;
  }
  if (hooks == 1 && sdk_mode == 17)
    state->modelState[25] ^= 85;
}
static void evaluate(void *state) {
  ++evaluations;
  if (must_use_full) {
    ++sdk_mutation_fallbacks;
    must_use_full = false;
  }
  Test_eval(state);
}
static void fast_eval(void *state) {
  if (must_use_full)
    throw std::runtime_error("fast evaluator used a stale pre-SDK certificate");
  ++evaluations;
  ++fast_evaluations;
  Test_stable_eval(state);
}
static uint32_t idle(void *state) {
  ++attempts;
  auto result = Test_idle_clock(state);
  accepted += result != 0;
  return result;
}
static uint32_t stable_query(void *state) {
  ++queries;
  auto result = Test_stable_combinational(state);
  query_accepted += result != 0;
  return result;
}
struct Record {
  std::vector<uint64_t> callbacks;
  std::vector<uint8_t> states;
  unsigned fast_evaluations, sdk_mutation_fallbacks;
  unsigned evaluations, hooks, attempts, accepted, queries, query_accepted;
};
struct Probe : Binding {
  unsigned mode;
  std::vector<uint64_t> &records;
  bool old = false, combo_pending = false, unreported_pending = false;
  uint64_t analog_value = 0;
  Probe(Runtime &r, unsigned mode, std::vector<uint64_t> &records)
      : Binding(r, "Probe"), mode(mode), records(records) {}
  bool
  output_convergence_safe() const override { // Observers that write ordinary
                                             // inputs use the opt-out contract.
    return mode != 5 && mode != 17 && mode != 19;
  }
  void record(unsigned phase) {
    records.push_back(runtime.signal("echo").get());
    records.push_back((uint64_t(phase) << 56) |
                      (runtime.signal("physical").get() << 48) |
                      (runtime.signal("held").get() << 40) |
                      (runtime.signal("y").get() << 32) |
                      (runtime.signal("data").get() << 24) |
                      (runtime.signal("reset").get() << 16) | runtime.time());
  }
  bool combinational() override {
    record(1);
    if (mode == 4) {
      runtime.drive_analog(this, "net", runtime.signal("net"), analog_value,
                           255);
      return runtime.signal("data").set(runtime.signal("net").get());
    }
    if (!combo_pending)
      return false;
    combo_pending = false;
    return runtime.signal("data").set((runtime.signal("data").get() + 17) &
                                      255);
  }
  void sample() override {
    record(
        2); // This callback deliberately has observable effects on every call.
    bool clock = runtime.signal("physical").get();
    if (old && !clock) {
      if (mode == 1) {
        auto s = runtime.signal("data");
        uint8_t fresh = (s.get() + 29) & 255;
        pending.push_back({s, {fresh}});
      }
      if (mode == 2)
        combo_pending = true;
      if (mode == 3)
        runtime.signal("data").set(239);
      if (mode == 6)
        unreported_pending = true;
      if (mode == 4)
        analog_value = (analog_value + 31) & 255;
    }
    old = clock;
  }
  bool commit() override {
    record(3);
    if (unreported_pending) {
      unreported_pending = false;
      runtime.signal("data").set(141);
      return false;
    }
    return Binding::commit();
  }
};
static Record trial(unsigned optimized, unsigned mode, bool filtered,
                    const char *vcd) {
  Record result;
  char arg0[] = "idle-test";
  std::string wave = std::string("+arc-vcdfile=") + vcd;
  char optout[] = "+arc-no-idle-clocks";
  char full_snapshots[] = "+arc-full-state-snapshots";
  char no_fast[] = "+arc-no-stable-evaluator";
  char no_schedule[] = "+arc-no-callback-schedule";
  char *argv[] = {arg0, wave.data(),
                  mode == 12   ? full_snapshots
                  : mode == 15 ? no_fast
                               : optout,
                  no_schedule};
  int argc = mode == 10 || mode == 12 || mode == 15 ? 3 : 2;
  if (!optimized)
    argv[argc++] = no_schedule;
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "IdleClock", nullptr};
  evaluations = hooks = attempts = accepted = queries = query_accepted = 0;
  fast_evaluations = sdk_mutation_fallbacks = 0;
  sdk_mode = mode;
  must_use_full = false;
  {
    Runtime runtime(argc, argv, info, evaluate);
    runtime.add_signal("held", 24, 1, false);
    runtime.add_signal("data", 25, 8, false);
    runtime.add_signal("y", 26, 8, true);
    runtime.add_signal("physical", 31, 1, false);
    runtime.add_signal("observed", 32, 1, true);
    runtime.add_signal("reset", 33, 1, false);
    runtime.add_signal("echo", 34, 8, true);
    runtime.add_clock("held", "observed");
    if (filtered) {
      runtime.add_write_only_output("y");
      runtime.add_write_only_output("observed");
      runtime.add_write_only_output("echo");
    }
    runtime.add_trace_signal("y", 26, 8);
    runtime.add_trace_signal("clock", 31, 1);
    runtime.add_analog_signal("net", 8);
    runtime.add_binding(
        make_binding_instance<Probe>(runtime, mode, result.callbacks));
    if (optimized)
      runtime.add_idle_clock_model(idle, {"held"},
                                   optimized >= 2 ? stable_query : nullptr,
                                   optimized == 3 ? fast_eval : nullptr);
    if (mode == 7)
      runtime.add_export("scope", "kernel", [](uint8_t **, uint64_t *) {}, 0, 0,
                         {}, {}, {});
    if (mode == 8)
      runtime.add_signal("late", 35, 1, false);
    if (mode == 9) {
      runtime.add_signal("sampling", 35, 8, false);
      runtime.add_sampling_signal("sampling");
    }
    runtime.initialize();
    if (mode == 18)
      runtime.add_signal("late_after_init", 35, 1, false);
    auto saved_held = runtime.signal("held");
    auto saved_data = runtime.signal("data");
    auto save_state = [&] {
      if (runtime.signal("held").data != saved_held.data ||
          runtime.signal("data").data != saved_data.data)
        throw std::runtime_error("cached Signal address changed");
      auto *base = saved_held.data - 24;
      result.states.insert(result.states.end(), base, base + 64);
    };
    // Raw noncanonical i1 bytes are representation probes. Their oracle is
    // exact behavior of the full evaluator; canonical inputs also use the
    // independent logical register model below.
    uint64_t expected = 0;
    for (unsigned step = 0; step < 64; ++step) {
      runtime.advance(step * 1000, [&] {
        runtime.signal("physical").set(0);
        if (mode == 13)
          runtime.signal("physical").data[0] = 128;
        if (mode == 14)
          runtime.signal("held").data[0] = 128;
        if (mode == 11)
          runtime.signal("held").set(0);
        runtime.signal("reset").set(step == 0 || step % 13 == 0);
        runtime.signal("data").set((step * 37 + 9) & 255);
      });
      save_state();
      if (mode < 13 && runtime.signal("y").get() != expected)
        throw std::runtime_error("register changed on an inactive edge/reset");
      expected =
          runtime.signal("reset").get() ? 90 : runtime.signal("data").get();
      runtime.advance(step * 1000 + 500, [&] {
        runtime.signal("physical").set(1);
        if (mode == 13)
          runtime.signal("physical").data[0] = 129;
      });
      save_state();
      if (mode < 13 && runtime.signal("y").get() != expected)
        throw std::runtime_error(
            "register did not sample settled pre-edge inputs");
      runtime.advance(step * 1000 + 750,
                      [&] { runtime.signal("data").set(step ^ 255); });
      save_state();
      if (mode < 13 && runtime.signal("y").get() != expected)
        throw std::runtime_error("data-only transition changed a register");
    }
  }
  result.fast_evaluations = fast_evaluations;
  result.sdk_mutation_fallbacks = sdk_mutation_fallbacks;
  result.evaluations = evaluations;
  result.hooks = hooks;
  result.attempts = attempts;
  result.accepted = accepted;
  result.queries = queries;
  result.query_accepted = query_accepted;
  return result;
}
static std::string bytes(const char *path) {
  std::ifstream f(path);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
int main() {
  for (unsigned recreation = 0; recreation < 2; ++recreation)
    for (unsigned mode = 0; mode < 20; ++mode)
      for (bool filtered : {false, true}) {
        auto control = trial(0, mode, filtered, "control.vcd");
        auto idle_only = trial(1, mode, filtered, "idle-only.vcd");
        auto candidate = trial(3, mode, filtered, "candidate.vcd");
        auto snapshot_only = trial(2, mode, filtered, "snapshot-only.vcd");
        bool fallback = mode == 5 || (mode >= 7 && mode <= 10) || mode == 17 ||
                        mode == 18 || mode == 19;
        bool bad_idle = mode >= 13 ? false
                        : fallback
                            ? (candidate.attempts || candidate.accepted ||
                               candidate.evaluations != control.evaluations)
                        : (mode == 11 || mode == 13 || mode == 14)
                            ? (!candidate.attempts || candidate.accepted ||
                               candidate.evaluations != control.evaluations)
                            : (!candidate.attempts || !candidate.accepted ||
                               candidate.evaluations >= control.evaluations);
        bool query_enabled = filtered && !fallback && mode != 12;
        bool bad_queries =
            query_enabled ? (!candidate.queries ||
                             (mode < 13 && !candidate.query_accepted))
                          : (candidate.queries || candidate.query_accepted);
        bool fast_allowed = filtered && !fallback && mode != 12 && mode != 15;
        if ((!fast_allowed && candidate.fast_evaluations) ||
            (fast_allowed && mode < 13 && !candidate.fast_evaluations) ||
            (mode == 16 && candidate.sdk_mutation_fallbacks != 1))
          throw std::runtime_error(
              "stable evaluator selection/fallback changed");
        if (control.states != candidate.states ||
            control.states != snapshot_only.states ||
            control.callbacks != snapshot_only.callbacks ||
            control.hooks != snapshot_only.hooks ||
            candidate.evaluations != snapshot_only.evaluations ||
            control.callbacks != candidate.callbacks ||
            control.callbacks != idle_only.callbacks ||
            control.hooks != candidate.hooks ||
            control.hooks != idle_only.hooks ||
            candidate.evaluations != idle_only.evaluations || bad_idle ||
            bad_queries || bytes("control.vcd") != bytes("candidate.vcd") ||
            bytes("control.vcd") != bytes("idle-only.vcd"))
          throw std::runtime_error(
              "callback sequence, SDK hooks, waveform, evaluation count, "
              "stable-state fallback or recreation changed");
      }
  for (unsigned kind = 0; kind < 11; ++kind) {
    char arg[] = "contract";
    char *argv[] = {arg};
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "Guards", nullptr};
    Runtime runtime(1, argv, info, evaluate);
    runtime.add_signal("held", 24, 1, false);
    runtime.add_signal("observed", 32, 1, true);
    runtime.add_signal("data", 25, 8, false);
    runtime.add_clock("held", "observed");
    bool rejected = false;
    try {
      if (kind == 0)
        runtime.add_idle_clock_model(nullptr, {"held"});
      if (kind == 1)
        runtime.add_idle_clock_model(idle, {});
      if (kind == 2)
        runtime.add_idle_clock_model(idle, {"absent"});
      if (kind == 3)
        runtime.add_idle_clock_model(idle, {"observed"});
      if (kind == 4)
        runtime.add_idle_clock_model(idle, {"data"});
      if (kind == 5)
        runtime.add_idle_clock_model(idle, {"held", "held"});
      if (kind == 6) {
        runtime.initialize();
        runtime.add_idle_clock_model(idle, {"held"});
      }
      if (kind == 7) {
        runtime.add_sampling_signal("data");
        runtime.add_idle_clock_model(idle, {"held"});
      }
      if (kind == 8) {
        runtime.add_signal("other", 36, 1, false);
        runtime.add_idle_clock_model(idle, {"other"});
      }
      if (kind == 10)
        runtime.add_idle_clock_model(idle, {"held"}, nullptr, fast_eval);
      if (kind == 9) {
        runtime.add_export("scope", "kernel", [](uint8_t **, uint64_t *) {}, 0,
                           0, {}, {}, {});
        runtime.add_idle_clock_model(idle, {"held"});
      }
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error("invalid idle clock contract accepted");
  }
  std::puts("PASS: 80 runtime quartets: all callback values/order/counts, "
            "SDK hooks, commit/combinational/analog feedback, snapshot "
            "copyback, falling reset, native VCD and recreation");
}
