#include "runtime.h"
#include <algorithm>
#include <initializer_list>

extern "C" {
long long plusarg_file_mem_init(const char *, unsigned char, int, int)
    __attribute__((weak));
void plusarg_file_mem_read(long long, long long, long long *)
    __attribute__((weak));
void plusarg_file_mem_write(long long, long long, long long)
    __attribute__((weak));
}
namespace chipyard_arc {
namespace {
// Native parameter text is an integer literal. Accumulate modulo the port
// width so negative constants sign extend and wide constants never truncate
// through a host scalar. Parse once when the binding is configured.
std::vector<uint8_t> analog_constant(const std::string &text, unsigned bits) {
  std::vector<uint8_t> value((size_t(bits) + 7) / 8);
  size_t pos = 0;
  bool negative = !text.empty() && text[0] == '-';
  if (!text.empty() && (text[0] == '-' || text[0] == '+'))
    ++pos;
  unsigned base = 10;
  if (pos < text.size() && text[pos] == '0') {
    base = 8;
    if (pos + 1 < text.size() &&
        (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
      base = 16;
      pos += 2;
    }
  }
  if (pos == text.size())
    throw std::runtime_error("invalid native analog constant: " + text);
  for (; pos < text.size(); ++pos) {
    char c = text[pos];
    unsigned digit = c >= '0' && c <= '9'   ? unsigned(c - '0')
                     : c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10)
                                            : 16;
    if (digit >= base)
      throw std::runtime_error("invalid native analog constant: " + text);
    unsigned carry = digit;
    for (auto &byte : value) {
      carry += unsigned(byte) * base;
      byte = uint8_t(carry);
      carry >>= 8;
    }
  }
  if (negative) {
    unsigned carry = 1;
    for (auto &byte : value) {
      carry += uint8_t(~byte);
      byte = uint8_t(carry);
      carry >>= 8;
    }
  }
  if (bits % 8)
    value.back() &= (1u << (bits % 8)) - 1;
  return value;
}

class DigitalGPIO : public Binding {
  std::vector<uint8_t> constant, constant_mask;
  uint64_t scalar_constant = 0;

public:
  using Binding::Binding;
  void configure() override {
    if (module == "AnalogConst") {
      unsigned bits = port("io").bits;
      if (number("WIDTH", bits) != bits)
        throw std::runtime_error(
            "native AnalogConst port/parameter width mismatch");
      auto value = analog_constant(text("CONST", "0"), bits);
      if (bits <= 64) {
        scalar_constant = 0;
        for (unsigned i = 0; i < value.size(); ++i)
          scalar_constant |= uint64_t(value[i]) << (i * 8);
        constant.clear();
        constant_mask.clear();
      } else {
        constant_mask.assign(value.size(), 255);
        constant = std::move(value);
      }
    }
  }
  bool combinational() override {
    if (module == "AnalogConst") {
      if (constant.empty())
        drive("io", scalar_constant, ~uint64_t(0));
      else
        drive("io", constant, constant_mask);
      return false;
    }
    drive("pad", get("o"), get("oe") ? 1 : 0);
    return port("i").set(get("ie") ? get("pad") : 0);
  }

public:
  constexpr BindingCallbackContract<DigitalGPIO>
  native_callback_contract() const {
    return reviewed_base_callback_contract<DigitalGPIO>();
  }
};
// The two edge processes and combinational equations of SPIFlashMemCtrl.sv.
// Memory requests call the original plusarg_file_mem C++ implementation.
class SPIFlash : public Binding {
  enum State {
    Standby,
    GetCmd,
    GetAddrS,
    GetAddrQ,
    Dummy,
    PutDataS,
    PutDataQ,
    GetDataS,
    GetDataQ,
    Error = 15
  };
  unsigned state = Standby, count = 0, dummy = 0, cmd = 0, data_out = 0;
  uint32_t buffer = 0, address = 0;
  bool drive_output = false, previous_cs = false;
  long long memory = 0, response = 0;
  uint64_t address_mask = 0;
  static bool contains(unsigned value, std::initializer_list<unsigned> set) {
    return std::find(set.begin(), set.end(), value) != set.end();
  }
  bool quad() const { return contains(state, {GetAddrQ, GetDataQ, PutDataQ}); }
  bool reading() const {
    return contains(cmd, {0x03, 0x0b, 0x6b, 0xeb, 0x13, 0x0c, 0x6c, 0xec});
  }
  struct Next {
    unsigned state, count, dummy, cmd;
    uint32_t buffer, address;
    bool request, read;
  };
  Next next() const {
    unsigned pins = 0;
    for (unsigned bit = 0; bit < 4; ++bit)
      pins |= get("dq_" + std::to_string(bit)) << bit;
    Next n{state,
           state == Dummy ? count : (count + (quad() ? 4 : 1)) & 63,
           state == Dummy ? (dummy + 1) & 255 : 0,
           cmd,
           quad() ? (buffer << 4) | pins : (buffer << 1) | (pins & 1),
           address,
           false,
           false};
    bool cmd_done = state == GetCmd && n.count == 8;
    if (cmd_done)
      n.cmd = n.buffer & 255;
    bool address4 = contains(cmd, {0x13, 0x0c, 0x6c, 0xec, 0x12, 0x34, 0x3e});
    bool address_done = contains(state, {GetAddrS, GetAddrQ}) &&
                        n.count == (address4 ? 40 : 32);
    bool aligned = (n.count & 7) == 0;
    if (state != Dummy) {
      if (address_done)
        n.address = address4 ? n.buffer : n.buffer & 0xffffff;
      else if (contains(state, {PutDataS, PutDataQ, GetDataS, GetDataQ}) &&
               aligned)
        ++n.address;
    }
    bool quad_address = contains(n.cmd, {0xeb, 0xec, 0x38, 0x3e});
    bool quad_data = quad_address || contains(n.cmd, {0x6b, 0x6c, 0x32, 0x34});
    bool valid = contains(n.cmd, {0x03, 0x0b, 0x6b, 0xeb, 0x13, 0x0c, 0x6c,
                                  0xec, 0x02, 0x32, 0x38, 0x12, 0x34, 0x3e});
    bool has_dummy = contains(cmd, {0x0b, 0x6b, 0xeb, 0x0c, 0x6c, 0xec});
    unsigned transfer = quad_data ? (reading() ? PutDataQ : GetDataQ)
                                  : (reading() ? PutDataS : GetDataS);
    switch (state) {
    case Standby:
      if (!get("cs_0"))
        n.state = GetCmd;
      break;
    case GetCmd:
      if (cmd_done)
        n.state = valid ? (quad_address ? GetAddrQ : GetAddrS) : Error;
      break;
    case GetAddrS:
    case GetAddrQ:
      if (address_done)
        n.state = has_dummy ? Dummy : transfer;
      break;
    case Dummy:
      if (dummy == 7)
        n.state = transfer;
      break;
    case PutDataS:
    case PutDataQ:
    case GetDataS:
    case GetDataQ:
    case Error:
      if (get("cs_0"))
        n.state = Standby;
      break;
    }
    n.read = contains(n.state, {PutDataS, PutDataQ});
    n.request = (n.read || contains(state, {GetDataS, GetDataQ})) && aligned;
    return n;
  }

public:
  using Binding::Binding;
  void initialize() override {
    if (!plusarg_file_mem_init || !plusarg_file_mem_read ||
        !plusarg_file_mem_write)
      throw std::runtime_error("SPI flash file-memory driver is not linked");
    auto capacity = number("CAPACITY_BYTES");
    unsigned bits = 0;
    for (auto value = capacity > 0 ? capacity - 1 : 0; value; value >>= 1)
      ++bits;
    if (!capacity || bits >= 31)
      throw std::runtime_error(
          "unsupported SPI flash address width in original file-memory driver");
    address_mask = (uint64_t(1) << bits) - 1;
    auto filename = runtime.option(text("PLUSARG") + "=");
    if (filename.empty())
      throw std::runtime_error("No memory image provided. Use +" +
                               text("PLUSARG") + "=<file>");
    memory =
        plusarg_file_mem_init(filename.c_str(), !number("READONLY"), bits, 1);
  }
  bool combinational() override {
    bool q = quad();
    unsigned value =
        ((data_out >> 4) & 1) | (((data_out >> (q ? 5 : 7)) & 1) << 1) |
        (((data_out >> 6) & 1) << 2) | (((data_out >> 7) & 1) << 3);
    unsigned mask = drive_output ? (q ? 15 : 2) : 0;
    for (unsigned bit = 0; bit < 4; ++bit)
      drive("dq_" + std::to_string(bit), (value >> bit) & 1, (mask >> bit) & 1);
    return false;
  }
  void sample() override {
    bool clock = get("sck"), cs = get("cs_0"), reset = get("reset");
    bool rising = clock && !old_clock, falling = !clock && old_clock;
    bool async = (cs && !previous_cs) || (reset && !old_reset);
    old_clock = clock;
    old_reset = reset;
    previous_cs = cs;
    if (!rising && !falling && !async)
      return;
    auto n = next();
    if (rising && !reset && n.request) {
      uint64_t request_address =
          (reading() ? n.address : address) & address_mask;
      if (n.read)
        plusarg_file_mem_read(memory, request_address, &response);
      else
        plusarg_file_mem_write(memory, request_address, n.buffer & 255);
    }
    if (rising || async) {
      if (cs || reset) {
        count = dummy = cmd = 0;
        state = Standby;
      } else {
        buffer = n.buffer;
        count = n.count;
        state = n.state;
        dummy = n.dummy;
        address = n.address;
        cmd = n.cmd;
      }
    }
    if (falling || async) {
      if (cs || reset)
        drive_output = false;
      else {
        drive_output = contains(state, {PutDataS, PutDataQ});
        data_out = (count & 7) == 0 ? response & 255
                                    : (data_out << (quad() ? 4 : 1)) & 255;
      }
    }
    mark_changed();
  }

public:
  constexpr BindingCallbackContract<SPIFlash> native_callback_contract() const {
    return reviewed_base_callback_contract<SPIFlash>();
  }
};
template <class T> void add(const char *name) {
  register_binding(name, [](Runtime &r, const std::string &n) {
    return make_binding_instance<T>(r, n);
  });
}
} // namespace
void register_analog_bindings() {
#define ARC_ANALOG_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_ANALOG_MODEL
}
} // namespace chipyard_arc
