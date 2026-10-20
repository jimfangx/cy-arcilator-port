// Native implementation of the SV read-memory file format. The compiler passes
// the current packed memory so holes and partial loads retain existing words.
#include "runtime.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_set>
namespace {
struct Token {
  enum Kind { End, Data, Address, Error } kind;
  std::string text;
};
class Lexer {
  std::istream &stream;
  int current, next;
  void advance() {
    current = next;
    next = stream.get();
  }

public:
  explicit Lexer(std::istream &stream)
      : stream(stream), current(stream.get()), next(stream.get()) {}
  Token token(bool binary) {
    for (;;) {
      while (current != EOF &&
             std::isspace(static_cast<unsigned char>(current)))
        advance();
      if (current == '/' && next == '/') {
        while (current != EOF && current != '\n')
          advance();
      } else if (current == '/' && next == '*') {
        advance();
        advance();
        while (current != EOF && !(current == '*' && next == '/'))
          advance();
        if (current == EOF)
          return {Token::Error, "unterminated comment"};
        advance();
        advance();
      } else
        break;
    }
    if (current == EOF)
      return {Token::End, {}};
    bool address = current == '@';
    if (address)
      advance();
    std::string value;
    while (current != EOF) {
      unsigned char c = current;
      bool digit = address
                       ? std::isxdigit(c)
                       : (binary ? (c == '0' || c == '1') : std::isxdigit(c));
      if (!digit && c != '_' &&
          (address || (c != 'x' && c != 'X' && c != 'z' && c != 'Z')))
        break;
      value.push_back(c);
      advance();
    }
    if (value.empty())
      return {Token::Error, address ? "missing hexadecimal address"
                                    : "invalid character in memory file"};
    return {address ? Token::Address : Token::Data, std::move(value)};
  }
};
unsigned digit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return 0; // Native simulation maps each X/Z bit to zero.
}
struct Dimension {
  uint64_t count;
  int64_t low;
  bool descending;
};
void readmem(uint64_t filename, uint32_t *memory, uint64_t bits, uint64_t words,
             uint64_t encoded_low, uint64_t descending, uint64_t binary,
             uint64_t flags, uint64_t encoded_start, uint64_t encoded_finish,
             uint64_t encoded_left, uint64_t encoded_right,
             uint64_t enum_handle, const std::vector<Dimension> &dimensions) {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime || !memory || !bits || !words || bits > UINT64_MAX / words)
    throw std::invalid_argument("invalid native readmem layout");
  uint64_t elements = 1;
  for (auto &dimension : dimensions) {
    if (!dimension.count || dimension.low < INT32_MIN ||
        dimension.low > INT32_MAX ||
        dimension.count - 1 > uint64_t(INT32_MAX - dimension.low) ||
        dimension.count > UINT64_MAX / elements)
      throw std::invalid_argument("invalid native readmem dimension");
    elements *= dimension.count;
  }
  if (dimensions.empty() || dimensions.front().count != words ||
      dimensions.front().low != int64_t(encoded_low) ||
      dimensions.front().descending != bool(descending) ||
      elements > UINT64_MAX / bits)
    throw std::invalid_argument("inconsistent native readmem dimensions");
  const uint64_t row_elements = elements / words;
  uint64_t within_row = 0;
  auto &name = runtime->string_value(filename);
  auto diagnostic = [&](const char *kind, const std::string &message) {
    std::fprintf(stderr, "%s: native $readmem%c(%s): %s\n", kind,
                 binary ? 'b' : 'h', name.c_str(), message.c_str());
  };
  int64_t low = int64_t(encoded_low), high = low + int64_t(words - 1);
  int64_t left = int32_t(encoded_left), right = int32_t(encoded_right);
  if (flags & 4) {
    if (std::min(left, right) < low || std::max(left, right) > high) {
      diagnostic("ERROR", "memory slice is out of bounds");
      return;
    }
    low = std::min(left, right);
    high = std::max(left, right);
  }
  int64_t start = flags & 1 ? int32_t(encoded_start) : low;
  int64_t finish = flags & 2 ? int32_t(encoded_finish) : high;
  if (start < low || start > high || finish < low || finish > high) {
    diagnostic("ERROR", "start or finish address is out of bounds");
    return;
  }
  int step = (flags & 2) && start > finish ? -1 : 1;
  int64_t minimum = std::min(start, finish), maximum = std::max(start, finish),
          address = start;
  std::ifstream file(name);
  if (name.find('\0') != std::string::npos || !file) {
    diagnostic("ERROR", "cannot open memory file");
    return;
  }
  std::unordered_set<uint64_t> allowed;
  uint64_t mask = bits < 64 ? (uint64_t(1) << bits) - 1 : UINT64_MAX;
  if (flags & 8) {
    std::istringstream input(runtime->string_value(enum_handle));
    std::string value;
    while (input >> value)
      allowed.insert(std::stoull(value) & mask);
  }
  Lexer lexer(file);
  bool explicit_address = false;
  uint64_t loaded = 0;
  for (;;) {
    auto token = lexer.token(binary);
    if (token.kind == Token::End)
      break;
    if (token.kind == Token::Error) {
      diagnostic("ERROR", token.text);
      return;
    }
    if (token.kind == Token::Address) {
      uint64_t value = 0;
      bool valid = false;
      for (char c : token.text) {
        if (c == '_')
          continue;
        valid = true;
        if (value > (UINT32_MAX - digit(c)) / 16) {
          diagnostic("ERROR", "file address exceeds the SV index range");
          return;
        }
        value = value * 16 + digit(c);
      }
      if (!valid) {
        diagnostic("ERROR", "empty file address");
        return;
      }
      address = int32_t(value);
      within_row = 0;
      explicit_address = true;
      if (address < minimum || address > maximum) {
        diagnostic("ERROR", "file address is outside the requested range");
        return;
      }
      continue;
    }
    if (address < minimum || address > maximum) {
      diagnostic("WARNING", "too many words for the requested range");
      return;
    }
    std::vector<uint32_t> value((bits + 31) / 32, 0);
    bool valid = false;
    unsigned shift = binary ? 1 : 4;
    for (char c : token.text) {
      if (c == '_')
        continue;
      valid = true;
      uint64_t carry = digit(c);
      for (auto &word : value) {
        carry |= uint64_t(word) << shift;
        word = uint32_t(carry);
        carry >>= 32;
      }
    }
    if (!valid) {
      diagnostic("ERROR", "empty data word");
      return;
    }
    if (bits % 32)
      value.back() &= (uint32_t(1) << (bits % 32)) - 1;
    if (flags & 8) {
      uint64_t scalar =
          value[0] | (value.size() > 1 ? uint64_t(value[1]) << 32 : 0);
      if (!allowed.count(scalar)) {
        diagnostic("ERROR", "word is outside the memory's enumeration");
        return;
      }
    }
    uint64_t outer = uint64_t(address - int64_t(encoded_low));
    uint64_t index = descending ? outer : words - 1 - outer;
    uint64_t remaining = within_row, stride = row_elements;
    for (size_t d = 1; d < dimensions.size(); ++d) {
      auto &dimension = dimensions[d];
      stride /= dimension.count;
      uint64_t ordinal = remaining / stride;
      remaining %= stride;
      index = index * dimension.count +
              (dimension.descending ? ordinal : dimension.count - 1 - ordinal);
    }
    uint64_t offset = index * bits;
    for (uint64_t bit = 0; bit < bits; bit++) {
      uint32_t target = uint32_t(1) << ((offset + bit) % 32);
      auto &word = memory[(offset + bit) / 32];
      word = (word & ~target) |
             (((value[bit / 32] >> (bit % 32)) & 1) ? target : 0);
    }
    loaded++;
    if (++within_row == row_elements) {
      within_row = 0;
      address += step;
    }
  }
  if (!explicit_address &&
      loaded < (uint64_t(maximum - minimum) + 1) * row_elements)
    diagnostic("WARNING", "fewer words than the requested range");
}
} // namespace
extern "C" void chipyard_arc_readmem(uint64_t filename, uint32_t *memory,
                                     uint64_t bits, uint64_t words,
                                     uint64_t low, uint64_t descending,
                                     uint64_t binary, uint64_t flags,
                                     uint64_t start, uint64_t finish,
                                     uint64_t left, uint64_t right,
                                     uint64_t allowed) {
  readmem(filename, memory, bits, words, low, descending, binary, flags, start,
          finish, left, right, allowed,
          {{words, int64_t(low), bool(descending)}});
}
extern "C" void chipyard_arc_readmem_multi(uint64_t filename, uint32_t *memory,
                                           uint64_t bits, uint64_t words,
                                           uint64_t low, uint64_t descending,
                                           uint64_t binary, uint64_t flags,
                                           uint64_t start, uint64_t finish,
                                           uint64_t left, uint64_t right,
                                           uint64_t allowed, uint64_t layout) {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime)
    throw std::invalid_argument("no active native readmem model");
  std::istringstream input(runtime->string_value(layout));
  std::vector<Dimension> dimensions;
  for (;;) {
    input >> std::ws;
    if (input.eof())
      break;
    uint64_t count;
    int64_t dimension_low;
    unsigned direction;
    if (!(input >> count >> dimension_low >> direction) || direction > 1)
      throw std::invalid_argument("invalid native readmem dimension metadata");
    dimensions.push_back({count, dimension_low, bool(direction)});
  }
  readmem(filename, memory, bits, words, low, descending, binary, flags, start,
          finish, left, right, allowed, dimensions);
}
