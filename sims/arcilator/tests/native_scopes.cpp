#include "native/runtime.h"
#include <array>
#include <cstdio>
#include <thread>

using namespace chipyard_arc;
static std::array<unsigned, 2> phases{};
static void check(bool good, const char *message) {
  if (!good)
    throw std::runtime_error(message);
}
class ScopeProbe : public Binding {
  bool timer_done = false;
  unsigned index() const { return instance_name == "left" ? 0 : 1; }
  void observe(unsigned phase) const {
    auto current = svGetScope();
    check(current == dpi_scope->handle(), "native callback has wrong scope");
    auto expected = "NativeScopesHarness/" + instance_name;
    check(svGetNameFromScope(current) == expected,
          "native scope lost original instance path");
    check(svGetScopeFromName(expected.c_str()) == current,
          "scope lookup changed identity");
    phases[index()] |= phase;
  }

public:
  using Binding::Binding;
  void initialize() override {
    observe(1);
    auto current = svGetScope();
    auto root = svGetScopeFromName("NativeScopesHarness");
    check(root && svSetScope(root) == current,
          "setting DPI scope lost previous");
    check(svSetScope(current) == root, "restoring DPI scope lost previous");
    auto nested_name = dpi_scope->name() + "/temporary";
    {
      DpiScope nested(nested_name);
      try {
        DpiContext context(nested);
        check(svGetScope() == nested.handle(), "nested DPI context mismatch");
        throw 7;
      } catch (int) {
        check(svGetScope() == current, "DPI context not restored on exception");
      }
    }
    check(!svGetScopeFromName(nested_name.c_str()),
          "destroyed scope still visible");
  }
  bool combinational() override {
    observe(2);
    return false;
  }
  void sample() override {
    observe(4);
    bool clock = get("clock");
    if (clock && !old_clock)
      stage("q", get("data") ^ (index() ? 0xa5 : 0));
    old_clock = clock;
  }
  uint64_t next_time() const override {
    observe(8);
    return timer_done ? never : 250;
  }
  void timed() override {
    observe(16);
    timer_done = true;
    if (runtime.flag("scope-run"))
      runtime.finish();
  }
};
int main(int argc, char **argv) {
  check(!svGetScope() && !svGetScopeFromName("missing") &&
            !svGetNameFromScope(nullptr),
        "scope APIs fabricated an active context");
  register_binding("ArcScopeProbe",
                   [](Runtime &runtime, const std::string &name) {
                     return std::make_unique<ScopeProbe>(runtime, name);
                   });
  auto model = create_model(argc, argv);
  if (model->flag("scope-run")) {
    auto status = model->run();
    check(!status && phases[0] == 31 && phases[1] == 31,
          "run loop did not scope every callback phase");
    model.reset();
    check(!svGetScope() && !svGetScopeFromName("NativeScopesHarness"),
          "run loop leaked DPI scope");
    std::puts("PASS: native run loop scopes every callback phase");
    return status;
  }
  model->initialize();
  check(!svGetScope(), "initialization leaked callback scope");
  std::thread worker([] {
    check(!svGetScope(), "DPI scope leaked between host threads");
    DpiScope scope("worker");
    DpiContext context(scope);
    check(svGetScope() == scope.handle(), "thread-local DPI context mismatch");
  });
  worker.join();
  check(!svGetScope() && !svGetScopeFromName("worker"),
        "worker scope outlived its owner");
  {
    DpiScope outer("duplicate");
    {
      DpiScope inner("duplicate");
      check(svGetScopeFromName("duplicate") == inner.handle(),
            "duplicate live scope lookup mismatch");
    }
    check(svGetScopeFromName("duplicate") == outer.handle(),
          "scope destruction did not restore prior lookup");
  }
  for (unsigned cycle = 0; cycle < 8; ++cycle) {
    auto value = uint8_t(cycle * 37 + 3);
    model->advance(cycle * 1000, [&] {
      model->signal("clock").set(0);
      model->signal("data").set(value);
    });
    model->advance(cycle * 1000 + 500, [&] { model->signal("clock").set(1); });
    check(model->signal("left").get() == value &&
              model->signal("right").get() == (value ^ 0xa5),
          "scoped native bindings lost independent state");
    check(!svGetScope(), "evaluation leaked callback scope");
  }
  check(phases[0] == 31 && phases[1] == 31,
        "scope regression did not visit every callback phase");
  model.reset();
  check(!svGetScopeFromName("NativeScopesHarness") &&
            !svGetScopeFromName("NativeScopesHarness/left") &&
            !svGetScopeFromName("NativeScopesHarness/nested/right"),
        "model destruction leaked registered scopes");
  std::puts(
      "PASS: native DPI scopes, callback paths, all phases, nested/exception "
      "restoration, thread isolation and lifetime cleanup");
}
