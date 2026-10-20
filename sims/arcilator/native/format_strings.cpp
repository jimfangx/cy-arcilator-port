// In-memory targets for the SDK's existing format-to-stream runtime.
#include "runtime.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <memory>
#include <stdexcept>
namespace {
struct Buffer {
  std::string bytes;
  llvm::raw_string_ostream stream;
  Buffer() : stream(bytes) {}
};
thread_local std::map<uint8_t *, std::unique_ptr<Buffer>> buffers;
} // namespace
extern "C" uint8_t *chipyard_arc_format_buffer_begin() {
  auto buffer = std::make_unique<Buffer>();
  auto *key = reinterpret_cast<uint8_t *>(&buffer->stream);
  buffers.emplace(key, std::move(buffer));
  return key;
}
extern "C" uint64_t chipyard_arc_format_buffer_end(uint8_t *key) {
  auto it = buffers.find(key);
  auto *runtime = chipyard_arc::active_runtime();
  if (it == buffers.end() || !runtime)
    throw std::runtime_error("invalid native format buffer completion");
  auto buffer = std::move(it->second);
  buffers.erase(it);
  buffer->stream.flush();
  return runtime->intern_string(buffer->bytes);
}
extern "C" void chipyard_arc_format_native_string(uint64_t handle,
                                                  int32_t width, int32_t left,
                                                  int32_t padding,
                                                  uint8_t *stream) {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime || !stream || width < -1 || (left != 0 && left != 1) ||
      padding < 0 || padding > 255)
    throw std::runtime_error("invalid native dynamic string format");
  const auto &value = runtime->string_value(handle);
  auto &output = *reinterpret_cast<llvm::raw_ostream *>(stream);
  size_t count =
      width > int64_t(value.size()) ? size_t(width - value.size()) : 0;
  auto pad = [&] {
    for (size_t n = 0; n < count; ++n)
      output.write(char(padding));
  };
  if (!left)
    pad();
  output.write(value.data(), value.size());
  if (left)
    pad();
}
