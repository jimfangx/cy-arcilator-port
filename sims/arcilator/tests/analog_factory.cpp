#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace chipyard_arc;
extern "C" {
ArcRuntimeModelInfo arcRuntimeModel_ArcModel{ARC_RUNTIME_API_VERSION, 64,
                                             "ArcModel", nullptr};
void ArcModel_eval(void *) {}
}
int main(int argc, char **argv) {
  auto r = create_model(argc, argv);
  auto pad = r->signal("pad");
  r->initialize();
  r->settle();
  r->advance(1000);
  std::vector<uint8_t> expected(pad.bytes(), 255);
  if (pad.bits % 8)
    expected.back() &= (1u << (pad.bits % 8)) - 1;
  auto filename = r->option("analog-expected-file=");
  if (!filename.empty()) {
    std::ifstream input(filename, std::ios::binary);
    if (!input)
      throw std::runtime_error("cannot open analog constant oracle");
    expected.assign(std::istreambuf_iterator<char>(input), {});
  }
  if (pad.copy() != expected)
    throw std::runtime_error("generated AnalogConst value mismatch");
  if (pad.bits % 8 && (pad.data[pad.bytes() - 1] >> (pad.bits % 8)))
    throw std::runtime_error("generated wide AnalogConst dirty tail bits");
  std::printf("PASS: generated AnalogConst width %u\n", pad.bits);
}
