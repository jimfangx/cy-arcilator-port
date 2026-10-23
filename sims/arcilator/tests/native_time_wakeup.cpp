#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected waveform path");
  std::vector<std::string> arguments{"time-wakeup", "+max-cycles=800",
                                     std::string("+vcdfile=") + argv[1]};
  std::vector<char *> pointers;
  for (auto &argument : arguments)
    pointers.push_back(argument.data());
  {
    auto r = chipyard_arc::create_model(pointers.size(), pointers.data());
    if (r->run() || r->time_fs() != 778500000 || r->signal("ticks").get() != 4)
      throw std::runtime_error(
          "autonomous wakeups or reset completion changed");
  }
  std::ifstream file(argv[1]);
  std::string waveform{std::istreambuf_iterator<char>(file), {}};
  if (waveform.find("$timescale 1fs $end") == std::string::npos)
    throw std::runtime_error("delay-only model lost femtosecond precision");
  for (auto time : {1, 2, 3, 4})
    if (waveform.find("#" + std::to_string(time) + "\n") == std::string::npos)
      throw std::runtime_error("delay-only model lost intermediate wakeups");
  std::puts("PASS: four exact 1 fs wakeups and complete waveform precision "
            "without any source time reads");
}
