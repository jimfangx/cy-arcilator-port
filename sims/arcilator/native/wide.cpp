// Wide integer arithmetic called directly by the compiled Arcilator dataflow.
#include <algorithm>
#include <cstdint>
#include <cstring>

static void bounded_shift(uint8_t *out, const uint8_t *data, uint64_t shift,
                          uint64_t width, uint32_t mode) {
  if (!width)
    return;
  size_t size = (width + 7) / 8;
  bool negative =
      mode == 2 && ((data[(width - 1) / 8] >> ((width - 1) % 8)) & 1);
  uint8_t fill = negative ? 255 : 0;
  if (shift < width) {
    size_t offset = shift / 8;
    unsigned bits = shift % 8;
    if (!bits) {
      size_t count = size - offset;
      if (mode == 0) {
        std::fill(out, out + offset, fill);
        std::memcpy(out + offset, data, count);
      } else {
        std::fill(out + count, out + size, fill);
        std::memcpy(out, data + offset, count);
        if (width % 8) {
          uint8_t mask = (1u << (width % 8)) - 1;
          out[count - 1] &= mask;
          if (negative)
            out[count - 1] |= uint8_t(~mask);
        }
      }
    } else {
      // Fixed-size interior copies permit SIMD without reading past a port.
      // Boundary copies preserve unaligned ports and partial final words.
      const size_t words = (size + 7) / 8;
      const size_t word_offset = shift / 64;
      const unsigned word_bits = shift % 64;
      const unsigned tail_bits = width % 64;
      const uint64_t extension = negative ? ~uint64_t(0) : 0;
      auto word = [&](size_t index) -> uint64_t {
        if (index >= words)
          return extension;
        size_t first = index * sizeof(uint64_t);
        uint64_t value = 0;
        if (size - first >= sizeof(value))
          std::memcpy(&value, data + first, sizeof(value));
        else
          std::memcpy(&value, data + first, size - first);
        if (index == words - 1 && tail_bits) {
          uint64_t mask = (uint64_t(1) << tail_bits) - 1;
          value = (value & mask) | (extension & ~mask);
        }
        return value;
      };
      auto put = [&](size_t index, uint64_t value) {
        size_t first = index * sizeof(value);
        if (size - first >= sizeof(value))
          std::memcpy(out + first, &value, sizeof(value));
        else
          std::memcpy(out + first, &value, size - first);
      };
      const size_t full_words = width / 64;
      if (mode == 0) {
        std::fill(out, out + word_offset * sizeof(uint64_t), fill);
        size_t i = word_offset;
        put(i++, word(0) << word_bits);
        // Interior words have complete source and destination storage. Keep
        // the two source loads independent so this loop can vectorize.
        for (; i < full_words; ++i) {
          uint64_t current, previous;
          size_t input = i - word_offset;
          std::memcpy(&current, data + input * sizeof(current), sizeof(current));
          std::memcpy(&previous, data + (input - 1) * sizeof(previous),
                      sizeof(previous));
          uint64_t value = (current << word_bits) |
                           (previous >> (64 - word_bits));
          std::memcpy(out + i * sizeof(value), &value, sizeof(value));
        }
        for (; i < words; ++i) {
          uint64_t current = word(i - word_offset);
          uint64_t previous = word(i - word_offset - 1);
          put(i, (current << word_bits) | (previous >> (64 - word_bits)));
        }
      } else {
        size_t written = std::min(size, (words - word_offset) * sizeof(uint64_t));
        std::fill(out + written, out + size, fill);
        size_t interior = full_words > word_offset
                              ? full_words - word_offset - 1
                              : 0;
        size_t i = 0;
        for (; i < interior; ++i) {
          uint64_t current, next;
          size_t input = i + word_offset;
          std::memcpy(&current, data + input * sizeof(current), sizeof(current));
          std::memcpy(&next, data + (input + 1) * sizeof(next), sizeof(next));
          uint64_t value = (current >> word_bits) |
                           (next << (64 - word_bits));
          std::memcpy(out + i * sizeof(value), &value, sizeof(value));
        }
        // Only the final source word and sign extension need bounded copies.
        for (; i < words - word_offset; ++i) {
          uint64_t current = word(i + word_offset);
          uint64_t next = word(i + word_offset + 1);
          put(i, (current >> word_bits) | (next << (64 - word_bits)));
        }
      }
    }
  } else {
    std::fill(out, out + size, fill);
  }
  if (width % 8)
    out[size - 1] &= (1u << (width % 8)) - 1;
}

// Full upper words are reduced in LLVM. The last partial word is masked here
// so host padding cannot be mistaken for a meaningful shift-amount bit.
extern "C" void chipyard_arc_wide_shift_words(uint8_t *out, const uint8_t *data,
                                              uint64_t low, uint64_t high,
                                              uint64_t tail, uint64_t width,
                                              uint32_t mode) {
  if (!width)
    return;
  if (width <= 64) {
    if (width < 64)
      low &= (uint64_t(1) << width) - 1;
  } else {
    unsigned bits = width % 64;
    if (high || (bits && (tail & ((uint64_t(1) << bits) - 1))))
      low = width;
  }
  bounded_shift(out, data, low, width, mode);
}

// Preserve the byte-array amount ABI for previously compiled models.
extern "C" void chipyard_arc_wide_shift_bytes(uint8_t *out, const uint8_t *data,
                                              const uint8_t *amount,
                                              uint64_t width, uint32_t mode) {
  if (!width)
    return;
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
      shift |= uint64_t(amount[whole_bytes] & ((1u << (width % 8)) - 1))
               << (whole_bytes * 8);
  }
  if (shift < width && whole_bytes >= sizeof(shift)) {
    uint64_t high = 0;
    size_t i = sizeof(shift);
    for (; i + sizeof(high) <= whole_bytes; i += sizeof(high)) {
      uint64_t word;
      std::memcpy(&word, amount + i, sizeof(word));
      high |= word;
    }
    for (; i < whole_bytes; ++i)
      high |= amount[i];
    if (width % 8)
      high |= amount[whole_bytes] & ((1u << (width % 8)) - 1);
    if (high)
      shift = width;
  }
  bounded_shift(out, data, shift, width, mode);
}

// Pure arithmetic: the input is a zero-extended unsigned host word.
extern "C" void chipyard_arc_private_shift_scalar(
    uint8_t *out, uint64_t data, uint64_t low, uint64_t high,
    uint64_t tail, uint64_t width) {
  if (!width)
    return;
  if (width <= 64) {
    if (width < 64)
      low &= (uint64_t(1) << width) - 1;
  } else {
    unsigned bits = width % 64;
    if (high || (bits && (tail & ((uint64_t(1) << bits) - 1))))
      low = width;
  }
  const size_t size = (width + 7) / 8;
  std::memset(out, 0, size);
  if (low < width) {
    const size_t offset = low / 8;
    const unsigned bits = low % 8;
    const uint64_t value = data << bits;
    const size_t count = std::min(size - offset, sizeof(value));
    std::memcpy(out + offset, &value, count);
    if (bits && size - offset > sizeof(value))
      out[offset + sizeof(value)] = data >> (64 - bits);
  }
  if (width % 8)
    out[size - 1] &= (1u << (width % 8)) - 1;
}
