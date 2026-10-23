#pragma once
#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "compat.h"
#include "trace.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
namespace chipyard_arc {
constexpr uint64_t never = std::numeric_limits<uint64_t>::max();
struct Signal {
  uint8_t *data;
  unsigned bits;
  size_t bytes() const { return (size_t(bits) + 7) / 8; }
  uint64_t get() const {
    if (bits <= 8)
      return bits ? data[0] & ((1u << bits) - 1) : 0;
    if (bits > 64)
      throw std::runtime_error("scalar access to wide native port");
    uint64_t value = 0;
    std::memcpy(&value, data, bytes());
    return bits < 64 ? value & ((uint64_t(1) << bits) - 1) : value;
  }
  bool set(uint64_t value) const {
    if (bits > 64)
      throw std::runtime_error("scalar assignment to wide native port");
    if (bits < 64)
      value &= (uint64_t(1) << bits) - 1;
    bool changed = std::memcmp(data, &value, bytes());
    std::memcpy(data, &value, bytes());
    return changed;
  }
  bool assign(const std::vector<uint8_t> &value) const {
    if (value.size() != bytes())
      throw std::runtime_error("native port width mismatch");
    auto clean = value;
    if (bits % 8)
      clean.back() &= (1u << (bits % 8)) - 1;
    bool changed = std::memcmp(data, clean.data(), bytes());
    std::memcpy(data, clean.data(), bytes());
    return changed;
  }
  std::vector<uint8_t> copy() const {
    std::vector<uint8_t> value(data, data + bytes());
    if (bits % 8)
      value.back() &= (1u << (bits % 8)) - 1;
    return value;
  }
};
struct Parameter {
  std::string name, value;
};
class Runtime;
template <class T> struct BindingCallbackContract {
  uint8_t inherited = 0;
};
// Opt-in assertion: review that callback member lookup exposes the final
// virtual overriders. An inherited declaration is not a contract for a derived
// type.
template <class T>
constexpr BindingCallbackContract<T> reviewed_base_callback_contract();
template <class T, class... Args>
std::unique_ptr<T> make_binding_instance(Args &&...args);
class Binding {
protected:
  Runtime &runtime;
  bool old_clock = false, old_reset = false;

private:
  // Exact construction can prove that a virtual callback is the inherited
  // base implementation. Erased/custom factories retain all callbacks.
  enum BaseCallback : uint8_t {
    BaseCombinational = 1,
    BaseSample = 2,
    BaseTimers = 4,
    BaseCommit = 8
  };
  uint8_t base_callbacks = 0;
  template <class T, class... Args>
  friend std::unique_ptr<T> make_binding_instance(Args &&...args);
  template <class T>
  friend constexpr BindingCallbackContract<T> reviewed_base_callback_contract();

protected:
  std::vector<std::pair<Signal, std::vector<uint8_t>>> pending;
  bool internal_changed = false;

public:
  std::string module;
  std::string instance_name;
  std::unique_ptr<DpiScope> dpi_scope;
  std::map<std::string, Signal> ports;
  std::map<std::string, std::string> parameters;
  Binding(Runtime &r, std::string name) : runtime(r), module(std::move(name)) {}
  virtual ~Binding() = default;
  Signal port(const std::string &name) const { return ports.at(name); }
  uint64_t get(const std::string &name) const { return port(name).get(); }
  uint64_t number(const std::string &name, uint64_t fallback = 0) const;
  std::string text(const std::string &name,
                   const std::string &fallback = "") const;
  void stage(const std::string &name, uint64_t value);
  void stage(const std::string &name, const std::vector<uint8_t> &value);
  virtual void configure() {}
  virtual void initialize() {}
  // Opt-in contract: combinational() is idempotent at unchanged inputs and
  // reports every change it makes to compiled-model inputs. Calls into the
  // compiled model or hidden output readback require the default fallback.
  virtual bool output_convergence_safe() const { return false; }
  virtual bool combinational() { return false; }
  virtual void sample() {}
  virtual uint64_t next_time() const { return never; }
  virtual uint64_t next_time_fs() const;
  virtual void timed() {}
  void drive(const std::string &name, uint64_t value, uint64_t mask);
  // Little endian bytes; mask bits select actively driven port bits. Both
  // vectors must match the port's byte count. Unused high bits are ignored.
  // Reusing a binding/port replaces its previous drive.
  void drive(const std::string &name, const std::vector<uint8_t> &value,
             const std::vector<uint8_t> &mask);
  void mark_changed() { internal_changed = true; }
  virtual bool commit();
  bool needs_combinational_callback() const {
    return !(base_callbacks & BaseCombinational);
  }
  bool needs_sample_callback() const { return !(base_callbacks & BaseSample); }
  bool needs_time_polling() const { return !(base_callbacks & BaseTimers); }
  bool needs_commit_callback() const {
    return !(base_callbacks & BaseCommit) || internal_changed ||
           !pending.empty();
  }
};
namespace detail {
template <class T, class = void>
struct PublicBindingCallbacks : std::false_type {};
template <class T>
struct PublicBindingCallbacks<
    T, std::void_t<decltype(&T::combinational), decltype(&T::sample),
                   decltype(&T::next_time), decltype(&T::next_time_fs),
                   decltype(&T::commit)>> : std::true_type {};
} // namespace detail
namespace detail {
template <class T, class = void>
struct OwnBindingCallbackContract : std::false_type {};
template <class T>
struct OwnBindingCallbackContract<
    T, std::void_t<decltype(&T::native_callback_contract)>>
    : std::is_same<decltype(&T::native_callback_contract),
                   BindingCallbackContract<T> (T::*)() const> {};
template <class T>
constexpr uint8_t declared_binding_callbacks(const T &binding) {
  if constexpr (OwnBindingCallbackContract<T>::value)
    return binding.native_callback_contract().inherited;
  return 0;
}
} // namespace detail
// This factory never infers a contract from an arbitrary user's inheritance
// graph. Only a declaration owned by the exact constructed type opts in.
template <class T>
constexpr BindingCallbackContract<T> reviewed_base_callback_contract() {
  if constexpr (detail::PublicBindingCallbacks<T>::value) {
    constexpr uint8_t facts =
        (std::is_same_v<decltype(&T::combinational),
                        decltype(&Binding::combinational)>
             ? Binding::BaseCombinational
             : 0) |
        (std::is_same_v<decltype(&T::sample), decltype(&Binding::sample)>
             ? Binding::BaseSample
             : 0) |
        (std::is_same_v<decltype(&T::next_time),
                        decltype(&Binding::next_time)> &&
                 std::is_same_v<decltype(&T::next_time_fs),
                                decltype(&Binding::next_time_fs)>
             ? Binding::BaseTimers
             : 0) |
        (std::is_same_v<decltype(&T::commit), decltype(&Binding::commit)>
             ? Binding::BaseCommit
             : 0);
    return {facts};
  }
  return {};
}
template <class T, class... Args>
std::unique_ptr<T> make_binding_instance(Args &&...args) {
  static_assert(std::is_convertible_v<T *, Binding *>);
  auto binding = std::make_unique<T>(std::forward<Args>(args)...);
  static_cast<Binding &>(*binding).base_callbacks =
      detail::declared_binding_callbacks(*binding);
  return binding;
}
using Factory =
    std::function<std::unique_ptr<Binding>(Runtime &, const std::string &)>;
void register_binding(const std::string &module, Factory factory);
void register_builtin_bindings();
std::unique_ptr<Binding> make_binding(Runtime &, const std::string &);

using ExportBytes = std::vector<uint8_t>;
using ExportKernel = void (*)(uint8_t **, uint64_t *);
class Runtime {
  struct ExportTarget {
    ExportKernel entry;
    std::string layout;
    unsigned formals, result_bits;
    std::vector<unsigned> widths;
    std::vector<bool> references;
    std::vector<Signal> captures;
  };
  std::map<std::pair<std::string, std::string>, ExportTarget> dpi_exports;
  std::map<std::string, std::unique_ptr<DpiScope>> export_scopes;
  std::map<std::pair<std::string, std::string>, DpiScope *> context_roots;
  std::map<std::string, uint64_t> native_strings;
  std::map<uint64_t, const std::string *> string_handles;
  unsigned evaluation_depth = 0, export_depth = 0;
  const ExportTarget &find_export(const std::string &) const;
  bool evaluate_state(ArcState *, bool clock_transition = false,
                      bool identity = false);
  Trace trace;
  ArcRuntimeModelInfo &info;
  DpiScope root_scope;
  void (*evaluate)(void *);
  uint32_t (*idle_clock_model)(void *) = nullptr;
  uint32_t (*stable_combinational_model)(void *) = nullptr;
  void (*finalize)(void *);
  void (*initial_model)(void *);
  ArcState *current, *staged;
  std::map<std::string, Signal> signals;
  struct AnalogNet {
    std::vector<uint8_t> storage;
    unsigned bits;
    std::map<std::pair<const Binding *, std::string>,
             std::pair<uint64_t, uint64_t>>
        drivers;
  };
  std::map<std::string, AnalogNet> analog_nets;
  std::map<uint8_t *, AnalogNet *> analog_addresses;
  bool resolve_analog();
  std::vector<std::unique_ptr<Binding>> bindings;
  // Entries refer to owned bindings. Sharing ends with this Runtime, so
  // independent simulations never reuse a package variable's contents.
  std::map<std::string, Binding *> signal_memories;
  std::vector<std::pair<unsigned, unsigned>> clocks;
  std::vector<uint8_t> previous_outputs;
  struct OutputRange {
    unsigned offset, bytes;
    size_t snapshot;
  };
  std::vector<OutputRange> output_ranges;
  std::set<std::string> output_names, certified_output_names;
  std::vector<std::pair<size_t, size_t>> write_only_ranges, comparison_ranges;
  bool convergence_bindings_safe = true, filtered_convergence = false;
  bool callback_schedule_enabled = true;
  void prepare_convergence();
  bool model_state_changed() const;

  std::vector<std::string> arguments;
  std::string error;
  bool finished = false;
  bool initialized = false;
  bool root_clock_used = true;
  uint64_t now = 0;
  uint64_t now_fs = 0;
  void (*set_model_time)(void *, uint64_t) = nullptr;
  uint64_t (*get_model_wakeup)(void *) = nullptr;
  bool femtosecond_trace = false;
  size_t sampling_offset = std::numeric_limits<size_t>::max();
  // Wide nets are allocated only when needed. The existing scalar storage
  // and resolver remain independent of their representation.
  struct WideAnalogState;
  struct WideAnalogDeleter {
    void operator()(WideAnalogState *) const;
  };
  std::unique_ptr<WideAnalogState, WideAnalogDeleter> wide_analog;
  void add_wide_analog_signal(const std::string &, unsigned);
  bool resolve_wide_analog();
  uint64_t next_model_time() const;
  void sample_trace() { trace.sample(femtosecond_trace ? now_fs : now); }
  // Native selection keeps binding/observation fallback controls in force.
  void (*stable_evaluator)(void *) = nullptr;
  std::vector<Binding *> combinational_bindings, sample_bindings,
      timed_bindings;

public:
  uint64_t intern_string(const std::string &);
  const std::string &scope_name() const { return root_scope.name(); }
  DpiScope &owned_scope(const std::string &);
  void add_context_root(const std::string &, const std::string &);
  DpiScope &context_scope(const std::string &, const std::string &, unsigned,
                          const std::string &);
  const std::string &string_value(uint64_t) const;
  Runtime(int argc, char **argv, ArcRuntimeModelInfo &, void (*eval)(void *),
          void (*initial)(void *) = nullptr, void (*final)(void *) = nullptr,
          void (*time_setter)(void *, uint64_t) = nullptr,
          uint64_t (*wakeup_getter)(void *) = nullptr,
          bool time_readers = false, const char *scope_name = nullptr);
  ~Runtime();
  Signal signal(const std::string &name) const { return signals.at(name); }
  void add_signal(const std::string &name, unsigned offset, unsigned bits,
                  bool output);
  void add_write_only_output(const std::string &name);
  void add_analog_signal(const std::string &name, unsigned bits);
  void drive_analog(const Binding *binding, const std::string &name,
                    Signal signal, uint64_t value, uint64_t mask);
  void drive_analog(const Binding *binding, const std::string &name,
                    Signal signal, const std::vector<uint8_t> &value,
                    const std::vector<uint8_t> &mask);
  void add_trace_signal(const std::string &name, unsigned offset,
                        unsigned bits);
  void set_root_clock_used(bool used) { root_clock_used = used; }
  void add_clock(const std::string &held, const std::string &observed);
  void add_sampling_signal(const std::string &name);
  void add_idle_clock_model(uint32_t (*)(void *),
                            const std::vector<std::string> &inputs,
                            uint32_t (*stable)(void *) = nullptr,
                            void (*stable_eval)(void *) = nullptr);
  void add_binding(std::unique_ptr<Binding> b);
  Binding *share_signal_memory(const std::string &key, Binding *candidate) {
    return signal_memories.emplace(key, candidate).first->second;
  }
  void add_export(const std::string &scope, const std::string &name,
                  ExportKernel entry, unsigned formals, unsigned result_bits,
                  std::vector<unsigned> widths, std::vector<bool> references,
                  std::vector<Signal> captures, std::string layout = {});
  unsigned export_width(const std::string &name, unsigned index) const;
  const std::string &export_layout(const std::string &name) const;
  void invoke_export(const std::string &name, std::vector<ExportBytes> &values);
  uint64_t time() const { return now; }
  uint64_t time_fs() const { return now_fs; }
  bool flag(const std::string &name) const;
  std::string option(const std::string &prefix,
                     const std::string &fallback = "") const;
  void fail(std::string reason) { error = std::move(reason); }
  void finish() { finished = true; }
  void settle();
  void initialize();
  void advance(uint64_t target, const std::function<void()> &drive = {});
  void advance_fs(uint64_t target, const std::function<void()> &drive = {});
  int run();
};
std::unique_ptr<Runtime> create_model(int argc, char **argv);
Runtime *active_runtime();
} // namespace chipyard_arc
