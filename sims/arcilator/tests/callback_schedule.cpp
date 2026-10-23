#include "native/runtime.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using namespace chipyard_arc;
static_assert(sizeof(Binding) == 224);
static_assert(sizeof(Signal) == 16);
static_assert(sizeof(Runtime) == 1544);
static unsigned evaluations, hooks;
extern "C" void __real_arcRuntimeOnEval(ArcState *);
extern "C" void __wrap_arcRuntimeOnEval(ArcState *s) {
  ++hooks;
  __real_arcRuntimeOnEval(s);
}
static void evaluate(void *opaque) {
  ++evaluations;
  auto *p = static_cast<uint8_t *>(opaque);
  p[32] = p[24];
  p[42] = active_runtime()->time() >= 780000;
}
struct Result {
  std::vector<std::string> calls;
  std::vector<uint8_t> states;
  std::vector<std::string> events;
  unsigned evaluations = 0, hooks = 0;
};
struct Recorded : Binding {
  Result &result;
  Recorded(Runtime &r, std::string n, Result &out)
      : Binding(r, std::move(n)), result(out) {}
  void note(const char *phase) const {
    if (svGetScope() != dpi_scope->handle())
      throw std::runtime_error("wrong callback DPI scope");
    result.calls.push_back(
        module + ":" + phase + ":" + std::to_string(runtime.time_fs()) + ":" +
        std::to_string(get("in")) + ":" + std::to_string(get("out")));
  }
  void initialize() override { note("initialize"); }
};
struct Pure : Recorded {
  using Recorded::Recorded;
  void initialize() override {
    Recorded::initialize();
    stage("in", 3);
    mark_changed();
  }

public:
  constexpr BindingCallbackContract<Pure> native_callback_contract() const {
    return reviewed_base_callback_contract<Pure>();
  }
};
struct Combo : Recorded {
  using Recorded::Recorded;
  bool combinational() override {
    note("combinational");
    stage("in", 11);
    return false;
  }

public:
  constexpr BindingCallbackContract<Combo> native_callback_contract() const {
    return reviewed_base_callback_contract<Combo>();
  }
};
struct Sample : Recorded {
  using Recorded::Recorded;
  bool first = true;
  void sample() override {
    note("sample");
    if (first) {
      first = false;
      stage("in", 23);
    }
  }

public:
  constexpr BindingCallbackContract<Sample> native_callback_contract() const {
    return reviewed_base_callback_contract<Sample>();
  }
};
struct Commit : Recorded {
  using Recorded::Recorded;
  bool commit() override {
    note("commit");
    return Binding::commit();
  }

public:
  constexpr BindingCallbackContract<Commit> native_callback_contract() const {
    return reviewed_base_callback_contract<Commit>();
  }
};
struct Middle : Recorded {
  using Recorded::Recorded;
  void sample() override { note("inherited-sample"); }

public:
  constexpr BindingCallbackContract<Middle> native_callback_contract() const {
    return reviewed_base_callback_contract<Middle>();
  }
};
struct Inherited : Middle {
  using Middle::Middle;

public:
  constexpr BindingCallbackContract<Inherited>
  native_callback_contract() const {
    return reviewed_base_callback_contract<Inherited>();
  }
};
struct Picoseconds : Recorded {
  using Recorded::Recorded;
  uint64_t deadline = 1;
  uint64_t next_time() const override {
    note("next-time");
    return deadline;
  }
  void timed() override {
    note("timed");
    result.events.push_back("ps:" + std::to_string(runtime.time_fs()));
    stage("in", 37);
    deadline = deadline == 1 ? 2 : never;
  }

public:
  constexpr BindingCallbackContract<Picoseconds>
  native_callback_contract() const {
    return reviewed_base_callback_contract<Picoseconds>();
  }
};
struct Femtoseconds : Recorded {
  using Recorded::Recorded;
  uint64_t deadline = 250;
  uint64_t next_time_fs() const override {
    note("next-time-fs");
    return deadline;
  }
  void timed() override {
    note("timed");
    result.events.push_back("fs:" + std::to_string(runtime.time_fs()));
    stage("in", 41);
    deadline = deadline == 250 ? 1250 : never;
  }

public:
  constexpr BindingCallbackContract<Femtoseconds>
  native_callback_contract() const {
    return reviewed_base_callback_contract<Femtoseconds>();
  }
};
struct Legacy : Recorded {
  using Recorded::Recorded;
  bool combinational() override {
    note("legacy-combinational");
    return false;
  }
  void sample() override { note("legacy-sample"); }
  uint64_t next_time_fs() const override {
    note("legacy-poll");
    return never;
  }
  bool commit() override {
    note("legacy-commit");
    return Binding::commit();
  }

public:
  constexpr BindingCallbackContract<Legacy> native_callback_contract() const {
    return reviewed_base_callback_contract<Legacy>();
  }
};
class PrivateOverride : public Recorded {
public:
  using Recorded::Recorded;

private:
  bool combinational() override {
    note("private-combinational");
    return false;
  }

public:
  constexpr BindingCallbackContract<PrivateOverride>
  native_callback_contract() const {
    return reviewed_base_callback_contract<PrivateOverride>();
  }
};
struct Hiding : Middle {
  using Binding::sample;
  using Middle::Middle;
};
struct Extended : Pure {
  using Pure::Pure;
  bool combinational() override {
    note("new-combinational");
    return false;
  }
};
template <class T> static T *add(Runtime &r, const char *name, Result &out) {
  auto p = make_binding_instance<T>(r, name, out);
  auto *raw = p.get();
  p->ports.emplace("in", r.signal("in"));
  p->ports.emplace("out", r.signal("out"));
  r.add_binding(std::move(p));
  return raw;
}
static Result trial(bool scheduled, bool run_model) {
  Result result;
  evaluations = hooks = 0;
  char arg0[] = "callback-schedule";
  char off[] = "+arc-no-callback-schedule";
  char *argv[] = {arg0, off};
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "CallbackSchedule",
                           nullptr};
  {
    Runtime r(scheduled ? 1 : 2, argv, info, evaluate);
    r.add_signal("in", 24, 8, false);
    r.add_signal("out", 32, 8, true);
    r.add_signal("clock", 40, 1, false);
    r.add_signal("reset", 41, 1, false);
    r.add_signal("io_success", 42, 1, true);
    r.add_signal("arc_printf_enable", 43, 1, false);
    r.add_signal("arc_stop_enable", 44, 1, false);
    auto *pure = add<Pure>(r, "pure", result);
    if (pure->needs_combinational_callback() || pure->needs_sample_callback() ||
        pure->needs_time_polling() || pure->needs_commit_callback())
      throw std::runtime_error("inherited no-op facts missing");
    auto *ps = add<Picoseconds>(r, "ps", result);
    auto *fs = add<Femtoseconds>(r, "fs", result);
    if (!ps->needs_time_polling() || !fs->needs_time_polling())
      throw std::runtime_error("overridden timer omitted");
    auto *sample = add<Sample>(r, "sample", result);
    auto *combo = add<Combo>(r, "combo", result);
    auto *commit = add<Commit>(r, "commit", result);
    auto *derived = add<Inherited>(r, "inherited", result);
    if (!sample->needs_sample_callback() ||
        !combo->needs_combinational_callback() ||
        !commit->needs_commit_callback() || !derived->needs_sample_callback())
      throw std::runtime_error("inherited/overridden callback omitted");
    auto *hiding = add<Hiding>(r, "hiding", result);
    auto *extended = add<Extended>(r, "extended", result);
    if (!hiding->needs_sample_callback() ||
        !extended->needs_combinational_callback())
      throw std::runtime_error("an inherited contract hid a virtual override");
    auto *hidden = add<PrivateOverride>(r, "private", result);
    if (!hidden->needs_combinational_callback() ||
        !hidden->needs_sample_callback() || !hidden->needs_time_polling() ||
        !hidden->needs_commit_callback())
      throw std::runtime_error(
          "private override did not use conservative fallback");
    register_binding("legacy",
                     [&](Runtime &owner,
                         const std::string &name) -> std::unique_ptr<Binding> {
                       return std::make_unique<Legacy>(owner, name, result);
                     });
    auto legacy = make_binding(r, "legacy");
    legacy->ports.emplace("in", r.signal("in"));
    legacy->ports.emplace("out", r.signal("out"));
    if (!legacy->needs_combinational_callback() ||
        !legacy->needs_sample_callback() || !legacy->needs_time_polling() ||
        !legacy->needs_commit_callback())
      throw std::runtime_error("erased factory did not retain all callbacks");
    r.add_binding(std::move(legacy));
    auto cached = r.signal("in");
    auto save = [&] {
      if (cached.data != r.signal("in").data)
        throw std::runtime_error("Signal address changed");
      auto *base = cached.data - 24;
      result.states.insert(result.states.end(), base, base + 64);
    };
    if (run_model) {
      if (r.run())
        throw std::runtime_error("run failed");
      save();
    } else {
      r.initialize();
      if (!pure->needs_commit_callback())
        throw std::runtime_error("pending base commit omitted");
      r.advance_fs(0);
      save();
      if (pure->needs_commit_callback())
        throw std::runtime_error("base commit did not drain queued writes");
      add<Inherited>(r, "late", result);
      for (uint64_t t : {500u, 1500u, 2500u}) {
        r.advance_fs(t, [&] { r.signal("in").set(t & 255); });
        save();
      }
    }
    if (r.signal("out").get() != 11)
      throw std::runtime_error("staged/combinational result incorrect");
  }
  result.evaluations = evaluations;
  result.hooks = hooks;
  return result;
}
int main() {
  const std::vector<std::string> events = {"fs:250", "ps:1000", "fs:1250",
                                           "ps:2000"};
  for (unsigned recreation = 0; recreation < 3; ++recreation)
    for (bool run_model : {false, true}) {
      auto control = trial(false, run_model);
      auto candidate = trial(true, run_model);
      if (control.calls != candidate.calls ||
          control.states != candidate.states ||
          control.evaluations != candidate.evaluations ||
          control.hooks != candidate.hooks || control.events != events ||
          candidate.events != events)
        throw std::runtime_error("callback ordering, scope, timers, state, SDK "
                                 "or recreation changed");
    }
  std::puts("PASS: exact/erased/private/inherited callback facts, pending and "
            "dirty base commits, ps/fs timers, late bindings, run/advance, "
            "scopes, full states, SDK hooks and pointer stability");
}
