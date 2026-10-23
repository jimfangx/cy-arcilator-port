// Exercise the real SimJTAG binding with the original jtag_tick C ABI.
// Expected times/call sequences come from SimJTAG.v, not the native scheduler.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace chipyard_arc;
namespace {
struct Pins {
  unsigned char tck, tms, tdi, trst;
  uint32_t exit;
};
struct Call {
  uint64_t time;
  unsigned char tdo;
};
std::vector<Pins> script;
std::vector<Call> calls;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
void evaluate(void *) {}
struct Probe {
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "JtagProbe", nullptr};
  Runtime runtime{0, nullptr, info, evaluate};
  Binding *binding = nullptr;
  Probe(unsigned divider = 0) {
    unsigned offset = 24;
    auto b = make_binding(runtime, "SimJTAG");
    require(bool(b), "SimJTAG factory missing");
    b->instance_name = "debug";
    b->parameters["TICK_DELAY"] = std::to_string(divider);
    for (const auto *name : {"clock", "reset", "enable", "init_done",
                             "jtag_TCK", "jtag_TMS", "jtag_TDI", "jtag_TRSTn",
                             "jtag_TDO_data", "jtag_TDO_driven", "exit"}) {
      unsigned bits = std::string(name) == "exit" ? 32 : 1;
      runtime.add_signal(name, offset, bits, false);
      offset += (bits + 7) / 8;
      b->ports[name] = runtime.signal(name);
    }
    binding = b.get();
    runtime.add_binding(std::move(b));
    runtime.initialize();
    at(0,
       {{"enable", 1}, {"init_done", 1}, {"reset", 1}, {"jtag_TDO_driven", 1}});
  }
  void
  at(uint64_t fs,
     std::initializer_list<std::pair<const char *, uint64_t>> values = {}) {
    runtime.advance_fs(fs, [&] {
      for (auto value : values)
        runtime.signal(value.first).set(value.second);
    });
    require(svGetScope() == nullptr, "JTAG callback leaked DPI scope");
  }
  void arm() {
    at(1000, {{"clock", 1}});
    at(3000, {{"clock", 0}, {"reset", 0}});
    at(5000, {{"clock", 1}}); // Previous reset remains asserted for one edge.
    at(6000, {{"clock", 0}});
    at(7000, {{"clock", 1}}); // Sticky init_done is visible next edge.
    at(8000, {{"clock", 0}});
    require(calls.empty(),
            "reset/sticky initialization called jtag_tick early");
  }
  void pins(Pins expected, const char *where) {
    const std::array<std::pair<const char *, uint64_t>, 5> values{
        {{"jtag_TCK", expected.tck},
         {"jtag_TMS", expected.tms},
         {"jtag_TDI", expected.tdi},
         {"jtag_TRSTn", expected.trst},
         {"exit", expected.exit}}};
    for (auto value : values)
      require(runtime.signal(value.first).get() == value.second,
              std::string(where) + ": " + value.first + " at " +
                  std::to_string(runtime.time_fs()) + "fs");
  }
};
void run(const std::string &name) {
  calls.clear();
  script.clear();
  if (name == "repeat" || name == "independent") {
    script = {{1, 1, 0, 1, 0x80000001}, {1, 1, 0, 1, 0x80000001}};
    if (name == "independent")
      script = {{1, 1, 0, 1, 0x80000001}, {0, 1, 1, 1, 0x40000003}};
    Probe p;
    p.arm();
    p.at(9000, {{"clock", 1}});
    p.at(10000, {{"clock", 0}});
    p.at(11000, {{"clock", 1}});
    require(calls.size() == 2 && calls[0].time == 9000 &&
                calls[1].time == 11000,
            "unexpected tick times");
    p.at(108999);
    p.pins({0, 0, 0, 0, 0}, "before delayed transition");
    p.at(109000);
    p.pins(name == "repeat" ? script.front() : Pins{0, 1, 0, 1, 0},
           "unchanged pins keep their original deadline");
    p.at(111000);
    p.pins(script.back(), "independent assignments and inertial cancellation");
  } else if (name == "fractional-output") {
    script = {{1, 1, 1, 1, 0x80000001}};
    Probe p;
    p.arm();
    p.at(9500, {{"clock", 1}});
    p.at(109250);
    p.pins({0, 0, 0, 0, 0}, "fractional output must not arrive early");
    require(p.binding->next_time_fs() == 109500,
            "fractional output deadline must retain femtoseconds");
    require(p.binding->next_time() == 110,
            "legacy picosecond deadline must round up");
    p.at(109500);
    p.pins(script[0], "exact fractional output deadline");
  } else if (name == "fractional-tdo") {
    script = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
    Probe p;
    p.arm();
    p.at(12500, {{"jtag_TDO_data", 1}});
    p.at(112250, {{"clock", 1}});
    p.at(112375, {{"clock", 0}});
    p.at(113000, {{"clock", 1}});
    require(calls.size() == 2 && calls[0].tdo == 0 && calls[1].tdo == 1,
            "TDO must arrive at 112500fs, between the two driver calls");
  } else if (name == "tdo-pulse") {
    script = {{0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}};
    Probe p;
    p.arm();
    p.at(12500, {{"jtag_TDO_data", 1}});
    p.at(20500, {{"jtag_TDO_data", 0}});
    p.at(115000, {{"clock", 1}});
    p.at(116000, {{"clock", 0}, {"jtag_TDO_data", 1}});
    p.at(216500, {{"clock", 1}});
    p.at(217000, {{"clock", 0}, {"jtag_TDO_data", 0}});
    p.at(218000, {{"jtag_TDO_data", 1}});
    p.at(317500, {{"clock", 1}});
    require(calls.size() == 3 && calls[0].tdo == 0 && calls[1].tdo == 1 &&
                calls[2].tdo == 1,
            "short positive/negative TDO pulses must cancel");
  } else if (name == "reset-divider") {
    script = {{1, 0, 0, 1, 3}, {0, 1, 1, 1, 7}, {1, 1, 0, 1, 5}};
    Probe p(2);
    p.at(1000000, {{"clock", 1}});
    p.at(1500000, {{"clock", 0}, {"reset", 0}});
    for (unsigned edge = 2; edge <= 18; ++edge) {
      p.at(uint64_t(edge) * 1000000, {{"clock", 1}});
      if (edge == 4)
        p.at(4500000, {{"clock", 0}, {"enable", 0}});
      else if (edge == 6)
        p.at(6500000, {{"clock", 0}, {"enable", 1}, {"init_done", 0}});
      else if (edge == 11)
        p.at(11500000, {{"clock", 0}, {"reset", 1}});
      else if (edge == 12)
        p.at(12500000, {{"clock", 0}, {"reset", 0}});
      else if (edge == 14)
        p.at(14500000, {{"clock", 0}, {"init_done", 1}});
      else
        p.at(uint64_t(edge) * 1000000 + 500000, {{"clock", 0}});
      if (edge == 12)
        require(p.runtime.signal("exit").get() == 0, "reset must clear exit");
    }
    require(
        calls.size() == 3 && calls[0].time == 8000000 &&
            calls[1].time == 11000000 && calls[2].time == 18000000,
        "enable freeze, TICK_DELAY+1, reset history or sticky init mismatch");
    p.pins(script.back(), "final divided-clock outputs");
  } else {
    throw std::runtime_error("unknown JTAG timing case");
  }
}
} // namespace
extern "C" int jtag_tick(unsigned char *tck, unsigned char *tms,
                         unsigned char *tdi, unsigned char *trst,
                         unsigned char tdo) {
  require(svGetScope() && std::string(svGetNameFromScope(svGetScope())) ==
                              "JtagProbe/debug",
          "jtag_tick must use its instance DPI scope");
  require(calls.size() < script.size(), "unexpected jtag_tick call");
  auto value = script[calls.size()];
  calls.push_back({active_runtime()->time_fs(), tdo});
  *tck = value.tck;
  *tms = value.tms;
  *tdi = value.tdi;
  *trst = value.trst;
  return int(value.exit);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "one case name required");
    register_builtin_bindings();
    // Recreate each model to catch leaked deadlines, histories and scopes.
    for (unsigned repeat = 0; repeat < 2; ++repeat)
      run(argv[1]);
    std::printf("PASS %s recreations=2\n", argv[1]);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
