#include "native/runtime.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
using namespace chipyard_arc;
static unsigned mode, evaluations, output_offset;
static void evaluate(void *opaque) {
  ++evaluations;
  auto *state = static_cast<uint8_t *>(opaque);
  auto old = state[output_offset];
  state[output_offset] =
      mode == 3 ? std::min<unsigned>(5, state[32] + 1) : state[32];
  if (mode == 1)
    state[48] = old;
  if (mode == 2)
    state[48] = std::min<unsigned>(3, state[48] + 1);
}
struct Unknown : Binding {
  using Binding::Binding;
};
struct Feedback : Binding {
  using Binding::Binding;
  bool output_convergence_safe() const override { return true; }
  bool combinational() override {
    return runtime.signal("in").set(runtime.signal("out").get());
  }
};
static void kernel(uint8_t **, uint64_t *) {}
static unsigned trial(unsigned offset, unsigned kind) {
  char arg[] = "convergence";
  char *argv[] = {arg};
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 96, "Convergence", nullptr};
  Runtime runtime(1, argv, info, evaluate);
  mode = kind == 4 ? 1 : kind == 2 ? 2 : kind == 3 ? 3 : 0;
  output_offset = offset;
  runtime.add_signal("out", offset, 8, true);
  runtime.add_signal("in", 32, 8, false);
  if (kind != 0 && kind != 4)
    runtime.add_write_only_output("out");
  if (kind == 5)
    runtime.add_binding(std::make_unique<Unknown>(runtime, "Unknown"));
  if (kind == 3)
    runtime.add_binding(std::make_unique<Feedback>(runtime, "Feedback"));
  runtime.initialize();
  if (kind == 6)
    runtime.add_export("scope", "kernel", kernel, 0, 0, {}, {}, {});
  if (kind == 7)
    runtime.add_signal("late", 55, 8, false);
  evaluations = 0;
  runtime.advance(0, [&] { runtime.signal("in").set(kind == 3 ? 0 : 1); });
  unsigned expected = kind == 0 || kind == 5 || kind == 6 || kind == 7 ? 2
                      : kind == 2                                      ? 4
                      : kind == 3                                      ? 6
                      : kind == 4                                      ? 3
                                                                       : 1;
  if (evaluations != expected ||
      runtime.signal("out").get() != (kind == 3 ? 5 : 1))
    throw std::runtime_error("convergence/feedback/fallback count");
  return evaluations;
}
int main() {
  for (unsigned offset : {24u, 40u})
    for (unsigned kind = 0; kind < 8; ++kind)
      trial(offset, kind);
  for (unsigned kind = 0; kind < 4; ++kind) {
    char arg[] = "guards";
    char *argv[] = {arg};
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 96, "Convergence",
                             nullptr};
    Runtime runtime(1, argv, info, evaluate);
    runtime.add_signal("in", 32, 8, false);
    runtime.add_signal("out", 24, 8, true);
    bool rejected = false;
    try {
      if (kind == 0)
        runtime.add_write_only_output("in");
      if (kind == 1) {
        runtime.add_write_only_output("out");
        runtime.add_write_only_output("out");
      }
      if (kind == 2) {
        runtime.initialize();
        runtime.add_write_only_output("out");
      }
      if (kind == 3) {
        runtime.add_signal("alias", 24, 1, false);
        runtime.add_write_only_output("out");
        runtime.initialize();
      }
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error("invalid certificate accepted");
  }
  std::puts("PASS: owned comparison ranges, private state, binding feedback, "
            "unknown bindings, exports, late signals and metadata guards");
}
