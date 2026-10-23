#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>

static unsigned evaluations;
static std::vector<unsigned> calls;
extern "C" void __real_ArcModel_eval(void *);
extern "C" void __wrap_ArcModel_eval(void *state) {
  ++evaluations;
  __real_ArcModel_eval(state);
}
extern "C" int scheduled_dpi_record(unsigned char count) {
  calls.push_back(count);
  return count * 7 + 1;
}
static std::string read(const std::string &path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected waveform directory");
  unsigned baseline = 0, optimized = 0;
  for (bool force : {true, false}) {
    std::string path =
        std::string(argv[1]) + (force ? "/baseline.vcd" : "/optimized.vcd");
    std::vector<std::string> arguments{"scheduled-dpi", "+max-cycles=1000",
                                       "+vcdfile=" + path};
    std::vector<char *> pointers;
    for (auto &argument : arguments)
      pointers.push_back(argument.data());
    calls.clear();
    auto r = chipyard_arc::create_model(pointers.size(), pointers.data());
    if (force)
      r->set_root_clock_used(true);
    evaluations = 0;
    if (r->run() || r->time() != 786500 || r->signal("count").get() != 3 ||
        r->signal("observed").get() != 22 ||
        calls != std::vector<unsigned>({0, 1, 2, 3}))
      throw std::runtime_error("DPI activation, return, or completion changed");
    (force ? baseline : optimized) = evaluations;
  }
  if (optimized >= baseline ||
      read(std::string(argv[1]) + "/baseline.vcd") !=
          read(std::string(argv[1]) + "/optimized.vcd"))
    throw std::runtime_error(
        "scheduler reads still force polling or alter VCD");
  std::printf(
      "PASS: four exact DPI calls/returns, reset/completion and complete "
      "waveforms; scheduler polling reduced %u -> %u evaluations\n",
      baseline, optimized);
}
