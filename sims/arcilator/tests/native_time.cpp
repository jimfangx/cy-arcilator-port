#include "native/runtime.h"
#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected timed model waveform path");
  std::vector<std::string> arguments{"timed-model",
                                     std::string("+vcdfile=") + argv[1]};
  std::vector<char *> pointers;
  for (auto &argument : arguments)
    pointers.push_back(argument.data());
  {
    auto runtime = chipyard_arc::create_model(pointers.size(), pointers.data());
    runtime->initialize();
    runtime->advance(0);
    for (auto ps : {1ULL, 2ULL, 3ULL, 5ULL, 6ULL, 10ULL, 12ULL, 20ULL}) {
      runtime->advance(ps);
      uint64_t count = std::min<uint64_t>(ps * 1000 / 1500, 4);
      if (runtime->time_fs() != ps * 1000 || runtime->time() != ps ||
          runtime->signal("ticks").get() != count ||
          runtime->signal("stamp").get() != count * 1500 ||
          runtime->signal("held_stamp").get() != (ps < 5 ? 0 : 2000) ||
          runtime->signal("clocked").get() !=
              std::min<uint64_t>((ps * 1000 + 1500) / 3000, 4))
        throw std::runtime_error(
            "timed coroutine wakeup, context time, or halt mismatch");
    }
  }
  std::ifstream file(argv[1]);
  std::string waveform{std::istreambuf_iterator<char>(file), {}};
  if (waveform.find("$timescale 1fs $end") == std::string::npos)
    throw std::runtime_error(
        "timed coroutine waveform lacks femtosecond precision");
  for (auto time : {1500, 3000, 4500, 6000, 7500, 9000, 10500, 12000})
    if (waveform.find("#" + std::to_string(time) + "\n") == std::string::npos)
      throw std::runtime_error(
          "timed coroutine waveform lost an intermediate wakeup");
  std::vector<std::string> run_arguments{"timed-run", "+max-cycles=800"};
  pointers.clear();
  for (auto &argument : run_arguments)
    pointers.push_back(argument.data());
  auto runtime = chipyard_arc::create_model(pointers.size(), pointers.data());
  if (runtime->run() || runtime->signal("ticks").get() != 4 ||
      runtime->signal("stamp").get() != 6000 ||
      runtime->signal("held_stamp").get() != 2000 ||
      runtime->signal("clocked").get() != 0)
    throw std::runtime_error(
        "standalone scheduler omitted autonomous coroutine events");
  std::puts("PASS: 1500 fs wakeups, context time, clocked registers, halt, and "
            "exact waveforms");
}
