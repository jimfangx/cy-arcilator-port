#include "native/runtime.h"
#include "svdpi.h"
#include <cstdio>
using namespace chipyard_arc;
static unsigned probes, plain_calls, stops;
extern "C" void __real_chipyard_arc_stop_fail();
extern "C" void __wrap_chipyard_arc_stop_fail() {
  ++stops;
  __real_chipyard_arc_stop_fail();
}
extern "C" int task_probe(int mode, const svBitVecVal *wide, int *result,
                          int *counter) {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || std::string(name) != "TaskAbiTop" ||
      scope != svGetScopeFromName(name))
    throw std::runtime_error("imported task lost physical context scope");
  if (wide[0] != 0xfedcba98U || wide[1] != 0x76543210U || wide[2] != 1 ||
      mode != int(probes))
    throw std::runtime_error("imported task changed scalar/65-bit input ABI");
  if (*counter != (mode ? 12 : 7))
    throw std::runtime_error("imported task changed inout input value");
  ++probes;
  *result = mode ? 999 : 123;
  *counter = mode ? 100 : 12;
  return mode; // Deliberately invalid nonzero completion tests the protocol
               // guard.
}
extern "C" int plain_probe(short data, svBit *result) {
  if (data != -123)
    throw std::runtime_error("plain task changed signed scalar ABI");
  ++plain_calls;
  *result = 1;
  return 0;
}
int main(int argc, char **argv) {
  auto m = create_model(argc, argv);
  m->signal("arc_printf_enable").set(1);
  m->signal("arc_stop_enable").set(1);
  m->initialize();
  m->advance(0, [] {});
  if (probes != 2 || plain_calls != 1 || stops != 1 ||
      m->signal("after").get() != 1)
    throw std::runtime_error(
        "task status did not stop the invalid call's continuation");
  auto callbacks = probes;
  m->advance(1, [] {});
  if (probes != callbacks || stops != 1 || m->signal("after").get() != 1)
    throw std::runtime_error("terminated task caller restarted");
  std::puts("PASS: imported task C int status, context/plain calls, "
            "signed/65-bit/output/inout ABI and invalid-status process halt");
}
