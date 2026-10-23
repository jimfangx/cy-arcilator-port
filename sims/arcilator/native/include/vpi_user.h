// Native VPI argument queries and simulation lifecycle callback ABI.
// Timed/value callbacks and hierarchy access require additional model metadata.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int32_t PLI_INT32;
typedef char PLI_BYTE8;
typedef struct t_vpi_vlog_info {
  PLI_INT32 argc;
  PLI_BYTE8 **argv;
  PLI_BYTE8 *product;
  PLI_BYTE8 *version;
} s_vpi_vlog_info, *p_vpi_vlog_info;
PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info info);
typedef uint32_t PLI_UINT32;
typedef struct __vpiHandle *vpiHandle;
typedef struct t_vpi_time {
  PLI_INT32 type;
  PLI_UINT32 high, low;
  double real;
} s_vpi_time, *p_vpi_time;
typedef struct t_vpi_value *p_vpi_value;
typedef struct t_cb_data {
  PLI_INT32 reason;
  PLI_INT32 (*cb_rtn)(struct t_cb_data *);
  vpiHandle obj;
  p_vpi_time time;
  p_vpi_value value;
  PLI_INT32 index;
  PLI_BYTE8 *user_data;
} s_cb_data, *p_cb_data;
#define vpiScaledRealTime 1
#define vpiSimTime 2
#define vpiSuppressTime 3
#define cbValueChange 1
#define cbReadWriteSynch 6
#define cbReadOnlySynch 7
#define cbNextSimTime 8
#define cbAfterDelay 9
#define cbStartOfSimulation 11
#define cbEndOfSimulation 12
typedef struct t_vpi_error_info {
  PLI_INT32 state, level;
  PLI_BYTE8 *message, *product, *code, *file;
  PLI_INT32 line;
} s_vpi_error_info, *p_vpi_error_info;
#define vpiCompile 1
#define vpiPLI 2
#define vpiRun 3
#define vpiNotice 1
#define vpiWarning 2
#define vpiError 3
#define vpiSystem 4
#define vpiInternal 5
vpiHandle vpi_register_cb(p_cb_data data);
PLI_INT32 vpi_remove_cb(vpiHandle callback);
PLI_INT32 vpi_chk_error(p_vpi_error_info info);

#ifdef __cplusplus
}
#endif
