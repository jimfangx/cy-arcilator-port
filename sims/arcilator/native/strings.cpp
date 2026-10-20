#include "runtime.h"
#include <algorithm>
namespace chipyard_arc {
uint64_t Runtime::intern_string(const std::string &value) {
  if (value.empty())
    return 0;
  auto [it, fresh] = native_strings.emplace(value, 0);
  if (fresh) {
    it->second = uint64_t(reinterpret_cast<uintptr_t>(&it->first));
    string_handles.emplace(it->second, &it->first);
  }
  return it->second;
}
const std::string &Runtime::string_value(uint64_t handle) const {
  static const std::string empty;
  if (!handle)
    return empty;
  auto it = string_handles.find(handle);
  if (it == string_handles.end())
    throw std::invalid_argument("unknown native string handle");
  return *it->second;
}
} // namespace chipyard_arc
namespace {
chipyard_arc::Runtime &owner() {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime)
    throw std::runtime_error(
        "native string operation requires an active model");
  return *runtime;
}
} // namespace
extern "C" uint64_t chipyard_arc_string_create(uint64_t address,
                                               uint64_t size) {
  auto *data = reinterpret_cast<const char *>(uintptr_t(address));
  if (size && !data)
    throw std::invalid_argument("null native string bytes");
  return owner().intern_string(size ? std::string(data, size) : std::string());
}
extern "C" uint64_t chipyard_arc_string_length(uint64_t handle) {
  return owner().string_value(handle).size();
}
extern "C" uint64_t chipyard_arc_string_concat(uint64_t a, uint64_t b) {
  auto &r = owner();
  return r.intern_string(r.string_value(a) + r.string_value(b));
}
extern "C" uint64_t chipyard_arc_string_get(uint64_t handle, uint64_t index) {
  auto &value = owner().string_value(handle);
  return index < value.size() ? uint8_t(value[index]) : 0;
}
extern "C" uint64_t chipyard_arc_string_compare(uint64_t a, uint64_t b) {
  auto &r = owner();
  auto &left = r.string_value(a);
  auto &right = r.string_value(b);
  for (size_t i = 0; i < std::max(left.size(), right.size()); i++) {
    int x = i < left.size() ? uint8_t(left[i]) : 0;
    int y = i < right.size() ? uint8_t(right[i]) : 0;
    if (x != y)
      return uint64_t(int64_t(x - y));
    if (i == left.size() || i == right.size())
      return i == left.size() ? uint64_t(-1) : 1;
  }
  return 0;
}
extern "C" uint64_t chipyard_arc_string_case(uint64_t handle, uint64_t upper) {
  auto &r = owner();
  auto result = r.string_value(handle);
  for (char &c : result) {
    auto v = uint8_t(c);
    if (upper ? (v >= 'a' && v <= 'z') : (v >= 'A' && v <= 'Z'))
      c = char(v + (upper ? -32 : 32));
  }
  return r.intern_string(result);
}
extern "C" uint64_t chipyard_arc_string_append_byte(uint64_t handle,
                                                    uint64_t byte) {
  auto &r = owner();
  auto result = r.string_value(handle);
  if (byte & 255)
    result.push_back(char(byte));
  return r.intern_string(result);
}
extern "C" uint64_t chipyard_arc_string_pack(uint64_t handle, uint64_t offset) {
  auto &value = owner().string_value(handle);
  uint64_t result = 0;
  for (unsigned i = 0; i < 8 && offset + i < value.size(); i++)
    result |= uint64_t(uint8_t(value[value.size() - 1 - offset - i]))
              << (i * 8);
  return result;
}

extern "C" uint32_t chipyard_arc_sv_fopen(const uint8_t *, uint64_t, int32_t);
extern "C" uint64_t chipyard_arc_string_fopen(uint64_t handle, uint64_t mode) {
  auto &name = owner().string_value(handle);
  return chipyard_arc_sv_fopen(reinterpret_cast<const uint8_t *>(name.data()),
                               name.size(), int32_t(mode));
}

// DPI strings borrow immutable input bytes; returned C buffers are copied
// before another import can reuse them. Handle zero denotes the empty string.
extern "C" const char *chipyard_arc_string_c_str(uint64_t handle) {
  return owner().string_value(handle).c_str();
}
extern "C" uint64_t chipyard_arc_string_from_c(const char *value) {
  return owner().intern_string(value ? std::string(value) : std::string());
}
