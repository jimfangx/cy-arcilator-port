// Native lifecycle callbacks. No simulation timebase is inferred here.
#include "vpi_lifecycle.h"
#include "compat.h"
#include "vpi_user.h"
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#if defined(__GNUC__)
extern "C" void (*vlog_startup_routines[])(void) __attribute__((weak));
#endif
namespace chipyard_arc::vpi_lifecycle {
bool active = false;
namespace {
enum class Phase { Pending, Building, Starting, Running, Ending, Ended };
struct Callback {
  PLI_INT32 reason;
  PLI_INT32 (*function)(p_cb_data);
  PLI_BYTE8 *user_data;
};
struct Registry {
  void *owner = nullptr;
  Phase phase = Phase::Pending;
  bool started = false;
  bool finalized = false;
  std::map<uintptr_t, std::shared_ptr<Callback>> handles;
  std::deque<uintptr_t> starts, ends;
};
Registry &registry() {
  static Registry value;
  return value;
}
uintptr_t serial = 0;
void *model_owner = nullptr;
bool model_started = false;
bool startup_called = false;
bool dispatch(Registry &state, std::deque<uintptr_t> &queue) noexcept {
  unsigned waves = 0;
  while (!queue.empty()) {
    if (++waves > 1000) {
      set_vpi_error(vpiError,
                    "native VPI lifecycle callback delta limit exceeded");
      return false;
    }
    // All callbacks already queued belong to one wave. New registrations
    // run in a following wave, with no cap on independent registrations.
    std::deque<uintptr_t> batch;
    batch.swap(queue);
    while (!batch.empty()) {
      uintptr_t id = batch.front();
      batch.pop_front();
      auto found = state.handles.find(id);
      if (found == state.handles.end())
        continue;
      // Keep in-flight data alive even if the callback cancels itself.
      auto callback = found->second;
      s_cb_data data{};
      data.reason = callback->reason;
      data.cb_rtn = callback->function;
      data.user_data = callback->user_data;
      try {
        callback->function(&data);
      } catch (const std::exception &error) {
        state.handles.erase(id);
        set_vpi_error(vpiError, error.what());
        return false;
      } catch (...) {
        state.handles.erase(id);
        set_vpi_error(vpiError, "native VPI callback threw an exception");
        return false;
      }
      state.handles.erase(id);
    }
  }
  return true;
}
} // namespace
void startup() {
  if (startup_called)
    return;
  startup_called = true;
#if defined(__GNUC__)
  if (vlog_startup_routines)
    for (unsigned i = 0; vlog_startup_routines[i]; ++i) {
      if (i == 1024)
        throw std::runtime_error("native VPI startup routine limit exceeded");
      vlog_startup_routines[i]();
    }
#endif
}
void attach(void *owner) {
  if (model_owner && model_owner != owner)
    throw std::runtime_error("another native VPI model is active");
  model_owner = owner;
  model_started = false;
  if (!active)
    return;
  auto &state = registry();
  if (!state.owner) {
    state.owner = owner;
    state.phase = Phase::Building;
  } else if (state.owner != owner)
    throw std::runtime_error("native VPI callbacks belong to another model");
}
bool start(void *owner) noexcept {
  if (model_owner != owner || model_started) {
    set_vpi_error(vpiError, "invalid native VPI start phase");
    return false;
  }
  model_started = true;
  if (!active)
    return true;
  auto &state = registry();
  if (!state.owner) {
    state.owner = owner;
    state.phase = Phase::Building;
  }
  if (state.owner != owner || state.started) {
    set_vpi_error(vpiError, "invalid native VPI start phase");
    return false;
  }
  state.started = true;
  state.phase = Phase::Starting;
  bool ok = dispatch(state, state.starts);
  state.phase = Phase::Running;
  return ok;
}
bool end(void *owner) noexcept {
  if (!active)
    return true;
  auto &state = registry();
  if (state.owner && state.owner != owner) {
    set_vpi_error(vpiError, "invalid native VPI model owner");
    return false;
  }
  if (!state.started || state.phase == Phase::Ended)
    return true;
  state.phase = Phase::Ending;
  bool ok = dispatch(state, state.ends);
  state.phase = Phase::Ended;
  return ok;
}
bool is_finalized(void *owner) noexcept {
  return active && registry().owner == owner && registry().finalized;
}
void mark_finalized(void *owner) noexcept {
  if (active && registry().owner == owner)
    registry().finalized = true;
}
bool finish(void *owner, void (*finalize)(void *), void *data) noexcept {
  if (!active)
    return true;
  auto &state = registry();
  if (state.owner != owner) {
    set_vpi_error(vpiError, "invalid native VPI finalization owner");
    return false;
  }
  if (!state.finalized) {
    state.finalized = true;
    try {
      if (finalize)
        finalize(data);
    } catch (const std::exception &error) {
      state.phase = Phase::Ended;
      set_vpi_error(vpiError, error.what());
      return false;
    } catch (...) {
      state.phase = Phase::Ended;
      set_vpi_error(vpiError, "native RTL finalization failed");
      return false;
    }
  }
  return end(owner);
}
void detach(void *owner) noexcept {
  if (model_owner && model_owner != owner)
    return;
  model_owner = nullptr;
  model_started = false;
  if (!active)
    return;
  auto &state = registry();
  if (state.owner && state.owner != owner)
    return;
  state.handles.clear();
  state.starts.clear();
  state.ends.clear();
  state.owner = nullptr;
  state.finalized = false;
  state.phase = Phase::Pending;
  state.started = false;
  active = false;
}
} // namespace chipyard_arc::vpi_lifecycle
extern "C" vpiHandle vpi_register_cb(p_cb_data data) {
  using namespace chipyard_arc;
  using namespace chipyard_arc::vpi_lifecycle;
  clear_vpi_error();
  if (!data || !data->cb_rtn) {
    set_vpi_error(vpiError, "native VPI callback requires a function");
    return nullptr;
  }
  if (data->reason != cbStartOfSimulation &&
      data->reason != cbEndOfSimulation) {
    set_vpi_error(vpiError, "native VPI callback reason is not implemented");
    return nullptr;
  }
  try {
    auto &state = registry();
    if (!active) {
      state.owner = model_owner;
      state.started = model_started;
      state.phase = !model_owner    ? Phase::Pending
                    : model_started ? Phase::Running
                                    : Phase::Building;
    }
    if (state.phase == Phase::Ended ||
        (data->reason == cbStartOfSimulation && state.phase != Phase::Pending &&
         state.phase != Phase::Building && state.phase != Phase::Starting)) {
      set_vpi_error(vpiError, "native VPI callback phase has already passed");
      return nullptr;
    }
    if (serial == std::numeric_limits<uintptr_t>::max()) {
      set_vpi_error(vpiSystem, "native VPI callback handles exhausted");
      return nullptr;
    }
    uintptr_t id = ++serial;
    auto callback = std::make_shared<Callback>(
        Callback{data->reason, data->cb_rtn, data->user_data});
    state.handles.emplace(id, callback);
    try {
      (data->reason == cbStartOfSimulation ? state.starts : state.ends)
          .push_back(id);
    } catch (...) {
      state.handles.erase(id);
      throw;
    }
    active = true;
    return reinterpret_cast<vpiHandle>(id);
  } catch (const std::exception &error) {
    set_vpi_error(vpiSystem, error.what());
    return nullptr;
  }
}
extern "C" PLI_INT32 vpi_remove_cb(vpiHandle handle) {
  using namespace chipyard_arc;
  using namespace chipyard_arc::vpi_lifecycle;
  clear_vpi_error();
  if (active && registry().handles.erase(reinterpret_cast<uintptr_t>(handle)))
    return 1;
  set_vpi_error(vpiError, "unknown or expired native VPI callback handle");
  return 0;
}
