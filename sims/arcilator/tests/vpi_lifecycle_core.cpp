#include "compat.h"
#include "vpi_lifecycle.h"
#include "vpi_user.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using chipyard_arc::vpi_lifecycle::attach;
using chipyard_arc::vpi_lifecycle::detach;
using chipyard_arc::vpi_lifecycle::end;
using chipyard_arc::vpi_lifecycle::start;
static std::vector<int> trace;
static vpiHandle cancelled, self_handle;
static void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
static PLI_INT32 record(p_cb_data data) {
  check(!data->obj && !data->time && !data->value,
        "lifecycle callback used ignored data");
  trace.push_back(int(reinterpret_cast<intptr_t>(data->user_data)));
  return 73; // Callback return values do not control callback scheduling.
}
static vpiHandle add(int reason, int tag, PLI_INT32 (*fn)(p_cb_data) = record) {
  s_cb_data data{};
  data.reason = reason;
  data.cb_rtn = fn;
  data.user_data = reinterpret_cast<PLI_BYTE8 *>(intptr_t(tag));
  // These lifecycle fields are ignored, not borrowed or dereferenced.
  data.obj = reinterpret_cast<vpiHandle>(uintptr_t(0x1234));
  data.time = reinterpret_cast<p_vpi_time>(uintptr_t(0x1234));
  data.value = reinterpret_cast<p_vpi_value>(uintptr_t(0x1234));
  auto handle = vpi_register_cb(&data);
  check(handle, "callback registration failed");
  data.reason = 999;
  data.cb_rtn = nullptr;
  data.user_data = nullptr;
  return handle;
}
static PLI_INT32 nested(p_cb_data data) {
  record(data);
  check(vpi_remove_cb(cancelled) == 1, "pending callback cancellation failed");
  add(cbStartOfSimulation, 3);
  add(cbEndOfSimulation, 5);
  return 0;
}
static PLI_INT32 self_remove(p_cb_data data) {
  record(data);
  check(vpi_remove_cb(self_handle) == 1, "self cancellation failed");
  return 0;
}
static PLI_INT32 repeat(p_cb_data) {
  add(cbStartOfSimulation, 0, repeat);
  return 0;
}
static PLI_INT32 throwing(p_cb_data) {
  throw std::runtime_error("intentional callback failure");
}
extern "C" vpiHandle register_from_c(unsigned *);
int main() {
  s_vpi_vlog_info info{};
  check(!vpi_get_vlog_info(&info), "unregistered command arguments accepted");
  chipyard_arc::command_args(0, nullptr);
  check(vpi_get_vlog_info(&info) == 1 && info.argc == 0 && !info.argv &&
            info.product && info.version && !vpi_chk_error(nullptr),
        "registered empty command arguments rejected");
  char *empty_argv[] = {nullptr};
  chipyard_arc::command_args(0, empty_argv);
  check(vpi_get_vlog_info(&info) == 1 && info.argc == 0 &&
            info.argv == empty_argv,
        "zero-count borrowed argv identity changed");
  unsigned c_calls = 0;
  check(register_from_c(&c_calls), "C callback registration failed");
  int first = 0, second = 0;
  check(!vpi_register_cb(nullptr), "null callback accepted");
  s_vpi_error_info error{};
  check(vpi_chk_error(&error) == vpiError && error.message,
        "missing callback error");
  s_cb_data unsupported{};
  unsupported.cb_rtn = record;
  unsupported.reason = cbAfterDelay;
  check(!vpi_register_cb(&unsupported) && vpi_chk_error(nullptr) == vpiError,
        "unimplemented time reason accepted");
  auto expired = add(cbStartOfSimulation, 1, nested);
  cancelled = add(cbStartOfSimulation, 99);
  self_handle = add(cbStartOfSimulation, 2, self_remove);
  add(cbEndOfSimulation, 4);
  attach(&first);
  check(start(&first), "start failed");
  check(trace == std::vector<int>({1, 2, 3}),
        "copy/cancel/registration order changed");
  check(!vpi_remove_cb(expired), "delivered handle remained live");
  s_cb_data late{};
  late.reason = cbStartOfSimulation;
  late.cb_rtn = record;
  check(!vpi_register_cb(&late), "late start registration accepted");
  check(end(&first) && end(&first), "end failed");
  check(trace == std::vector<int>({1, 2, 3, 4, 5}),
        "end ordering or once-only delivery changed");
  check(c_calls == 1, "C callback ABI was not delivered");
  detach(&first);
  check(!chipyard_arc::vpi_lifecycle::active, "context cleanup failed");
  attach(&second);
  check(start(&second), "empty model start failed");
  add(cbEndOfSimulation, 6);
  check(end(&second), "late first registration lost end event");
  detach(&second);
  check(trace.back() == 6 && !vpi_remove_cb(expired),
        "recreation or stale handles failed");
  attach(&first);
  add(cbEndOfSimulation, 99);
  detach(&first); // Never initialized.
  check(trace.back() == 6, "unstarted model delivered end callback");
  attach(&first);
  for (unsigned i = 0; i < 2001; ++i)
    add(cbStartOfSimulation, 10);
  auto count = trace.size();
  check(start(&first), "independent callback registrations were capped");
  check(trace.size() == count + 2001, "independent lifecycle callbacks lost");
  end(&first);
  detach(&first);
  attach(&first);
  add(cbStartOfSimulation, 7, throwing);
  check(!start(&first), "throwing callback accepted");
  check(vpi_chk_error(&error) == vpiError, "callback exception lacked error");
  end(&first);
  detach(&first);
  attach(&first);
  add(cbStartOfSimulation, 0, repeat);
  check(!start(&first), "unbounded lifecycle callback chain accepted");
  detach(&first);
  char name[] = "core-vpi", *argv[] = {name};
  chipyard_arc::command_args(1, argv);
  check(vpi_get_vlog_info(&info) == 1 && info.argc == 1 && info.argv == argv &&
            !vpi_chk_error(nullptr),
        "legacy argument ABI or error reset changed");
  chipyard_arc::command_args(0, nullptr);
  check(vpi_get_vlog_info(&info) == 1 && info.argc == 0 && !info.argv,
        "argument recreation retained an earlier command line");
  chipyard_arc::command_args(1, argv);
  check(!vpi_get_vlog_info(nullptr) && vpi_get_vlog_info(&info) == 1 &&
            info.argc == 1 && info.argv == argv,
        "null query or argument restoration changed");
  std::puts("PASS: native VPI lifecycle copy, phase, cancellation, reentrancy, "
            "lifetime, stale handles, exceptions, bounded chains and legacy "
            "argument ABI including empty command lines");
}
