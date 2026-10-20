#include "compat.h"
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
using chipyard_arc::OpenArray;
namespace {
constexpr unsigned vectorData = 1, fourState = 2, logicScalar = 4;
const OpenArray &array(svOpenArrayHandle h) {
  if (!h)
    throw std::invalid_argument("null native DPI array handle");
  return *static_cast<const OpenArray *>(h);
}
size_t rank(const OpenArray &a) {
  return a.explicit_shape ? a.dimensions.size() : 1;
}
OpenArray::Dimension dimension(const OpenArray &a, int d) {
  if (d == 0) {
    if (a.explicit_shape)
      return {a.packed_left, a.packed_right};
    return {int(a.element_bytes * 8) - 1, 0};
  }
  if (d < 1 || size_t(d) > rank(a))
    return {0, -1};
  return a.explicit_shape ? a.dimensions[d - 1]
                          : OpenArray::Dimension{0, int(a.elements) - 1};
}
unsigned bits(const OpenArray &a) {
  return a.explicit_shape ? a.element_bits : a.element_bytes * 8;
}
void *element(svOpenArrayHandle h, const int *indices, size_t count) {
  if (!h)
    return nullptr;
  auto &a = array(h);
  if (count != rank(a) || !a.data)
    return nullptr;
  size_t offset = 0;
  for (size_t i = 0; i < count; i++) {
    auto dim = dimension(a, i + 1);
    auto low = std::min(dim.left, dim.right),
         high = std::max(dim.left, dim.right);
    if (indices[i] < low || indices[i] > high)
      return nullptr;
    size_t size = uint64_t(int64_t(high) - low) + 1;
    size_t at = dim.left <= dim.right ? int64_t(indices[i]) - dim.left
                                      : int64_t(dim.left) - indices[i];
    offset = offset * size + at;
  }
  return static_cast<void *>(a.data + offset * a.element_bytes);
}
std::vector<int> indices(svOpenArrayHandle h, int first, va_list rest) {
  std::vector<int> result;
  if (!h)
    return result;
  auto n = rank(array(h));
  if (n)
    result.push_back(first);
  for (size_t i = 1; i < n; i++)
    result.push_back(va_arg(rest, int));
  return result;
}
uint32_t word(const OpenArray &a, const void *p, unsigned n, bool unknown) {
  if (!p)
    return 0;
  if (a.flags & vectorData) {
    if (a.flags & fourState)
      return unknown ? static_cast<const svLogicVecVal *>(p)[n].bval
                     : static_cast<const svLogicVecVal *>(p)[n].aval;
    return unknown ? 0 : static_cast<const svBitVecVal *>(p)[n];
  }
  if (a.flags & logicScalar) {
    auto v = *static_cast<const uint8_t *>(p);
    return n ? 0 : unknown ? (v >> 1) & 1 : v & 1;
  }
  uint32_t value = 0;
  size_t start = size_t(n) * 4;
  if (!unknown && start < a.element_bytes)
    std::memcpy(&value, static_cast<const uint8_t *>(p) + start,
                std::min(size_t(4), a.element_bytes - start));
  return value;
}
void setWord(const OpenArray &a, void *p, unsigned n, uint32_t value,
             uint32_t unknown) {
  if (!p)
    return;
  if (a.flags & vectorData) {
    if (a.flags & fourState)
      static_cast<svLogicVecVal *>(p)[n] = {value, unknown};
    else
      static_cast<svBitVecVal *>(p)[n] = value & ~unknown;
    return;
  }
  if (a.flags & logicScalar) {
    if (!n)
      *static_cast<uint8_t *>(p) = uint8_t((value & 1) | ((unknown & 1) << 1));
    return;
  }
  size_t start = size_t(n) * 4;
  value &= ~unknown;
  if (start < a.element_bytes)
    std::memcpy(static_cast<uint8_t *>(p) + start, &value,
                std::min(size_t(4), a.element_bytes - start));
}
void getVector(svOpenArrayHandle h, const int *idx, size_t n, void *out,
               bool logic) {
  auto &a = array(h);
  auto p = element(h, idx, n);
  if (!p || !out)
    return;
  for (unsigned w = 0; w < (bits(a) + 31) / 32; w++) {
    auto value = word(a, p, w, false), unknown = word(a, p, w, true);
    if (logic)
      static_cast<svLogicVecVal *>(out)[w] = {value, unknown};
    else
      static_cast<svBitVecVal *>(out)[w] = value & ~unknown;
  }
}
void putVector(svOpenArrayHandle h, const int *idx, size_t n, const void *in,
               bool logic) {
  auto &a = array(h);
  auto p = element(h, idx, n);
  if (!p || !in)
    return;
  for (unsigned w = 0; w < (bits(a) + 31) / 32; w++) {
    auto value = logic ? static_cast<const svLogicVecVal *>(in)[w].aval
                       : static_cast<const svBitVecVal *>(in)[w];
    auto unknown = logic ? static_cast<const svLogicVecVal *>(in)[w].bval : 0;
    setWord(a, p, w, value, unknown);
  }
}
svLogic getScalar(svOpenArrayHandle h, const int *idx, size_t n, bool logic) {
  auto &a = array(h);
  auto p = element(h, idx, n);
  if (!p)
    return 0;
  auto value = word(a, p, 0, false), unknown = word(a, p, 0, true);
  return logic ? svLogic((value & 1) | ((unknown & 1) << 1))
               : svBit((value & ~unknown) & 1);
}
void putScalar(svOpenArrayHandle h, const int *idx, size_t n, svLogic value,
               bool logic) {
  auto &a = array(h);
  auto p = element(h, idx, n);
  if (!p)
    return;
  // Preserve the original SimDRAM byte payload accessor contract.
  if (!logic && !(a.flags & (vectorData | logicScalar))) {
    std::memset(p, 0, a.element_bytes);
    *static_cast<uint8_t *>(p) = value;
    return;
  }
  setWord(a, p, 0, value & 1, logic ? (value >> 1) & 1 : 0);
}
} // namespace
extern "C" {
void *chipyard_arc_open_array_create(void *data, uint64_t bytes, uint64_t width,
                                     uint64_t flags, const char *shape,
                                     int64_t packedLeft, int64_t packedRight) {
  if (!data || !bytes || !width ||
      width > std::numeric_limits<unsigned>::max() || !shape || flags > 7 ||
      packedLeft < std::numeric_limits<int>::min() ||
      packedLeft > std::numeric_limits<int>::max() ||
      packedRight < std::numeric_limits<int>::min() ||
      packedRight > std::numeric_limits<int>::max())
    throw std::invalid_argument("invalid native open-array descriptor");
  auto a = std::make_unique<OpenArray>(
      OpenArray{static_cast<uint8_t *>(data), 1, size_t(bytes)});
  a->element_bits = width;
  a->flags = flags;
  a->explicit_shape = true;
  a->packed_left = packedLeft;
  a->packed_right = packedRight;
  const char *p = shape;
  auto number = [&]() {
    char *end = nullptr;
    errno = 0;
    long value = std::strtol(p, &end, 10);
    if (errno || end == p || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max())
      throw std::invalid_argument("invalid native open-array dimension");
    p = end;
    return int(value);
  };
  while (*p) {
    int left = number();
    if (*p++ != ':')
      throw std::invalid_argument("invalid native open-array dimensions");
    int right = number();
    size_t count = uint64_t(std::abs(int64_t(left) - right)) + 1;
    if (count > std::numeric_limits<size_t>::max() / a->elements)
      throw std::overflow_error("native open-array size overflow");
    a->elements *= count;
    a->dimensions.push_back({left, right});
    if (!*p)
      break;
    if (*p++ != ',' || !*p)
      throw std::invalid_argument("invalid native open-array dimensions");
  }
  if (a->elements > std::numeric_limits<size_t>::max() / a->element_bytes)
    throw std::overflow_error("native open-array byte size overflow");
  return a.release();
}
void chipyard_arc_open_array_delete(void *h) {
  delete static_cast<OpenArray *>(h);
}
int svDimensions(svOpenArrayHandle h) { return rank(array(h)); }
int svLeft(svOpenArrayHandle h, int d) {
  auto &a = array(h);
  return d < 0 || size_t(d) > rank(a) ? 0 : dimension(a, d).left;
}
int svRight(svOpenArrayHandle h, int d) {
  auto &a = array(h);
  return d < 0 || size_t(d) > rank(a) ? 0 : dimension(a, d).right;
}
int svLow(svOpenArrayHandle h, int d) {
  return std::min(svLeft(h, d), svRight(h, d));
}
int svHigh(svOpenArrayHandle h, int d) {
  return std::max(svLeft(h, d), svRight(h, d));
}
int svSize(svOpenArrayHandle h, int d) {
  auto &a = array(h);
  if (d < 0 || size_t(d) > rank(a))
    return 0;
  if (!d && a.explicit_shape)
    return a.element_bits;
  auto x = dimension(a, d);
  return uint64_t(std::abs(int64_t(x.left) - x.right)) + 1;
}
int svIncrement(svOpenArrayHandle h, int d) {
  auto &a = array(h);
  if (d < 0 || size_t(d) > rank(a))
    return 0;
  auto x = dimension(a, d);
  return x.left >= x.right ? 1 : -1;
}
int svSizeOfArray(svOpenArrayHandle h) {
  auto &a = array(h);
  auto n = a.elements * a.element_bytes;
  return n > std::numeric_limits<int>::max() ? 0 : int(n);
}
void *svGetArrayPtr(svOpenArrayHandle h) { return array(h).data; }
void *svGetArrElemPtr(svOpenArrayHandle h, int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  return element(h, v.data(), v.size());
}
void *svGetArrElemPtr1(svOpenArrayHandle h, int i0) {
  int idx[] = {i0};
  return element(h, idx, 1);
}
void *svGetArrElemPtr2(svOpenArrayHandle h, int i0, int i1) {
  int idx[] = {i0, i1};
  return element(h, idx, 2);
}
void *svGetArrElemPtr3(svOpenArrayHandle h, int i0, int i1, int i2) {
  int idx[] = {i0, i1, i2};
  return element(h, idx, 3);
}
svBit svGetBitArrElem(svOpenArrayHandle h, int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  return getScalar(h, v.data(), v.size(), false);
}
void svPutBitArrElem(svOpenArrayHandle h, svBit value, int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  putScalar(h, v.data(), v.size(), value, false);
}
void svGetBitArrElemVecVal(svBitVecVal *out, svOpenArrayHandle h, int first,
                           ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  getVector(h, v.data(), v.size(), out, false);
}
void svPutBitArrElemVecVal(svOpenArrayHandle h, const svBitVecVal *in,
                           int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  putVector(h, v.data(), v.size(), in, false);
}
svBit svGetBitArrElem1(svOpenArrayHandle h, int i0) {
  int v[] = {i0};
  return getScalar(h, v, 1, false);
}
void svPutBitArrElem1(svOpenArrayHandle h, svBit value, int i0) {
  int v[] = {i0};
  putScalar(h, v, 1, value, false);
}
void svGetBitArrElem1VecVal(svBitVecVal *out, svOpenArrayHandle h, int i0) {
  int v[] = {i0};
  getVector(h, v, 1, out, false);
}
void svPutBitArrElem1VecVal(svOpenArrayHandle h, const svBitVecVal *in,
                            int i0) {
  int v[] = {i0};
  putVector(h, v, 1, in, false);
}
svBit svGetBitArrElem2(svOpenArrayHandle h, int i0, int i1) {
  int v[] = {i0, i1};
  return getScalar(h, v, 2, false);
}
void svPutBitArrElem2(svOpenArrayHandle h, svBit value, int i0, int i1) {
  int v[] = {i0, i1};
  putScalar(h, v, 2, value, false);
}
void svGetBitArrElem2VecVal(svBitVecVal *out, svOpenArrayHandle h, int i0,
                            int i1) {
  int v[] = {i0, i1};
  getVector(h, v, 2, out, false);
}
void svPutBitArrElem2VecVal(svOpenArrayHandle h, const svBitVecVal *in, int i0,
                            int i1) {
  int v[] = {i0, i1};
  putVector(h, v, 2, in, false);
}
svBit svGetBitArrElem3(svOpenArrayHandle h, int i0, int i1, int i2) {
  int v[] = {i0, i1, i2};
  return getScalar(h, v, 3, false);
}
void svPutBitArrElem3(svOpenArrayHandle h, svBit value, int i0, int i1,
                      int i2) {
  int v[] = {i0, i1, i2};
  putScalar(h, v, 3, value, false);
}
void svGetBitArrElem3VecVal(svBitVecVal *out, svOpenArrayHandle h, int i0,
                            int i1, int i2) {
  int v[] = {i0, i1, i2};
  getVector(h, v, 3, out, false);
}
void svPutBitArrElem3VecVal(svOpenArrayHandle h, const svBitVecVal *in, int i0,
                            int i1, int i2) {
  int v[] = {i0, i1, i2};
  putVector(h, v, 3, in, false);
}
svLogic svGetLogicArrElem(svOpenArrayHandle h, int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  return getScalar(h, v.data(), v.size(), true);
}
void svPutLogicArrElem(svOpenArrayHandle h, svLogic value, int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  putScalar(h, v.data(), v.size(), value, true);
}
void svGetLogicArrElemVecVal(svLogicVecVal *out, svOpenArrayHandle h, int first,
                             ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  getVector(h, v.data(), v.size(), out, true);
}
void svPutLogicArrElemVecVal(svOpenArrayHandle h, const svLogicVecVal *in,
                             int first, ...) {
  va_list ap;
  va_start(ap, first);
  auto v = indices(h, first, ap);
  va_end(ap);
  putVector(h, v.data(), v.size(), in, true);
}
svLogic svGetLogicArrElem1(svOpenArrayHandle h, int i0) {
  int v[] = {i0};
  return getScalar(h, v, 1, true);
}
void svPutLogicArrElem1(svOpenArrayHandle h, svLogic value, int i0) {
  int v[] = {i0};
  putScalar(h, v, 1, value, true);
}
void svGetLogicArrElem1VecVal(svLogicVecVal *out, svOpenArrayHandle h, int i0) {
  int v[] = {i0};
  getVector(h, v, 1, out, true);
}
void svPutLogicArrElem1VecVal(svOpenArrayHandle h, const svLogicVecVal *in,
                              int i0) {
  int v[] = {i0};
  putVector(h, v, 1, in, true);
}
svLogic svGetLogicArrElem2(svOpenArrayHandle h, int i0, int i1) {
  int v[] = {i0, i1};
  return getScalar(h, v, 2, true);
}
void svPutLogicArrElem2(svOpenArrayHandle h, svLogic value, int i0, int i1) {
  int v[] = {i0, i1};
  putScalar(h, v, 2, value, true);
}
void svGetLogicArrElem2VecVal(svLogicVecVal *out, svOpenArrayHandle h, int i0,
                              int i1) {
  int v[] = {i0, i1};
  getVector(h, v, 2, out, true);
}
void svPutLogicArrElem2VecVal(svOpenArrayHandle h, const svLogicVecVal *in,
                              int i0, int i1) {
  int v[] = {i0, i1};
  putVector(h, v, 2, in, true);
}
svLogic svGetLogicArrElem3(svOpenArrayHandle h, int i0, int i1, int i2) {
  int v[] = {i0, i1, i2};
  return getScalar(h, v, 3, true);
}
void svPutLogicArrElem3(svOpenArrayHandle h, svLogic value, int i0, int i1,
                        int i2) {
  int v[] = {i0, i1, i2};
  putScalar(h, v, 3, value, true);
}
void svGetLogicArrElem3VecVal(svLogicVecVal *out, svOpenArrayHandle h, int i0,
                              int i1, int i2) {
  int v[] = {i0, i1, i2};
  getVector(h, v, 3, out, true);
}
void svPutLogicArrElem3VecVal(svOpenArrayHandle h, const svLogicVecVal *in,
                              int i0, int i1, int i2) {
  int v[] = {i0, i1, i2};
  putVector(h, v, 3, in, true);
}
svBit svGetBitselBit(const svBitVecVal *p, int i) {
  return (p[i / 32] >> (i % 32)) & 1;
}
svLogic svGetBitselLogic(const svLogicVecVal *p, int i) {
  return ((p[i / 32].aval >> (i % 32)) & 1) |
         (((p[i / 32].bval >> (i % 32)) & 1) << 1);
}
void svPutBitselBit(svBitVecVal *p, int i, svBit v) {
  uint32_t mask = uint32_t(1) << (i % 32);
  p[i / 32] = (p[i / 32] & ~mask) | ((uint32_t(v) & 1) << (i % 32));
}
void svPutBitselLogic(svLogicVecVal *p, int i, svLogic v) {
  uint32_t mask = uint32_t(1) << (i % 32);
  p[i / 32].aval = (p[i / 32].aval & ~mask) | ((uint32_t(v) & 1) << (i % 32));
  p[i / 32].bval =
      (p[i / 32].bval & ~mask) | (((uint32_t(v) >> 1) & 1) << (i % 32));
}
void svGetPartselBit(svBitVecVal *d, const svBitVecVal *s, int i, int n) {
  *d = 0;
  for (int b = 0; b < n; b++)
    svPutBitselBit(d, b, svGetBitselBit(s, i + b));
}
void svGetPartselLogic(svLogicVecVal *d, const svLogicVecVal *s, int i, int n) {
  *d = {0, 0};
  for (int b = 0; b < n; b++)
    svPutBitselLogic(d, b, svGetBitselLogic(s, i + b));
}
void svPutPartselBit(svBitVecVal *d, svBitVecVal s, int i, int n) {
  for (int b = 0; b < n; b++)
    svPutBitselBit(d, i + b, svGetBitselBit(&s, b));
}
void svPutPartselLogic(svLogicVecVal *d, svLogicVecVal s, int i, int n) {
  for (int b = 0; b < n; b++)
    svPutBitselLogic(d, i + b, svGetBitselLogic(&s, b));
}
}
