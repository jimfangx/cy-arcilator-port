#include "native/evaluation.h"
#include <array>
#include <cstdint>
#include <stdexcept>

extern "C" uint32_t chipyard_dpi_chain(uint32_t site, uint32_t value) {
  static std::array<uint32_t, 16> previous{};
  static std::array<bool, 16> seen{};
  if (chipyard_arc_is_sampling())
    throw std::runtime_error("combinational DPI ran during hybrid sampling");
  if (seen.at(site) && previous.at(site) == value)
    throw std::runtime_error("stable hybrid DPI input activated again");
  previous.at(site) = value;
  seen.at(site) = true;
  unsigned shift = site + 1;
  return ((value << shift) | (value >> (32 - shift))) ^
         (0x9e3779b9u * (site + 1));
}
