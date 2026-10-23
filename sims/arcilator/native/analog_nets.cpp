#include "runtime.h"
#include <algorithm>

namespace chipyard_arc {
struct Runtime::WideAnalogState {
  struct Net {
    unsigned bits;
    std::vector<uint8_t> storage, ones, zeros;
    std::map<std::pair<const Binding *, std::string>,
             std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> drivers;
  };
  std::map<std::string, Net> nets;
  std::map<uint8_t *, Net *> addresses;
};

void Runtime::WideAnalogDeleter::operator()(WideAnalogState *state) const {
  delete state;
}

void Runtime::add_wide_analog_signal(const std::string &name, unsigned bits) {
  if (!wide_analog)
    wide_analog.reset(new WideAnalogState);
  auto &net = wide_analog->nets[name];
  net.bits = bits;
  size_t bytes = (size_t(bits) + 7) / 8;
  net.storage.resize(bytes);
  net.ones.resize(bytes);
  net.zeros.resize(bytes);
  signals.emplace(name, Signal{net.storage.data(), bits});
  wide_analog->addresses.emplace(net.storage.data(), &net);
  trace.add(name, net.storage.data(), bits);
}

void Binding::drive(const std::string &name,
                    const std::vector<uint8_t> &value,
                    const std::vector<uint8_t> &mask) {
  runtime.drive_analog(this, name, port(name), value, mask);
}

void Runtime::drive_analog(const Binding *binding, const std::string &name,
                           Signal signal, const std::vector<uint8_t> &value,
                           const std::vector<uint8_t> &mask) {
  if (signal.bits <= 64) {
    auto it = analog_addresses.find(signal.data);
    if (it == analog_addresses.end())
      throw std::runtime_error("native drive of a non-analog port " + name);
    if (signal.bits != it->second->bits || value.size() != signal.bytes() ||
        mask.size() != signal.bytes())
      throw std::runtime_error("native vector analog drive width mismatch " + name);
    uint64_t v = 0, m = 0;
    std::memcpy(&v, value.data(), value.size());
    std::memcpy(&m, mask.data(), mask.size());
    drive_analog(binding, name, signal, v, m);
    return;
  }
  if (!wide_analog)
    throw std::runtime_error("native drive of a non-analog port " + name);
  auto it = wide_analog->addresses.find(signal.data);
  if (it == wide_analog->addresses.end())
    throw std::runtime_error("native drive of a non-analog port " + name);
  auto &net = *it->second;
  if (signal.bits != net.bits || value.size() != net.storage.size() ||
      mask.size() != net.storage.size())
    throw std::runtime_error("native vector analog drive width mismatch " + name);
  auto key = std::make_pair(binding, name);
  auto driver = net.drivers.find(key);
  if (driver == net.drivers.end()) {
    net.drivers.emplace(std::move(key), std::make_pair(value, mask));
  } else {
    // Validate both operands before changing either one. Reuse allocations
    // across deltas; net storage and previously returned Signals stay stable.
    std::copy(value.begin(), value.end(), driver->second.first.begin());
    std::copy(mask.begin(), mask.end(), driver->second.second.begin());
  }
}

bool Runtime::resolve_wide_analog() {
  bool changed = false;
  for (auto &entry : wide_analog->nets) {
    auto &net = entry.second;
    std::fill(net.ones.begin(), net.ones.end(), 0);
    std::fill(net.zeros.begin(), net.zeros.end(), 0);
    for (const auto &driver : net.drivers) {
      const auto &value = driver.second.first;
      const auto &mask = driver.second.second;
      for (size_t i = 0; i < net.storage.size(); ++i) {
        net.ones[i] |= value[i] & mask[i];
        net.zeros[i] |= uint8_t(~value[i]) & mask[i];
      }
    }
    for (size_t i = 0; i < net.storage.size(); ++i)
      net.ones[i] &= uint8_t(~net.zeros[i]);
    if (net.bits % 8)
      net.ones.back() &= (1u << (net.bits % 8)) - 1;
    changed |= bool(std::memcmp(net.storage.data(), net.ones.data(),
                                net.storage.size()));
    std::memcpy(net.storage.data(), net.ones.data(), net.storage.size());
  }
  return changed;
}
} // namespace chipyard_arc
