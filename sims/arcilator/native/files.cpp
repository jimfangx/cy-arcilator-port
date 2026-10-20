// Native SystemVerilog file descriptors and multichannel streams.
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
class FileStream : public llvm::raw_ostream {
  FILE *file;
  void write_impl(const char *data, size_t size) override {
    if (std::fwrite(data, 1, size, file) != size)
      throw std::runtime_error("native SV file write failed");
  }
  uint64_t current_pos() const override {
    auto position = std::ftell(file);
    return position < 0 ? 0 : uint64_t(position);
  }

public:
  explicit FileStream(FILE *file) : llvm::raw_ostream(true), file(file) {}
};
struct OwnedFile {
  FILE *file;
  FileStream stream;
  explicit OwnedFile(FILE *file) : file(file), stream(file) {}
  ~OwnedFile() {
    stream.flush();
    std::fclose(file);
  }
};
void write_channels(uint32_t mask, const char *data, size_t size);
class ChannelStream : public llvm::raw_ostream {
  uint32_t mask;
  void write_impl(const char *data, size_t size) override {
    write_channels(mask, data, size);
  }
  uint64_t current_pos() const override { return 0; }

public:
  explicit ChannelStream(uint32_t mask) : llvm::raw_ostream(true), mask(mask) {}
};
struct Files {
  uint32_t next_fd = 3;
  std::map<uint32_t, std::unique_ptr<OwnedFile>> descriptors, channels;
  std::map<uint32_t, std::unique_ptr<ChannelStream>> streams;
};
Files &files() {
  static Files value;
  return value;
}
void write_channels(uint32_t mask, const char *data, size_t size) {
  if (mask & 1)
    llvm::outs().write(data, size);
  for (auto &entry : files().channels)
    if (mask & entry.first)
      entry.second->stream.write(data, size);
}
} // namespace
extern "C" uint32_t chipyard_arc_sv_fopen(const uint8_t *data, uint64_t size,
                                          int32_t mode) {
  if (!data || mode < -1 || mode > 5)
    return 0;
  std::string name(reinterpret_cast<const char *>(data), size);
  if (name.find('\0') != std::string::npos)
    return 0;
  static const char *modes[] = {"r", "w", "a", "r+", "w+", "a+"};
  FILE *file = std::fopen(name.c_str(), mode < 0 ? "w" : modes[mode]);
  if (!file)
    return 0;
  auto owned = std::make_unique<OwnedFile>(file);
  if (mode < 0) {
    for (unsigned i = 1; i < 31; ++i) {
      uint32_t channel = uint32_t(1) << i;
      if (!files().channels.count(channel)) {
        files().channels[channel] = std::move(owned);
        return channel;
      }
    }
    return 0;
  }
  if (files().next_fd >= 0x80000000u)
    return 0;
  uint32_t fd = 0x80000000u | files().next_fd++;
  files().descriptors[fd] = std::move(owned);
  return fd;
}
extern "C" uint8_t *chipyard_arc_sv_stream(uint32_t channel) {
  llvm::raw_ostream *stream = &llvm::nulls();
  if (channel == 0x80000001u)
    stream = &llvm::outs();
  else if (channel == 0x80000002u)
    stream = &llvm::errs();
  else if (channel & 0x80000000u) {
    auto found = files().descriptors.find(channel);
    if (found != files().descriptors.end())
      stream = &found->second->stream;
  } else {
    auto &found = files().streams[channel];
    if (!found)
      found = std::make_unique<ChannelStream>(channel);
    stream = found.get();
  }
  return reinterpret_cast<uint8_t *>(stream);
}
extern "C" void chipyard_arc_sv_fclose(uint32_t channel) {
  if (channel & 0x80000000u)
    files().descriptors.erase(channel);
  else
    for (auto it = files().channels.begin(); it != files().channels.end();) {
      if (channel & it->first)
        it = files().channels.erase(it);
      else
        ++it;
    }
}
extern "C" void chipyard_arc_sv_fflush_all() {
  llvm::outs().flush();
  llvm::errs().flush();
  for (auto &entry : files().descriptors) {
    entry.second->stream.flush();
    std::fflush(entry.second->file);
  }
  for (auto &entry : files().channels) {
    entry.second->stream.flush();
    std::fflush(entry.second->file);
  }
}
extern "C" void chipyard_arc_close_files() {
  chipyard_arc_sv_fflush_all();
  files().descriptors.clear();
  files().channels.clear();
  files().streams.clear();
  files().next_fd = 3;
}
