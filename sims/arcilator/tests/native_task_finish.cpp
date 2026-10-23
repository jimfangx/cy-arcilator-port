#include "native/runtime.h"
#include "svdpi.h"
#include <cstdio>
extern "C" int preflight();
extern "C" int finish_now();
extern "C" void __real_chipyard_arc_stop_fail();
static unsigned entries, completed, stops;
static int completion;
static bool discard_status;
extern "C" void __wrap_chipyard_arc_stop_fail() {
  ++stops;
  __real_chipyard_arc_stop_fail();
}
extern "C" int host_preflight() {
  ++entries;
  if (preflight() != 0)
    throw std::runtime_error("normal exported task did not complete normally");
  return 0;
}
extern "C" int host_finish() {
  ++completed;
  auto scope = svGetScope();
  completion = finish_now();
  if (!scope || svGetScope() != scope)
    throw std::runtime_error("export finish changed its C caller scope");
  // Even a C caller that discards the completion must not resume the SV
  // process after the model has successfully finished.
  return discard_status ? 0 : completion;
}
int main(int argc, char **argv) {
  for (unsigned recreation = 0; recreation < 4; ++recreation) {
    entries = completed = stops = 0;
    completion = -1;
    discard_status = recreation % 2;
    auto model = chipyard_arc::create_model(argc, argv);
    int status = model->run();
    if (status || entries != 1 || completed != 1 || stops || completion != 1 ||
        model->signal("preflights").get() != 1 ||
        model->signal("resumed").get() != 0)
      throw std::runtime_error("exported task finish lost success, resumed SV, "
                               "or leaked termination into a new model");
  }
  std::puts("PASS: exported task finish preserves successful model exit, "
            "halts SV for propagated/discarded C completion, restores scope "
            "and clears termination across model recreation");
}
