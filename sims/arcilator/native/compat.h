#pragma once
#include "include/svdpi.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace chipyard_arc {
void command_args(int argc, char **argv);
void clear_vpi_error() noexcept;
void set_vpi_error(int level, const char *message) noexcept;
// Native scopes have the same lifetime as their model/binding. They identify
// native callbacks and the physical instance paths of compiled SV exports.
class DpiScope {
  std::string full_name;

public:
  explicit DpiScope(std::string name);
  ~DpiScope();
  DpiScope(const DpiScope &) = delete;
  DpiScope &operator=(const DpiScope &) = delete;
  const std::string &name() const { return full_name; }
  svScope handle() { return this; }
};
class DpiContext {
  svScope previous;

public:
  explicit DpiContext(DpiScope &scope);
  ~DpiContext();
  DpiContext(const DpiContext &) = delete;
  DpiContext &operator=(const DpiContext &) = delete;
};
struct OpenArray {
  uint8_t *data;
  size_t elements;
  size_t element_bytes;
  struct Dimension {
    int left, right;
  };
  std::vector<Dimension> dimensions;
  unsigned element_bits = 0, flags = 0;
  int packed_left = 0, packed_right = 0;
  bool explicit_shape = false;
};
} // namespace chipyard_arc
