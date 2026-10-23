#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>

static unsigned evaluations = 0;
extern "C" void __real_ArcModel_eval(void *);
extern "C" void __wrap_ArcModel_eval(void *state) { ++evaluations; __real_ArcModel_eval(state); }
static std::string read(const std::string &path) {
  std::ifstream file(path); return {(std::istreambuf_iterator<char>(file)), {}};
}
int main(int argc, char **argv) {
  if (argc != 2) throw std::runtime_error("expected native waveform output directory");
  unsigned baseline = 0, optimized = 0;
  for (bool force : {true, false}) {
    std::string path = std::string(argv[1]) + (force ? "/baseline.vcd" : "/optimized.vcd");
    std::vector<std::string> arguments{"unused-clock", "+max-cycles=1000", "+vcdfile=" + path};
    std::vector<char *> pointers;
    for (auto &argument : arguments) pointers.push_back(argument.data());
    auto r = chipyard_arc::create_model(pointers.size(), pointers.data());
    if (force) r->set_root_clock_used(true);
    evaluations = 0;
    if (r->run() || r->time() != 786500 || r->signal("count").get() != 3)
      throw std::runtime_error("unused-root-clock scheduling changed completion or reset timing");
    (force ? baseline : optimized) = evaluations;
  }
  if (optimized >= baseline || read(std::string(argv[1]) + "/baseline.vcd") != read(std::string(argv[1]) + "/optimized.vcd"))
    throw std::runtime_error("unused-root-clock optimization changed the waveform or failed to eliminate evaluations");
  std::printf("PASS: unused root clock preserves reset, completion, and complete waveforms (%u -> %u evaluations)\n", baseline, optimized);
}
