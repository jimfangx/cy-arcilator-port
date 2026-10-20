#include "runtime.h"
#include "compat.h"
#include "evaluation.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
extern "C" void chipyard_arc_close_files();
namespace chipyard_arc {
namespace {
Runtime *active = nullptr;
std::map<std::string, Factory> &factories() {
  static std::map<std::string, Factory> map;
  return map;
}
} // namespace
Runtime *active_runtime() { return active; }
void Runtime::add_binding(std::unique_ptr<Binding> binding) {
  const auto &name =
      binding->instance_name.empty() ? binding->module : binding->instance_name;
  binding->dpi_scope =
      std::make_unique<DpiScope>(root_scope.name() + "/" + name);
  binding->configure();
  bindings.push_back(std::move(binding));
}
void register_binding(const std::string &name, Factory factory) {
  factories()[name] = std::move(factory);
}
std::unique_ptr<Binding> make_binding(Runtime &runtime,
                                      const std::string &name) {
  auto it = factories().find(name);
  if (it == factories().end())
    throw std::runtime_error("no native Arcilator binding for foreign module " +
                             name);
  return it->second(runtime, name);
}
uint64_t Binding::number(const std::string &name, uint64_t fallback) const {
  auto it = parameters.find(name);
  return it == parameters.end() ? fallback
                                : std::stoull(it->second, nullptr, 0);
}
std::string Binding::text(const std::string &name,
                          const std::string &fallback) const {
  auto it = parameters.find(name);
  return it == parameters.end() ? fallback : it->second;
}
void Binding::stage(const std::string &name, uint64_t value) {
  auto s = port(name);
  if (s.bits > 64)
    throw std::runtime_error("scalar stage of wide native port");
  if (s.bits < 64)
    value &= (uint64_t(1) << s.bits) - 1;
  std::vector<uint8_t> bytes(s.bytes());
  std::memcpy(bytes.data(), &value, bytes.size());
  stage(name, bytes);
}
void Binding::stage(const std::string &name,
                    const std::vector<uint8_t> &value) {
  pending.emplace_back(port(name), value);
}
void Binding::drive(const std::string &name, uint64_t value, uint64_t mask) {
  runtime.drive_analog(this, name, port(name), value, mask);
}
bool Binding::commit() {
  bool changed = internal_changed;
  internal_changed = false;
  for (auto &entry : pending)
    changed |= entry.first.assign(entry.second);
  pending.clear();
  return changed;
}
Runtime::Runtime(int argc, char **argv, ArcRuntimeModelInfo &model,
                 void (*eval)(void *), void (*initial)(void *),
                 void (*final)(void *), void (*time_setter)(void *, uint64_t),
                 uint64_t (*wakeup_getter)(void *), bool time_readers,
                 const char *scope_name)
    : info(model), root_scope(scope_name ? scope_name : model.modelName),
      evaluate(eval), finalize(final), initial_model(initial),
      set_model_time(time_setter), get_model_wakeup(wakeup_getter),
      femtosecond_trace(time_readers) {
  if (active)
    throw std::runtime_error(
        "only one native simulation context is supported per process");
  if (argc < 0 || (argc && !argv))
    throw std::invalid_argument("invalid native command arguments");
  if (argc)
    arguments.assign(argv, argv + argc);
  trace.open(option("arc-vcdfile=", option("vcdfile=")),
             std::stoull(option("dump-start=", "0")), femtosecond_trace);
  active = this;
  command_args(argc, argv);
  current = arcRuntimeAllocateInstance(&info, nullptr);
  staged = arcRuntimeAllocateInstance(&info, nullptr);
}
Runtime::~Runtime() {
  if (initialized && finalize)
    finalize(current->modelState);
  chipyard_arc_close_files();
  arcRuntimeDeleteInstance(staged);
  arcRuntimeDeleteInstance(current);
  active = nullptr;
}
void Runtime::add_signal(const std::string &name, unsigned offset,
                         unsigned bits, bool output) {
  if (!bits || offset + (bits + 7) / 8 > info.numStateBytes)
    throw std::runtime_error("invalid native layout for " + name);
  signals.emplace(name, Signal{current->modelState + offset, bits});
  if (output) {
    unsigned bytes = (bits + 7) / 8;
    output_ranges.push_back({offset, bytes, previous_outputs.size()});
    previous_outputs.resize(previous_outputs.size() + bytes);
  }
}
void Runtime::add_analog_signal(const std::string &name, unsigned bits) {
  if (!bits || bits > 64 || signals.count(name))
    throw std::runtime_error("invalid native analog net " + name);
  auto &net = analog_nets[name];
  net.bits = bits;
  net.storage.resize((bits + 7) / 8);
  signals.emplace(name, Signal{net.storage.data(), bits});
  analog_addresses.emplace(net.storage.data(), &net);
  trace.add(name, net.storage.data(), bits);
}
void Runtime::drive_analog(const Binding *binding, const std::string &name,
                           Signal signal, uint64_t value, uint64_t mask) {
  auto it = analog_addresses.find(signal.data);
  if (it == analog_addresses.end())
    throw std::runtime_error("native drive of a non-analog port " + name);
  it->second->drivers[{binding, name}] = {value, mask};
}
bool Runtime::resolve_analog() {
  bool changed = false;
  for (auto &entry : analog_nets) {
    auto &net = entry.second;
    uint64_t ones = 0, zeros = 0;
    for (auto &driver : net.drivers) {
      auto value = driver.second.first, mask = driver.second.second;
      ones |= value & mask;
      zeros |= ~value & mask;
    }
    // Arc is two-state. Undriven and conflicting bits resolve to zero;
    // driven, uncontested bits retain their value and high impedance releases.
    changed |= Signal{net.storage.data(), net.bits}.set(ones & ~zeros);
  }
  return changed;
}
void Runtime::add_trace_signal(const std::string &name, unsigned offset,
                               unsigned bits) {
  if (!bits || offset + (bits + 7) / 8 > info.numStateBytes)
    throw std::runtime_error("invalid native trace layout for " + name);
  trace.add(name, current->modelState + offset, bits);
}
void Runtime::add_clock(const std::string &held, const std::string &observed) {
  clocks.emplace_back(signal(held).data - current->modelState,
                      signal(observed).data - current->modelState);
}
void Runtime::add_sampling_signal(const std::string &name) {
  auto input = signal(name);
  if (input.bits != 8)
    throw std::runtime_error("native sampling control must be one byte");
  sampling_offset = input.data - current->modelState;
}
bool Runtime::flag(const std::string &name) const {
  return std::find(arguments.begin(), arguments.end(), "+" + name) !=
         arguments.end();
}
std::string Runtime::option(const std::string &prefix,
                            const std::string &fallback) const {
  for (auto &arg : arguments)
    if (arg.compare(0, prefix.size() + 1, "+" + prefix) == 0)
      return arg.substr(prefix.size() + 1);
  return fallback;
}
void Runtime::settle() {
  struct Guard {
    unsigned &depth;
    Guard(unsigned &depth) : depth(depth) { ++depth; }
    ~Guard() { --depth; }
  } guard(evaluation_depth);
  if (sampling_offset != std::numeric_limits<size_t>::max())
    current->modelState[sampling_offset] = 0;
  if (set_model_time)
    set_model_time(current->modelState, now_fs);
  auto size = info.numStateBytes;
  for (unsigned delta = 0; delta < 1000; ++delta) {
    bool stable = false;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      // Internal coroutine results can change before any public output does.
      // Reuse staging storage while settling; the clock phase overwrites it
      // from fully settled current state before any sequential sampling.
      std::memcpy(staged->modelState, current->modelState, size);
      evaluate_state(current);
      bool changed =
          std::memcmp(current->modelState, staged->modelState, size) != 0;
      for (auto &binding : bindings) {
        DpiContext context(*binding->dpi_scope);
        changed |= binding->combinational();
      }
      changed |= resolve_analog();
      if (!changed) {
        stable = true;
        break;
      }
    }
    if (!stable)
      throw std::runtime_error("native combinational logic did not converge");
    bool edge = false;
    for (auto clock : clocks)
      edge |=
          current->modelState[clock.first] != current->modelState[clock.second];
    if (edge) {
      std::memcpy(staged->modelState, current->modelState, size);
      for (auto clock : clocks)
        staged->modelState[clock.first] = current->modelState[clock.second];
    }
    // All foreign sequential models sample settled pre-edge values. Publish
    // their nonblocking outputs only after the native snapshot is complete.
    for (auto &binding : bindings) {
      DpiContext context(*binding->dpi_scope);
      binding->sample();
    }
    if (edge) {
      SamplingPhase sampling;
      if (sampling_offset != std::numeric_limits<size_t>::max())
        staged->modelState[sampling_offset] = 1;
      evaluate_state(staged);
      std::memcpy(current->modelState, staged->modelState, size);
      if (sampling_offset != std::numeric_limits<size_t>::max())
        current->modelState[sampling_offset] = 0;
    }
    bool changed = false;
    for (auto &binding : bindings) {
      DpiContext context(*binding->dpi_scope);
      changed |= binding->commit();
    }
    if (!edge && !changed && next_model_time() != now_fs) {
      sample_trace();
      return;
    }
  }
  throw std::runtime_error("native event delta limit exceeded");
}
void Runtime::initialize() {
  if (initialized)
    throw std::runtime_error("native bindings were already initialized");
  for (auto &binding : bindings) {
    DpiContext context(*binding->dpi_scope);
    binding->initialize();
  }
  initialized = true;
  if (initial_model) {
    ++evaluation_depth;
    try {
      initial_model(current->modelState);
    } catch (...) {
      --evaluation_depth;
      throw;
    }
    --evaluation_depth;
  }
  arcRuntimeOnInitialized(current);
  arcRuntimeOnInitialized(staged);
}
void Runtime::evaluate_state(ArcState *state) {
  ++evaluation_depth;
  try {
    arcRuntimeOnEval(state);
    evaluate(state->modelState);
  } catch (...) {
    --evaluation_depth;
    throw;
  }
  --evaluation_depth;
}
// Foreign bindings keep their public picosecond interface. Arc's context and
// coroutine delays use femtoseconds, including events between those boundaries.
static uint64_t in_femtoseconds(uint64_t picoseconds) {
  if (picoseconds == never)
    return never;
  if (picoseconds > (never - 1) / 1000)
    throw std::runtime_error(
        "native simulation time exceeds the Arc context range");
  return picoseconds * 1000;
}
uint64_t Binding::next_time_fs() const {
  return in_femtoseconds(next_time());
}
uint64_t Runtime::next_model_time() const {
  if (!get_model_wakeup)
    return never;
  auto wakeup = get_model_wakeup(current->modelState);
  return wakeup == never ? never : std::max(now_fs, wakeup);
}
void Runtime::advance(uint64_t target, const std::function<void()> &drive) {
  advance_fs(in_femtoseconds(target), drive);
}
void Runtime::advance_fs(uint64_t target_fs, const std::function<void()> &drive) {
  bool drive_applied = false;
  if (!initialized || target_fs == never || target_fs < now_fs)
    throw std::runtime_error("invalid native simulation advance");
  for (;;) {
    uint64_t next = std::min(target_fs, next_model_time());
    for (auto &binding : bindings) {
      DpiContext context(*binding->dpi_scope);
      next = std::min(next,
                      std::max(now_fs, binding->next_time_fs()));
    }
    now_fs = next;
    now = now_fs / 1000;
    if (now_fs == target_fs && drive && !drive_applied) {
      drive();
      drive_applied = true;
    }
    for (auto &binding : bindings) {
      DpiContext context(*binding->dpi_scope);
      if (binding->next_time_fs() <= now_fs)
        binding->timed();
    }
    settle();
    bool pending_now = next_model_time() == now_fs;
    for (auto &binding : bindings) {
      DpiContext context(*binding->dpi_scope);
      pending_now |= binding->next_time_fs() <= now_fs;
    }
    if (now_fs == target_fs && !pending_now)
      return;
  }
}
int Runtime::run() {
  try {
    auto root_clock = signal("clock"), reset = signal("reset");
    reset.set(1);
    root_clock.set(0);
    initialize();
    uint64_t root_next = 500, reset_at = 777700, cycles = 0;
    auto max_cycles = std::stoull(option("max-cycles=", "0"));
    auto printf_enable = signal("arc_printf_enable");
    auto stop_enable = signal("arc_stop_enable");
    Signal success{};
    const bool verbose = flag("verbose");
    printf_enable.set(0);
    stop_enable.set(0);
    settle();
    while (!finished && error.empty()) {
      uint64_t next =
          std::min(in_femtoseconds(root_next), in_femtoseconds(reset_at));
      for (auto &binding : bindings) {
        DpiContext context(*binding->dpi_scope);
        next = std::min(
            next, std::max(now_fs, binding->next_time_fs()));
      }
      next = std::min(next, next_model_time());
      if (next == never)
        throw std::runtime_error("native simulation has no scheduled events");
      now_fs = next;
      now = now_fs / 1000;
      bool evaluate_event = root_clock_used || femtosecond_trace ||
                            now_fs == in_femtoseconds(reset_at) ||
                            now_fs == next_model_time();
      if (now_fs == in_femtoseconds(reset_at)) {
        reset.set(0);
        reset_at = never;
      }
      if (now_fs == in_femtoseconds(root_next)) {
        root_clock.set(!root_clock.get());
        root_next += 500;
        if (root_clock.get()) {
          ++cycles;
          if (!reset.get()) {
            if (max_cycles && cycles > max_cycles)
              fail("timeout after " + std::to_string(cycles) +
                   " simulation cycles");
            // Resolve lazily, at the same first post-reset check as before.
            // Model storage and existing signal entries keep stable addresses.
            if (!success.data)
              success = signal("io_success");
            if (success.get())
              finish();
          }
        }
      }
      for (auto &binding : bindings) {
        DpiContext context(*binding->dpi_scope);
        if (binding->next_time_fs() <= now_fs) {
          binding->timed();
          evaluate_event = true;
        }
      }
      printf_enable.set(!reset.get() && verbose);
      stop_enable.set(!reset.get());
      // The TestDriver clock often has no consumers: generated clock sources
      // drive the hardware instead. Keep its cycle checks and waveform edges,
      // and evaluate the model only for an event that can affect its state.
      if (evaluate_event)
        settle();
      else
        sample_trace();
    }
    if (!error.empty()) {
      std::fprintf(stderr, "*** FAILED *** %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stderr,
                 "*** PASSED *** Completed after %llu simulation cycles\n",
                 (unsigned long long)cycles);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Native Arcilator: %s\n", e.what());
    return 1;
  }
}
} // namespace chipyard_arc
extern "C" void chipyard_arc_assert_fail() {
  chipyard_arc::active_runtime()->fail("RTL assertion failed");
}
extern "C" void chipyard_arc_stop_fail() {
  chipyard_arc::active_runtime()->fail("RTL stopped with failure");
}
extern "C" void chipyard_arc_finish() {
  chipyard_arc::active_runtime()->finish();
}
// Keep each procedural diagnostic guard independent during MLIR rewriting.
extern "C" uint32_t chipyard_arc_guard(uint32_t condition) { return condition; }
