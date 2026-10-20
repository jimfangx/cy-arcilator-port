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
#include <stdexcept>
#include <string>
#include <vector>
namespace chipyard_arc {
constexpr uint64_t never = std::numeric_limits<uint64_t>::max();
struct Signal {
  uint8_t *data;
  unsigned bits;
  size_t bytes() const { return (bits + 7) / 8; }
  uint64_t get() const {
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
class Binding {
protected:
  Runtime &runtime;
  bool old_clock = false, old_reset = false;
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
  virtual bool combinational() { return false; }
  virtual void sample() {}
  virtual uint64_t next_time() const { return never; }
  virtual uint64_t next_time_fs() const;
  virtual void timed() {}
  void drive(const std::string &name, uint64_t value, uint64_t mask);
  void mark_changed() { internal_changed = true; }
  virtual bool commit();
};
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
  void evaluate_state(ArcState *);
  Trace trace;
  ArcRuntimeModelInfo &info;
  DpiScope root_scope;
  void (*evaluate)(void *);
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
  uint64_t next_model_time() const;
  void sample_trace() { trace.sample(femtosecond_trace ? now_fs : now); }

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
  void add_analog_signal(const std::string &name, unsigned bits);
  void drive_analog(const Binding *binding, const std::string &name,
                    Signal signal, uint64_t value, uint64_t mask);
  void add_trace_signal(const std::string &name, unsigned offset,
                        unsigned bits);
  void set_root_clock_used(bool used) { root_clock_used = used; }
  void add_clock(const std::string &held, const std::string &observed);
  void add_sampling_signal(const std::string &name);
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
