#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace chipyard_arc {
class Trace {
  struct Entry {
    std::string name;
    const uint8_t *data;
    unsigned bits;
    std::vector<uint8_t> previous;
  };
  std::ofstream file;
  std::vector<Entry> entries;
  uint64_t start = 0, last_time = ~uint64_t(0);
  bool header = false, first = true;
  bool femtoseconds = false;
public:
  void open(const std::string &path, uint64_t start_cycle, bool use_femtoseconds = false) {
    if (path.empty()) return;
    femtoseconds = use_femtoseconds;
    uint64_t cycle_units = femtoseconds ? 1000000 : 1000;
    if (start_cycle > std::numeric_limits<uint64_t>::max() / cycle_units)
      throw std::runtime_error("native waveform start cycle exceeds the simulation time range");
    start = start_cycle ? start_cycle * cycle_units - cycle_units / 2 : 0;
    file.open(path);
    if (!file) throw std::runtime_error("cannot open native waveform " + path);
  }
  void add(const std::string &name, const uint8_t *data, unsigned bits) {
    if (!file.is_open()) return;
    if (header) throw std::runtime_error("cannot add a native waveform signal after simulation starts");
    if (name.find_first_of(" \t\r\n") != std::string::npos)
      throw std::runtime_error("native waveform signal contains whitespace: " + name);
    entries.push_back({name, data, bits, std::vector<uint8_t>((size_t(bits) + 7) / 8)});
  }
  void sample(uint64_t time) {
    if (!file.is_open()) return;
    if (!header) {
      file << "$timescale " << (femtoseconds ? "1fs" : "1ps") << " $end\n$scope module ArcModel $end\n";
      for (size_t i = 0; i < entries.size(); ++i)
        file << "$var wire " << entries[i].bits << " s" << i << " \\" << entries[i].name << " $end\n";
      file << "$upscope $end\n$enddefinitions $end\n";
      header = true;
    }
    if (time < start) return;
    if (last_time != time) { file << '#' << time << '\n'; last_time = time; }
    for (size_t i = 0; i < entries.size(); ++i) {
      auto &entry = entries[i];
      if (!first && !std::memcmp(entry.data, entry.previous.data(), entry.previous.size())) continue;
      file << 'b';
      for (unsigned bit = entry.bits; bit-- > 0;) file << ((entry.data[bit / 8] >> (bit % 8)) & 1);
      file << " s" << i << '\n';
      std::memcpy(entry.previous.data(), entry.data, entry.previous.size());
    }
    first = false;
  }
};
}
