#include "native/runtime.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace chipyard_arc;
int main() {
  const std::vector<std::vector<std::string>> cases = {
      {},
      {"+foo"},
      {"+foobar"},
      {"+foo=1"},
      {"+fo"},
      {"+"},
      {"+disable_nv_clk_gate_asserts"},
      {"+disable_nv_clk_gate_asserts_extra"},
      {"+x y"},
      {"+UPPER"},
      {"+upper"},
      {"+π"},
      {"+foo=7", "+UPPER", "+π", "+x y"},
      {"ordinary-argument", "--foo", "foo=1"}};
  const char *keys[] = {"foo", "foo=",  "", "disable_nv_clk_gate_asserts",
                        "x y", "UPPER", "π"};
  unsigned observations = 0;
  for (unsigned repeat = 0; repeat < 2; ++repeat)
    for (auto args : cases) {
      unsigned expected = 0;
      for (unsigned bit = 0; bit < 7; ++bit)
        for (const auto &arg : args)
          if (arg.rfind(std::string("+") + keys[bit], 0) == 0)
            expected |= 1u << bit;
      args.insert(args.begin(), "native-test-plusargs");
      std::vector<char *> argv;
      for (auto &arg : args)
        argv.push_back(arg.data());
      auto m = create_model(argv.size(), argv.data());
      m->initialize();
      m->advance(0, [&] { m->signal("clock").set(0); });
      if (m->signal("initial_matched").get() != expected)
        throw std::runtime_error("time-zero test-plusargs result mismatch");
      for (unsigned cycle = 0; cycle < 16; ++cycle) {
        m->advance(cycle * 2, [&] { m->signal("clock").set(0); });
        m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
        if (m->signal("matched").get() != expected ||
            m->signal("initial_matched").get() != expected)
          throw std::runtime_error(
              "clocked test-plusargs or retained startup result mismatch");
        observations += 14;
      }
    }
  auto empty = create_model(0, nullptr);
  empty->initialize();
  empty->advance(0, [&] { empty->signal("clock").set(0); });
  if (empty->signal("initial_matched").get() != 0)
    throw std::runtime_error("absent VPI argument list must not match");
  std::printf("PASS: %u native SV test-plusargs observations, initial/clocked "
              "processes, prefix/empty/UTF8/case/space matching, absent VPI "
              "arguments and model recreation\n",
              observations);
}
