// Dynamic real formatting missing from the matching CIRCT Arc runtime.
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void chipyard_arc_format_packed_string(const uint8_t *data,
                                                  int32_t bits, int32_t width,
                                                  int32_t left, int32_t padding,
                                                  uint8_t *stream) {
  if (bits < 0 || width < -1 || (left != 0 && left != 1) || padding < 0 ||
      padding > 255 || !stream || (bits && !data))
    throw std::runtime_error("invalid native packed-string format");
  std::string value;
  for (int64_t i = (int64_t(bits) + 7) / 8; i > 0; --i) {
    uint8_t byte = data[i - 1];
    if (i == (int64_t(bits) + 7) / 8 && bits % 8)
      byte &= uint8_t((1u << (bits % 8)) - 1);
    if (byte)
      value.push_back(char(byte));
  }
  auto &output = *reinterpret_cast<llvm::raw_ostream *>(stream);
  size_t count =
      width > int64_t(value.size()) ? size_t(width - value.size()) : 0;
  auto pad = [&] {
    for (size_t i = 0; i < count; ++i)
      output.write(char(padding));
  };
  if (!left)
    pad();
  output.write(value.data(), value.size());
  if (left)
    pad();
}

extern "C" void chipyard_arc_format_real(double value, int32_t width,
                                         int32_t precision, int32_t left,
                                         int32_t kind, uint8_t *stream) {
  if (!stream || width < -1 || precision < 0 || (left != 0 && left != 1) ||
      (kind != 'f' && kind != 'e' && kind != 'g'))
    throw std::runtime_error("invalid native real format");
  std::string format = left ? "%-" : "%";
  if (width >= 0)
    format += std::to_string(width);
  format += "." + std::to_string(precision);
  format += char(kind);
  int length = std::snprintf(nullptr, 0, format.c_str(), value);
  if (length < 0)
    throw std::runtime_error("native real formatting failed");
  std::vector<char> buffer(size_t(length) + 1);
  std::snprintf(buffer.data(), buffer.size(), format.c_str(), value);
  reinterpret_cast<llvm::raw_ostream *>(stream)->write(buffer.data(), length);
}
