// VPI argument-query ABI used by Chipyard's native C/C++ peripheral drivers.
// This is an Arcilator compatibility interface, not a general VPI implementation.
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
#ifdef __cplusplus
}
#endif
