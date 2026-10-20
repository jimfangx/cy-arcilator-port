#include "compat.h"
#include "runtime.h"
#include "svdpi.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <random>

// Weak declarations let a binding diagnose missing selected driver sources;
// configurations do not need to link drivers for peripherals they do not use.
extern "C" {
int tsi_tick(int, unsigned char, unsigned char *, int, unsigned char *,
             unsigned char, int *) __attribute__((weak));
void uart_init(const char *, int, int) __attribute__((weak));
void uart_tick(int, unsigned char, unsigned char *, char, unsigned char *,
               unsigned char, char *) __attribute__((weak));
int jtag_tick(unsigned char *, unsigned char *, unsigned char *,
              unsigned char *, unsigned char) __attribute__((weak));
void *memory_init(int, long long, long long, long long, long long, long long,
                  long long, int) __attribute__((weak));
void memory_tick(void *, unsigned char, unsigned char, unsigned char *,
                 long long, int, int, int, unsigned char, unsigned char *,
                 long long, int, int, int, unsigned char, unsigned char *, int,
                 svOpenArrayHandle, unsigned char, unsigned char *,
                 unsigned char, int *, int *, svOpenArrayHandle,
                 unsigned char *, unsigned char *, unsigned char, int *, int *)
    __attribute__((weak));
}
namespace chipyard_arc {
void register_peripheral_bindings();
void register_additional_bindings();
void register_cosim_bindings();
void register_spike_bindings();
void register_analog_bindings();
namespace {
class AsyncBank : public Binding {
public:
  using Binding::Binding;
  void sample() override {
    bool clock = get("clock"), reset = get("reset");
    if ((clock && !old_clock) || (reset && !old_reset))
      stage("q", port(reset ? "reset_data" : "d").copy());
    old_clock = clock;
    old_reset = reset;
  }
};

class WideShift : public Binding {
  std::vector<uint8_t> result;
  unsigned mode = 0;

public:
  using Binding::Binding;
  void initialize() override {
    if (port("data").bits != port("amount").bits ||
        port("data").bits != port("out").bits)
      throw std::runtime_error("native wide shift port width mismatch");
    result.resize(port("out").bytes());
    mode = number("OP");
    if (mode > 2)
      throw std::runtime_error("invalid native wide shift operation");
  }
  bool combinational() override {
    auto input = port("data"), amount = port("amount");
    uint64_t shift = 0;
    for (size_t i = 0; i < amount.bytes(); ++i) {
      uint8_t value = amount.data[i];
      if (i == amount.bytes() - 1 && amount.bits % 8)
        value &= (1u << (amount.bits % 8)) - 1;
      if (i >= 8) {
        if (value) {
          shift = input.bits;
          break;
        }
      } else
        shift |= uint64_t(value) << (i * 8);
    }
    bool negative =
        mode == 2 &&
        ((input.data[(input.bits - 1) / 8] >> ((input.bits - 1) % 8)) & 1);
    uint8_t fill = negative ? 255 : 0;
    std::fill(result.begin(), result.end(), fill);
    if (shift < input.bits) {
      size_t bytes = shift / 8;
      unsigned bits = shift % 8;
      auto byte = [&](int64_t index) -> uint8_t {
        if (index < 0)
          return 0;
        if (uint64_t(index) >= input.bytes())
          return fill;
        uint8_t value = input.data[index];
        if (uint64_t(index) == input.bytes() - 1 && input.bits % 8) {
          value &= (1u << (input.bits % 8)) - 1;
          if (negative)
            value |= uint8_t(255u << (input.bits % 8));
        }
        return value;
      };
      for (size_t i = 0; i < result.size(); ++i) {
        if (mode == 0)
          result[i] = uint8_t(byte(int64_t(i) - int64_t(bytes)) << bits) |
                      (bits ? uint8_t(byte(int64_t(i) - int64_t(bytes) - 1) >>
                                      (8 - bits))
                            : 0);
        else
          result[i] = uint8_t(byte(i + bytes) >> bits) |
                      (bits ? uint8_t(byte(i + bytes + 1) << (8 - bits)) : 0);
      }
    }
    if (input.bits % 8)
      result.back() &= (1u << (input.bits % 8)) - 1;
    return port("out").assign(result);
  }
};

// Parse directly into the declared port width. SV integers and plusargs can
// exceed the host word size; truncation and negative values wrap at that width.
std::vector<uint8_t> integer_bits(const std::string &value, unsigned bits,
                                  unsigned base) {
  std::vector<uint8_t> result((bits + 7) / 8);
  size_t position = 0;
  bool negative = !value.empty() && value[0] == '-';
  if (!value.empty() && (value[0] == '-' || value[0] == '+'))
    ++position;
  bool hex = value.size() >= position + 2 && value[position] == '0' &&
             (value[position + 1] == 'x' || value[position + 1] == 'X');
  if (!base)
    base = hex ? 16 : 10;
  if (hex && base == 16)
    position += 2;
  bool digit_seen = false;
  for (; position < value.size(); ++position) {
    char c = value[position];
    if (c == '_')
      continue;
    unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                     : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                            : base;
    if (digit >= base)
      throw std::runtime_error("invalid native integer plusarg");
    digit_seen = true;
    unsigned carry = digit;
    for (auto &byte : result) {
      carry += byte * base;
      byte = carry & 255;
      carry >>= 8;
    }
  }
  if (!digit_seen)
    throw std::runtime_error("empty native integer plusarg");
  if (negative) {
    unsigned carry = 1;
    for (auto &byte : result) {
      carry += uint8_t(~byte);
      byte = carry & 255;
      carry >>= 8;
    }
  }
  if (bits % 8)
    result.back() &= (1u << (bits % 8)) - 1;
  return result;
}

// FIRRTL memory ports retain their own clocks and sample before any storage
// commit. Unwritten pages read as zero, matching native two-state
// initialization.
class SparseMemory : public Binding {
  struct ReadPort {
    Signal clock, address, enable, data;
    bool old_clock = false;
  };
  struct WritePort {
    Signal clock, address, enable, data, mask;
    bool old_clock = false;
  };
  struct Write {
    uint64_t address;
    std::vector<uint8_t> data, mask;
  };
  std::vector<ReadPort> reads;
  std::vector<WritePort> writes;
  std::vector<Write> pending_writes;
  std::map<uint64_t, std::vector<uint8_t>> pages;
  uint64_t depth = 0, words_per_page = 0;
  unsigned bits = 0, masks = 0, grain = 0, bytes = 0;
  bool synchronous = false, read_new = false;

  Signal checked(const std::string &name, unsigned width) {
    auto value = port(name);
    if (value.bits != width)
      throw std::runtime_error("native sparse-memory port width mismatch: " +
                               name);
    return value;
  }
  std::vector<uint8_t> read(uint64_t address) const {
    std::vector<uint8_t> value(bytes, 0);
    if (address >= depth)
      return value;
    auto page = pages.find(address / words_per_page);
    if (page != pages.end())
      std::memcpy(value.data(),
                  page->second.data() + (address % words_per_page) * bytes,
                  bytes);
    return value;
  }
  bool overlay(uint8_t *destination, const Write &write) const {
    bool changed = false;
    for (unsigned bit = 0; bit < bits; ++bit) {
      auto lane = bit / grain;
      if (!((write.mask[lane / 8] >> (lane % 8)) & 1))
        continue;
      auto mask = uint8_t(1u << (bit % 8));
      auto value = (write.data[bit / 8] & mask);
      changed |= (destination[bit / 8] & mask) != value;
      destination[bit / 8] = (destination[bit / 8] & ~mask) | value;
    }
    return changed;
  }
  bool put(const Write &write) {
    if (write.address >= depth)
      return false;
    auto key = write.address / words_per_page;
    auto page = pages.find(key);
    if (page == pages.end()) {
      std::vector<uint8_t> value(bytes, 0);
      if (!overlay(value.data(), write))
        return false;
      auto &created = pages[key];
      created.resize(words_per_page * bytes, 0);
      std::memcpy(created.data() + (write.address % words_per_page) * bytes,
                  value.data(), bytes);
      return true;
    }
    return overlay(
        page->second.data() + (write.address % words_per_page) * bytes, write);
  }

public:
  using Binding::Binding;
  void initialize() override {
    depth = number("DEPTH");
    bits = number("BITS");
    masks = number("MASKS");
    if (!depth || !bits || !masks || bits % masks || number("READ_LATENCY") > 1)
      throw std::runtime_error("invalid native sparse-memory layout");
    grain = bits / masks;
    bytes = (uint64_t(bits) + 7) / 8;
    words_per_page = std::max<uint64_t>(1, 4096 / bytes);
    synchronous = number("READ_LATENCY") != 0;
    read_new = number("READ_NEW") != 0;
    unsigned address_bits = 1;
    for (auto remaining = depth - 1; remaining > 1; remaining >>= 1)
      ++address_bits;
    for (unsigned i = 0; i < number("READS"); ++i) {
      auto name = "r" + std::to_string(i);
      reads.push_back({checked(name + "_clock", 1),
                       checked(name + "_address", address_bits),
                       checked(name + "_enable", 1),
                       checked(name + "_data", bits)});
      reads.back().data.assign(std::vector<uint8_t>(bytes, 0));
    }
    for (unsigned i = 0; i < number("WRITES"); ++i) {
      auto name = "w" + std::to_string(i);
      writes.push_back({checked(name + "_clock", 1),
                        checked(name + "_address", address_bits),
                        checked(name + "_enable", 1),
                        checked(name + "_data", bits),
                        checked(name + "_mask", masks)});
    }
  }
  bool combinational() override {
    bool changed = false;
    if (!synchronous)
      for (auto &port : reads)
        changed |= port.data.assign(port.enable.get()
                                        ? read(port.address.get())
                                        : std::vector<uint8_t>(bytes, 0));
    return changed;
  }
  void sample() override {
    for (auto &port : writes) {
      bool clock = port.clock.get(), edge = clock && !port.old_clock;
      port.old_clock = clock;
      if (edge && port.enable.get())
        pending_writes.push_back(
            {port.address.get(), port.data.copy(), port.mask.copy()});
    }
    for (unsigned i = 0; i < reads.size(); ++i) {
      auto &port = reads[i];
      bool clock = port.clock.get(), edge = clock && !port.old_clock;
      port.old_clock = clock;
      if (!synchronous || !edge || !port.enable.get())
        continue;
      auto value = read(port.address.get());
      if (read_new)
        for (const auto &write : pending_writes)
          if (write.address == port.address.get() && write.address < depth)
            overlay(value.data(), write);
      stage("r" + std::to_string(i) + "_data", value);
    }
  }
  bool commit() override {
    bool changed = Binding::commit();
    for (const auto &write : pending_writes)
      changed |= put(write);
    pending_writes.clear();
    return changed;
  }
};

// Residual LLHD references use this storage when register inference cannot
// preserve their procedural write timing. Pending NBA entries contain only
// the bits actually written; committing them never rolls back unrelated
// blocking writes made later in the same activation.
class SignalMemory : public Binding {
  struct Write {
    uint64_t offset, count, value;
  };
  std::vector<uint8_t> bits;
  std::vector<Write> delayed;
  uint64_t width = 0, revision = 1;
  SignalMemory *storage = this;
  bool put(uint64_t offset, uint64_t count, uint64_t value) {
    bool changed = false;
    for (uint64_t i = 0; i < count; ++i) {
      auto bit = offset + i;
      uint8_t mask = uint8_t(1u << (bit % 8));
      uint8_t next = (value >> i) & 1 ? mask : 0;
      changed |= (bits[bit / 8] & mask) != next;
      bits[bit / 8] = (bits[bit / 8] & ~mask) | next;
    }
    if (changed && !++revision)
      ++revision;
    return changed;
  }

public:
  using Binding::Binding;
  void initialize() override {
    width = number("BITS");
    if (!width || width > (1u << 24) || port("handle").bits != 64 ||
        port("version").bits != 64)
      throw std::runtime_error("invalid native signal-memory layout");
    auto initial = integer_bits(text("INIT", "0"), unsigned(width), 0);
    if (auto key = text("KEY"); !key.empty()) {
      storage =
          static_cast<SignalMemory *>(runtime.share_signal_memory(key, this));
      if (storage != this && (storage->width != width ||
                              integer_bits(storage->text("INIT", "0"),
                                           unsigned(width), 0) != initial))
        throw std::runtime_error("conflicting native global signal layout: " +
                                 key);
    }
    if (storage == this)
      bits = std::move(initial);
    port("handle").set(uint64_t(reinterpret_cast<uintptr_t>(storage)));
    port("version").set(storage->revision);
  }
  uint64_t read(uint64_t offset, uint64_t available, uint64_t count) const {
    if (!count || count > 64)
      throw std::runtime_error("invalid native signal-memory read width");
    if (offset >= width)
      return 0;
    count = std::min({count, available, width - offset});
    uint64_t result = 0;
    for (uint64_t i = 0; i < count; ++i)
      result |= uint64_t((bits[(offset + i) / 8] >> ((offset + i) % 8)) & 1)
                << i;
    return result;
  }
  void write(uint64_t offset, uint64_t available, uint64_t count,
             uint64_t value, uint64_t mode, uint64_t enable) {
    if (!count || count > 64 || mode > 1)
      throw std::runtime_error("invalid native signal-memory write");
    if (!enable || offset >= width)
      return;
    count = std::min({count, available, width - offset});
    if (!count)
      return;
    if (mode)
      delayed.push_back({offset, count, value});
    else
      put(offset, count, value);
  }
  bool combinational() override {
    return port("version").set(storage->revision);
  }
  bool commit() override {
    bool changed = Binding::commit();
    for (const auto &write : delayed)
      changed |= put(write.offset, write.count, write.value);
    delayed.clear();
    return port("version").set(storage->revision) | changed;
  }
};

// Every delay owns its history for this physical instance and Runtime.
// Inertial assignments retain the latest still-pending transition. Transport
// nodes retain all transitions, including several at the same physical time.
class Delay : public Binding {
  std::map<uint64_t, std::deque<std::vector<uint8_t>>> events;
  std::vector<uint8_t> previous;
  Signal input{}, output{}, delay_input{};
  uint64_t delay = 0;
  bool inertial = false, observed = false;

public:
  using Binding::Binding;
  void initialize() override {
    input = port("value");
    output = port("q");
    delay = number("DELAY_FS");
    auto delayPort = ports.find("delay_fs");
    if (delayPort != ports.end())
      delay_input = delayPort->second;
    inertial = number("INERTIAL") != 0;
    if ((!delay_input.data && !delay) || delay == never ||
        (delay_input.data && delay_input.bits != 64) || input.bits != output.bits)
      throw std::runtime_error("invalid native delayed-signal layout");
    output.assign(integer_bits(text("INIT", "0"), output.bits, 0));
  }
  bool combinational() override {
    auto value = input.copy();
    if (input.bits % 8)
      value.back() &= (1u << (input.bits % 8)) - 1;
    if (observed && value == previous)
      return false;
    observed = true;
    previous = value;
    if (delay_input.data)
      delay = delay_input.get();
    if (delay >= never - runtime.time_fs())
      throw std::runtime_error("native delayed signal exceeds time range");
    if (inertial) {
      events.clear();
      if (value == output.copy())
        return false;
    }
    if (!delay)
      return output.assign(value);
    events[runtime.time_fs() + delay].push_back(std::move(value));
    return false;
  }
  uint64_t next_time_fs() const override {
    return events.empty() ? never : events.begin()->first;
  }
  void timed() override {
    auto it = events.begin();
    if (it == events.end() || it->first > runtime.time_fs())
      throw std::runtime_error("native delayed signal fired before its event");
    output.assign(it->second.front());
    it->second.pop_front();
    if (it->second.empty())
      events.erase(it);
  }
};

class SignalState : public Binding {
public:
  using Binding::Binding;
  void initialize() override {
    if (port("value").bits != port("q").bits || port("write").bits != 1)
      throw std::runtime_error("native signal-state port width mismatch");
    port("q").assign(integer_bits(text("INIT", "0"), port("q").bits, 0));
  }
  void sample() override {
    if (get("write"))
      stage("q", port("value").copy());
  }
};

class Latch : public SignalState {
  Signal data{}, enable{}, output{};

public:
  using SignalState::SignalState;
  void initialize() override {
    SignalState::initialize();
    data = port("value");
    enable = port("write");
    output = port("q");
  }
  bool combinational() override {
    if (!enable.get())
      return false;
    size_t whole = data.bits / 8;
    bool changed = std::memcmp(output.data, data.data, whole) != 0;
    if (data.bits % 8) {
      uint8_t last = data.data[whole] & ((1u << (data.bits % 8)) - 1);
      changed |= output.data[whole] != last;
      std::memmove(output.data, data.data, whole);
      output.data[whole] = last;
    } else
      std::memmove(output.data, data.data, whole);
    return changed;
  }
  void sample() override {}
};

class FeedbackWire : public Binding {
public:
  using Binding::Binding;
  void initialize() override {
    if (port("value").bits != port("q").bits)
      throw std::runtime_error("native feedback port width mismatch");
    port("q").assign(std::vector<uint8_t>((port("q").bits + 7) / 8, 0));
  }
  bool combinational() override {
    return port("q").assign(port("value").copy());
  }
};

class Combinational : public Binding {
  bool latched = false;

public:
  using Binding::Binding;
  void initialize() override {
    if (module == "plusarg_reader") {
      auto format = text("FORMAT");
      auto percent = format.find('%');
      if (percent == std::string::npos)
        throw std::runtime_error("invalid plusarg_reader format");
      auto value = runtime.option(format.substr(0, percent));
      auto parsed = integer_bits(text("DEFAULT", "0"), port("out").bits, 0);
      if (!value.empty()) {
        char code = format.at(percent + 1);
        unsigned base = code == 'h' || code == 'x' ? 16
                        : code == 'b'              ? 2
                        : code == 'o'              ? 8
                        : code == 'd'              ? 10
                                                   : 0;
        if (!base)
          throw std::runtime_error("unsupported native plusarg format " +
                                   format);
        parsed = integer_bits(value, port("out").bits, base);
      }
      port("out").assign(parsed);
    }
  }
  bool combinational() override {
    if (module == "plusarg_reader")
      return false;
    if (module == "EICG_wrapper") {
      if (!get("in"))
        latched = get("en") || get("test_en");
      return port("out").set(get("in") && latched);
    }
    if (module == "GenericDigitalInIOCell")
      return port("i").set(get("ie") ? get("pad") : 0);
    if (module == "GenericDigitalOutIOCell")
      return port("pad").set(get("oe") ? get("o") : 0);
    if (module == "ClockInverter")
      return port("clockOut").set(!get("clockIn"));
    if (module == "ClockOr2")
      return port("clockOut").set(get("clocksIn_0") || get("clocksIn_1"));
    if (module == "ClockSignalNor2")
      return port("clockOut").set(!(get("clockIn") || get("signalIn")));
    throw std::runtime_error("incomplete native combinational binding " +
                             module);
  }
};
class ClockSource : public Binding {
  uint64_t half = 0, next = never;

public:
  using Binding::Binding;
  void initialize() override {
    double half_ps;
    if (module == "ClockSourceAtFreqFromPlusArg") {
      auto format = text("FORMAT");
      auto percent = format.find('%');
      auto override = runtime.option(format.substr(0, percent));
      double frequency = override.empty()
                             ? std::stod(text("DEFAULT_FREQUENCY_MHZ"))
                         : format.back() == 'd' ? double(std::stoll(override))
                                                : std::stod(override);
      if (!std::isfinite(frequency) || frequency <= 0)
        throw std::runtime_error("invalid native clock frequency");
      half_ps = 500000.0 / frequency;
    } else
      half_ps = std::stod(text("PERIOD")) * 500.0;
    if (!std::isfinite(half_ps) || half_ps <= 0 || half_ps >= double(INT64_MAX))
      throw std::runtime_error("invalid native clock period");
    half = std::llround(half_ps);
    if (!half)
      throw std::runtime_error("native clock period rounds to zero");
    next = half;
    port("clk").set(0);
  }
  uint64_t next_time() const override { return next; }
  void timed() override {
    port("clk").set(!get("clk") && get("power") && !get("gate"));
    next += half;
  }
};
class ClockFlop : public Binding {
public:
  using Binding::Binding;
  void sample() override {
    bool clock = get("clockIn");
    if (clock && !old_clock)
      stage("clockOut", get("d"));
    old_clock = clock;
  }
};
class TSI : public Binding {
  unsigned char valid = 0, ready = 0;
  int bits = 0;

public:
  using Binding::Binding;
  void initialize() override {
    if (!tsi_tick)
      throw std::runtime_error("SimTSI driver is not linked");
  }
  void sample() override {
    bool clock = get("clock");
    bool edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    int exit = 0;
    if (get("reset")) {
      valid = ready = 0;
      bits = 0;
    } else
      exit = tsi_tick(number("CHIPID"), get("tsi_out_valid"), &ready,
                      get("tsi_out_bits"), &valid, get("tsi_in_ready"), &bits);
    stage("tsi_out_ready", ready);
    stage("tsi_in_valid", valid);
    stage("tsi_in_bits", uint32_t(bits));
    stage("exit", uint32_t(exit));
  }
};
class UART : public Binding {
  unsigned char valid = 0, ready = 0;
  char bits = 0;

public:
  using Binding::Binding;
  void initialize() override {
    if (!uart_init || !uart_tick)
      throw std::runtime_error("SimUART driver is not linked");
    uart_init(runtime.option("uartlog=").c_str(), number("UARTNO"),
              number("FORCEPTY"));
  }
  void sample() override {
    bool clock = get("clock");
    bool edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    if (get("reset")) {
      valid = ready = 0;
      bits = 0;
    } else
      uart_tick(number("UARTNO"), get("serial_out_valid"), &ready,
                get("serial_out_bits"), &valid, get("serial_in_ready"), &bits);
    stage("serial_out_ready", ready);
    stage("serial_in_valid", valid);
    stage("serial_in_bits", uint8_t(bits));
  }
};
class DRAM : public Binding {
  void *channel = nullptr;
  std::vector<uint8_t> r_data;

public:
  using Binding::Binding;
  void initialize() override {
    if (!memory_init || !memory_tick)
      throw std::runtime_error("SimDRAM driver is not linked");
    r_data.resize(port("axi_r_bits_data").bytes());
  }
  void sample() override {
    bool clock = get("clock");
    bool edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    unsigned char ar = 0, aw = 0, w = 0, rv = 0, rl = 0, bv = 0;
    int rid = 0, rresp = 0, bid = 0, bresp = 0;
    if (!get("reset")) {
      if (!channel)
        channel = memory_init(number("CHIP_ID"), number("MEM_SIZE"),
                              number("DATA_BITS") / 8, number("LINE_SIZE"),
                              number("ID_BITS"), number("CLOCK_HZ"),
                              number("MEM_BASE"), number("ADDR_BITS"));
      auto write = port("axi_w_bits_data").copy();
      OpenArray wa{write.data(), write.size(), 1},
          ra{r_data.data(), r_data.size(), 1};
      memory_tick(channel, 0, get("axi_ar_valid"), &ar, get("axi_ar_bits_addr"),
                  get("axi_ar_bits_id"), get("axi_ar_bits_size"),
                  get("axi_ar_bits_len"), get("axi_aw_valid"), &aw,
                  get("axi_aw_bits_addr"), get("axi_aw_bits_id"),
                  get("axi_aw_bits_size"), get("axi_aw_bits_len"),
                  get("axi_w_valid"), &w, get("axi_w_bits_strb"), &wa,
                  get("axi_w_bits_last"), &rv, get("axi_r_ready"), &rid, &rresp,
                  &ra, &rl, &bv, get("axi_b_ready"), &bid, &bresp);
      stage("axi_r_bits_id", rid);
      stage("axi_r_bits_resp", rresp);
      stage("axi_r_bits_last", rl);
      stage("axi_r_bits_data", r_data);
      stage("axi_b_bits_id", bid);
      stage("axi_b_bits_resp", bresp);
    }
    stage("axi_ar_ready", ar);
    stage("axi_aw_ready", aw);
    stage("axi_w_ready", w);
    stage("axi_r_valid", rv);
    stage("axi_b_valid", bv);
  }
};
class JTAG : public Binding {
  unsigned counter = 0;
  bool previous_reset = false, initialized = false;
  unsigned char tck = 0, tms = 0, tdi = 0, trst = 0, tdo = 0;
  uint64_t output_at = never, tdo_at = never;
  unsigned char tdo_next = 0;
  int exit = 0;
  unsigned char random_tdo = 0;

public:
  using Binding::Binding;
  void initialize() override {
    if (!jtag_tick)
      throw std::runtime_error("SimJTAG driver is not linked");
    std::mt19937 random(std::stoull(runtime.option("seed=", "1")));
    random_tdo = random() & 1;
  }
  bool combinational() override {
    auto next = get("jtag_TDO_driven") ? get("jtag_TDO_data") : random_tdo;
    if (next != tdo_next) {
      tdo_next = next;
      tdo_at = runtime.time() + 100;
    }
    return false;
  }
  void sample() override {
    bool clock = get("clock");
    bool edge = clock && !old_clock;
    old_clock = clock;
    if (!edge)
      return;
    bool reset = get("reset"), changed = false;
    if (reset || previous_reset) {
      counter = number("TICK_DELAY", 50);
      initialized = false;
      tck = !tck;
      exit = 0;
      changed = true;
    } else {
      if (get("enable") && initialized) {
        if (!counter) {
          exit = jtag_tick(&tck, &tms, &tdi, &trst, tdo);
          changed = true;
        }
        counter = counter ? counter - 1 : number("TICK_DELAY", 50);
      }
      initialized |= bool(get("init_done"));
    }
    previous_reset = reset;
    if (changed)
      output_at = runtime.time() + 100;
  }
  uint64_t next_time() const override { return std::min(output_at, tdo_at); }
  void timed() override {
    if (runtime.time() == tdo_at) {
      tdo = tdo_next;
      tdo_at = never;
    }
    if (runtime.time() == output_at) {
      port("jtag_TCK").set(tck);
      port("jtag_TMS").set(tms);
      port("jtag_TDI").set(tdi);
      port("jtag_TRSTn").set(trst);
      port("exit").set(uint32_t(exit));
      output_at = never;
    }
  }
};
template <class T> void add(const std::string &name) {
  register_binding(name, [](Runtime &r, const std::string &n) {
    return std::make_unique<T>(r, n);
  });
}
} // namespace
extern "C" uint64_t chipyard_arc_signal_read(uint64_t handle, uint64_t,
                                             uint64_t offset,
                                             uint64_t available,
                                             uint64_t count) {
  if (!handle)
    throw std::runtime_error("uninitialized native signal-memory handle");
  return reinterpret_cast<SignalMemory *>(uintptr_t(handle))
      ->read(offset, available, count);
}
extern "C" void chipyard_arc_signal_write(uint64_t handle, uint64_t,
                                          uint64_t offset, uint64_t available,
                                          uint64_t count, uint64_t value,
                                          uint64_t mode, uint64_t enable) {
  if (!handle)
    throw std::runtime_error("uninitialized native signal-memory handle");
  reinterpret_cast<SignalMemory *>(uintptr_t(handle))
      ->write(offset, available, count, value, mode, enable);
}
void register_builtin_bindings() {
#define ARC_CORE_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_CORE_MODEL
  register_peripheral_bindings();
  register_cosim_bindings();
  register_spike_bindings();
  register_analog_bindings();
  register_additional_bindings();
  void register_context_bindings();
  register_context_bindings();
}
} // namespace chipyard_arc
