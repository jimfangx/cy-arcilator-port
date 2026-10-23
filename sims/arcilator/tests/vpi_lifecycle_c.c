#include "vpi_user.h"
#include <stddef.h>
_Static_assert(sizeof(s_vpi_vlog_info) == 32, "legacy argument ABI changed");
_Static_assert(sizeof(s_vpi_time) == 24, "VPI time ABI layout");
_Static_assert(sizeof(s_cb_data) == 56, "VPI callback ABI layout");
_Static_assert(offsetof(s_cb_data, user_data) == 48,
               "VPI callback user data ABI");
_Static_assert(sizeof(s_vpi_error_info) == 48, "VPI error ABI layout");
static PLI_INT32 from_c(p_cb_data data) {
  if (data->reason != cbEndOfSimulation)
    return -1;
  ++*(unsigned *)data->user_data;
  return 0;
}
vpiHandle register_from_c(unsigned *count) {
  s_cb_data data = {0};
  data.reason = cbEndOfSimulation;
  data.cb_rtn = from_c;
  data.user_data = (PLI_BYTE8 *)count;
  return vpi_register_cb(&data);
}
