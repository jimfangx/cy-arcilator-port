#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected waveform path");
  std::vector<std::string> arguments{"time-output", "+max-cycles=1000",
                                     std::string("+vcdfile=") + argv[1]};
  std::vector<char *> pointers;
  for (auto &argument : arguments)
    pointers.push_back(argument.data());
  {
    auto r = chipyard_arc::create_model(pointers.size(), pointers.data());
    if (r->run() || r->time_fs() != 780500000 ||
        r->signal("stamp").get() != r->time_fs())
      throw std::runtime_error("source time output stopped updating");
  }
  std::ifstream file(argv[1]);
  std::string waveform{std::istreambuf_iterator<char>(file), {}};
  if (waveform.find("$timescale 1fs $end") == std::string::npos ||
      waveform.find("#780500000\n") == std::string::npos)
    throw std::runtime_error("source time output lost precision or final edge");
  std::puts("PASS: source time output updates on unused root edges with exact "
            "femtosecond waveform precision");
}
