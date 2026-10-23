#include "native/runtime.h"
#include <cstdio>
#include <string>

using namespace chipyard_arc;
static unsigned mode, evaluations;
static bool benchmark;

static void initialize_model(void *) {
  auto &r = *active_runtime();
  r.add_signal("arc_printf_enable", 2, 1, false);
  r.add_signal("arc_stop_enable", 3, 1, false);
  if (!(mode & 8) && mode != 16)
    r.add_signal("io_success", 4, 1, true);
  if (!benchmark) {
    r.add_trace_signal("clock", 0, 1);
    r.add_trace_signal("reset", 1, 1);
    r.add_trace_signal("print", 2, 1);
    r.add_trace_signal("stop", 3, 1);
    r.add_trace_signal("success", 4, 1);
  }
}

static void evaluate_model(void *opaque) {
  ++evaluations;
  auto *state = static_cast<uint8_t *>(opaque);
  if (!benchmark && mode != 17 && active_runtime()->time() >= 900500)
    state[4] = 1;
}

class Events : public Binding {
  unsigned event = 0;

public:
  Events(Runtime &r) : Binding(r, "RunLoopEvents") {}
  uint64_t next_time() const override {
    return event < 3 ? (event == 0   ? 700000
                        : event == 1 ? 800000
                                     : 900500)
                     : never;
  }
  void timed() override {
    if (event == 0 && (mode & 8) && mode != 16)
      runtime.add_signal("io_success", 4, 1, true);
    if (event == 1) {
      // Existing signal names retain their original storage on re-registration.
      runtime.add_signal("arc_printf_enable", 12, 1, false);
      runtime.add_signal("arc_stop_enable", 13, 1, false);
      if (mode != 16)
        runtime.add_signal("io_success", 14, 1, true);
    }
    ++event;
  }
};

int main(int argc, char **argv) {
  uint8_t dirty[] = {0x02, 0xff, 0x80, 0xfe};
  const uint8_t original[] = {0x02, 0xff, 0x80, 0xfe};
  if (Signal{dirty, 1}.get() != 0 || Signal{dirty + 1, 8}.get() != 255 ||
      Signal{dirty + 2, 7}.get() != 0 || Signal{dirty + 3, 4}.get() != 14 ||
      Signal{dirty + 1, 0}.get() != 0 || Signal{dirty + 1, 9}.get() != 255 ||
      std::memcmp(dirty, original, sizeof(dirty)))
    throw std::runtime_error("incorrect scalar port masking or input mutation");
  benchmark = argc > 1 && std::string(argv[1]) == "--benchmark";
  if (!benchmark && argc != 2)
    throw std::runtime_error("expected waveform directory or --benchmark");
  for (mode = 0; mode != (benchmark ? 1u : 18u); ++mode) {
    std::vector<std::string> args{"run-loop", benchmark ? "+max-cycles=2000000"
                                                        : "+max-cycles=1000"};
    if (mode & 1)
      args.emplace_back("+verbose");
    if (!benchmark)
      args.emplace_back("+vcdfile=" + std::string(argv[1]) + "/mode" +
                        std::to_string(mode) + ".vcd");
    std::vector<char *> pointers;
    for (auto &arg : args)
      pointers.push_back(arg.data());
    ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 16, "RunLoop", nullptr};
    Runtime r(pointers.size(), pointers.data(), info, evaluate_model,
              initialize_model, nullptr, nullptr, nullptr, bool(mode & 4));
    r.add_signal("clock", 0, 1, false);
    r.add_signal("reset", 1, 1, false);
    r.set_root_clock_used(bool(mode & 2));
    r.add_binding(std::make_unique<Events>(r));
    evaluations = 0;
    int rc = r.run();
    bool expected_failure = benchmark || mode >= 16;
    if (rc != int(expected_failure))
      throw std::runtime_error("incorrect success/timeout/missing-port result");
    if (mode < 16 && !benchmark && r.time() != 901500)
      throw std::runtime_error("incorrect completion time");
    if (r.signal("arc_printf_enable").get() != bool(mode & 1) ||
        r.signal("arc_stop_enable").get() != 1)
      throw std::runtime_error("incorrect diagnostic enables");
    std::printf("mode=%u rc=%d ps=%llu evals=%u print=%llu stop=%llu\n", mode,
                rc, (unsigned long long)r.time(), evaluations,
                (unsigned long long)r.signal("arc_printf_enable").get(),
                (unsigned long long)r.signal("arc_stop_enable").get());
  }
  std::puts(
      "PASS: run loop reset/completion/timeout, initialized and late ports, "
      "verbose, root and timed clocks, stable storage, and waveforms");
}
