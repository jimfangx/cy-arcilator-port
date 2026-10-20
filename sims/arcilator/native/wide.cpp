// Wide integer arithmetic called directly by the compiled Arcilator dataflow.
#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" void chipyard_arc_wide_shift_bytes(uint8_t *out, const uint8_t *data,
                                             const uint8_t *amount, uint64_t width,
                                             uint32_t mode) {
  if (!width) return;
  size_t size = (width + 7) / 8;
  // Most shift amounts have zero high bytes. Reduce full words so checking
  // an arbitrary-width amount does not require a branch for every byte.
  const size_t whole_bytes = width / 8;
  uint64_t shift = 0;
  if (whole_bytes >= sizeof(shift))
    std::memcpy(&shift, amount, sizeof(shift));
  else {
    for (size_t i = 0; i < whole_bytes; ++i)
      shift |= uint64_t(amount[i]) << (i * 8);
    if (width % 8)
      shift |= uint64_t(amount[whole_bytes] & ((1u << (width % 8)) - 1)) << (whole_bytes * 8);
  }
  if (shift < width && whole_bytes >= sizeof(shift)) {
    uint64_t high = 0;
    size_t i = sizeof(shift);
    for (; i + sizeof(high) <= whole_bytes; i += sizeof(high)) {
      uint64_t word;
      std::memcpy(&word, amount + i, sizeof(word));
      high |= word;
    }
    for (; i < whole_bytes; ++i) high |= amount[i];
    if (width % 8) high |= amount[whole_bytes] & ((1u << (width % 8)) - 1);
    if (high) shift = width;
  }
  bool negative = mode == 2 && ((data[(width - 1) / 8] >> ((width - 1) % 8)) & 1);
  uint8_t fill = negative ? 255 : 0;
  std::fill(out, out + size, fill);
  if (shift < width) {
    size_t offset = shift / 8;
    unsigned bits = shift % 8;
    if (!bits) {
      size_t count = size - offset;
      if (mode == 0) std::memcpy(out + offset, data, count);
      else {
        std::memcpy(out, data + offset, count);
        if (width % 8) {
          uint8_t mask = (1u << (width % 8)) - 1;
          out[count - 1] &= mask;
          if (negative) out[count - 1] |= uint8_t(~mask);
        }
      }
    } else {
      auto byte = [&](int64_t index) -> uint8_t {
        if (index < 0) return 0;
        if (uint64_t(index) >= size) return fill;
        uint8_t value = data[index];
        if (uint64_t(index) == size - 1 && width % 8) {
          value &= (1u << (width % 8)) - 1;
          if (negative) value |= uint8_t(255u << (width % 8));
        }
        return value;
      };
      for (size_t i = 0; i < size; ++i) {
        if (mode == 0)
          out[i] = uint8_t(byte(int64_t(i) - int64_t(offset)) << bits) |
                   uint8_t(byte(int64_t(i) - int64_t(offset) - 1) >> (8 - bits));
        else
          out[i] = uint8_t(byte(i + offset) >> bits) |
                   uint8_t(byte(i + offset + 1) << (8 - bits));
      }
    }
  }
  if (width % 8) out[size - 1] &= (1u << (width % 8)) - 1;
}
