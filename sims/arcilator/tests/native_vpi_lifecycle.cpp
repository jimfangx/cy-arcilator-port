#include "native/runtime.h"
#include "vpi_user.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace chipyard_arc;
static Runtime *model;
static std::vector<int> trace;
static unsigned startup_calls, initialized_hooks, eval_hooks;
static bool early_eval = false, ready = false, is_run = false, fail_end = false;
static void check(bool value, const char *why) {
  if (!value)
    throw std::runtime_error(why);
}
static void poke(uint8_t **, uint64_t *) {
  model->signal("q").set(33);
  trace.push_back(3);
}
static void peek(uint8_t **p, uint64_t *) {
  p[0][0] = model->signal("q").get();
}
static PLI_INT32 on_start(p_cb_data data) {
  check(data->reason == cbStartOfSimulation, "wrong start reason");
  check(model && model->signal("q").get() == 17 && !ready,
        "start was not before RTL initial");
  s_vpi_vlog_info info{};
  check(vpi_get_vlog_info(&info) && info.argc == 1, "startup lost arguments");
  check(!svGetScope(), "VPI inherited an unrelated binding DPI context");
  trace.push_back(2);
  svSetScope(svGetScopeFromName("VpiHarness"));
  std::vector<ExportBytes> values;
  model->invoke_export("poke", values);
  svSetScope(nullptr);
  return 0;
}
static PLI_INT32 on_end(p_cb_data data) {
  check(data->reason == cbEndOfSimulation && model && ready, "wrong end phase");
  check(model->signal("q").get() == 88, "end preceded RTL final");
  auto before = eval_hooks;
  svSetScope(svGetScopeFromName("VpiHarness"));
  std::vector<ExportBytes> values(1, ExportBytes(1));
  model->invoke_export("peek", values);
  svSetScope(nullptr);
  check(values[0][0] == 88 && eval_hooks == before,
        "end export evaluated finalized model");
  trace.push_back(8);
  if (fail_end)
    throw std::runtime_error("intentional end failure");
  return 0;
}
static void callbacks() {
  s_cb_data data{};
  data.cb_rtn = on_start;
  data.reason = cbStartOfSimulation;
  check(vpi_register_cb(&data), "start registration failed");
  data.cb_rtn = on_end;
  data.reason = cbEndOfSimulation;
  check(vpi_register_cb(&data), "end registration failed");
}
extern "C" void linked_startup() {
  ++startup_calls;
  trace.push_back(0);
  callbacks();
}
extern "C" {
void (*vlog_startup_routines[])() = {linked_startup, nullptr};
}
static void initial(void *p) {
  check(model->signal("q").get() == 33,
        "startup export write missing at RTL initial");
  trace.push_back(4);
  static_cast<uint8_t *>(p)[29] = 42;
}
static void final(void *p) {
  trace.push_back(7);
  static_cast<uint8_t *>(p)[29] = 88;
}
static void eval(void *) {
  check(ready, "model body ran before SDK initialized");
  trace.push_back(6);
}
extern "C" void __real_arcRuntimeOnInitialized(ArcState *);
extern "C" void __wrap_arcRuntimeOnInitialized(ArcState *s) {
  ++initialized_hooks;
  trace.push_back(5);
  __real_arcRuntimeOnInitialized(s);
  if (initialized_hooks % 2 == 0)
    ready = true;
}
extern "C" void __real_arcRuntimeOnEval(ArcState *);
extern "C" void __wrap_arcRuntimeOnEval(ArcState *s) {
  if (!ready)
    early_eval = true;
  ++eval_hooks;
  __real_arcRuntimeOnEval(s);
}
class Probe : public Binding {
  bool done = false;

public:
  using Binding::Binding;
  void initialize() override {
    check(svGetScope() == dpi_scope->handle(), "binding DPI context lost");
    trace.push_back(1);
    port("q").set(17);
  }
  uint64_t next_time_fs() const override {
    return is_run && !done ? 250 : never;
  }
  void timed() override {
    done = true;
    runtime.finish();
  }
};
int main() {
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "VpiUnit", nullptr};
  char name[] = "lifecycle-test", *argv[] = {name};
  for (unsigned pass = 0; pass < 3; ++pass) {
    trace.clear();
    ready = false;
    is_run = pass != 0;
    fail_end = pass == 2;
    if (pass)
      callbacks();
    {
      Runtime runtime(1, argv, info, eval, initial, final, nullptr, nullptr,
                      false, "VpiHarness");
      model = &runtime;
      runtime.add_signal("clock", 24, 1, false);
      runtime.add_signal("reset", 25, 1, false);
      runtime.add_signal("arc_printf_enable", 26, 1, false);
      runtime.add_signal("arc_stop_enable", 27, 1, false);
      runtime.add_signal("io_success", 28, 1, true);
      runtime.add_signal("q", 29, 8, false);
      auto binding = std::make_unique<Probe>(runtime, "Probe");
      binding->ports = {{"q", runtime.signal("q")}};
      runtime.add_binding(std::move(binding));
      runtime.add_export("VpiHarness", "poke", poke, 0, 0, {}, {}, {});
      runtime.add_export("VpiHarness", "peek", peek, 0, 8, {}, {}, {});
      if (is_run)
        check(runtime.run() == int(fail_end),
              "run status did not reflect end callback failure");
      else {
        runtime.initialize();
        runtime.settle();
        runtime.advance_fs(250);
      }
      check(!early_eval,
            "VPI startup export called SDK OnEval before initialization");
    }
    model = nullptr;
    check(trace.back() == 8, "runtime destruction did not deliver end");
    auto first = pass ? 0 : 1;
    check(trace[first] == 1 && trace[first + 1] == 2 && trace[first + 2] == 3 &&
              trace[first + 3] == 4 && trace[first + 4] == 5 &&
              trace[first + 5] == 5,
          "initialization ordering changed");
    check(trace[trace.size() - 2] == 7 && !svGetScope(),
          "end/final order or scope cleanup changed");
  }
  check(startup_calls == 1,
        "linked startup routines replayed across model recreation");
  std::puts("PASS: real SDK Runtime lifecycle, linked C startup, "
            "binding/SV/DPI/export order, SDK hooks, manual/run paths, "
            "final/end and recreation");
}
