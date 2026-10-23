#include "native/runtime.h"
#include <cstdio>
using namespace chipyard_arc;
extern "C" {
ArcRuntimeModelInfo arcRuntimeModel_ArcModel{ARC_RUNTIME_API_VERSION, 64, "ArcModel", nullptr};
void ArcModel_eval(void *) {}
}
int main(int argc, char **argv) {
  auto r = create_model(argc, argv);
  auto pad = r->signal("pad");
  r->initialize(); r->settle(); r->advance(1000);
  for (unsigned bit = 0; bit < pad.bits; ++bit)
    if (!((pad.data[bit / 8] >> (bit % 8)) & 1))
      throw std::runtime_error("generated wide AnalogConst lost sign extension");
  if (pad.bits % 8 && (pad.data[pad.bytes() - 1] >> (pad.bits % 8)))
    throw std::runtime_error("generated wide AnalogConst dirty tail bits");
  std::printf("PASS: generated AnalogConst width %u\n", pad.bits);
}
