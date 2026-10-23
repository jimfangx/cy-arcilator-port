// Native-state VCD snapshots share Verilator's physical simulation time.
#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <verilated.h>

struct ArcTraceSignal { const char *name; unsigned offset, bits; };

class ArcTrace {
  std::ofstream file;
  std::vector<uint8_t> previous;
  bool first = true;
  uint64_t lastTime = ~uint64_t(0);
public:
  explicit ArcTrace(size_t bytes) : previous(bytes, 0) {
    const char *arg = Verilated::commandArgsPlusMatch("arc-vcdfile=");
    std::string path;
    if (arg && *arg) path = arg + std::strlen("+arc-vcdfile=");
#ifdef ARC_TRACE
    else {
      arg = Verilated::commandArgsPlusMatch("vcdfile=");
      if (arg && *arg) path = std::string(arg + std::strlen("+vcdfile=")) + ".arc.vcd";
    }
#endif
    if (!path.empty()) {
      file.open(path);
      if (!file) VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "Cannot open native VCD file");
    }
  }
  void sample(const uint8_t *state, const ArcTraceSignal *signals, size_t count) {
    if (!file.is_open()) return;
    if (first) {
      file << "$timescale 1ps $end\n$scope module ArcModel $end\n";
      for (size_t i = 0; i < count; ++i)
        file << "$var wire " << signals[i].bits << " s" << i << " \\" << signals[i].name << " $end\n";
      file << "$upscope $end\n$enddefinitions $end\n";
    }
    const auto time = Verilated::threadContextp()->time();
    if (time != lastTime) { file << '#' << time << '\n'; lastTime = time; }
    for (size_t i = 0; i < count; ++i) {
      auto &s = signals[i];
      if (!first && !std::memcmp(state + s.offset, previous.data() + s.offset, (s.bits+7)/8)) continue;
      file << 'b';
      for (unsigned b = s.bits; b-- > 0;)
        file << ((state[s.offset + b/8] >> (b%8)) & 1);
      file << " s" << i << '\n';
    }
    std::memcpy(previous.data(), state, previous.size());
    first = false;
  }
};
