#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef DROMAJO_REFERENCE
#include "native/runtime.h"
#endif

struct Commit {
  uint64_t pc, instruction, data, mstatus;
  unsigned check;
};
struct Frame {
  unsigned reset, valid, trap;
  uint64_t cause;
  std::array<Commit, 3> slots;
};
static Frame *frame = nullptr;
static unsigned next_slot = 0, calls = 0, traps = 0, initializations = 0;
static int last_status = 0;

extern "C" int __real_dromajo_init(const char *, const char *, const char *);
extern "C" int __real_dromajo_step(int, long long, int, long long, long long,
                                   bool);
extern "C" void __real_dromajo_raise_trap(int, long long);
extern "C" int dromajo_init(const char *, const char *, const char *);
extern "C" int dromajo_step(int, long long, int, long long, long long, bool);
extern "C" void dromajo_raise_trap(int, long long);
extern "C" int __wrap_dromajo_init(const char *rom, const char *dtb,
                                   const char *binary) {
  ++initializations;
  return __real_dromajo_init(rom, dtb, binary);
}
extern "C" int __wrap_dromajo_step(int hart, long long pc, int instruction,
                                   long long data, long long mstatus,
                                   bool check) {
  if (!frame || frame->reset || last_status)
    throw std::runtime_error("unexpected Dromajo step activation");
  while (next_slot < 3 && !(frame->valid >> next_slot & 1))
    ++next_slot;
  if (next_slot >= 3)
    throw std::runtime_error("extra Dromajo retirement callback");
  const auto &expected = frame->slots[next_slot++];
  if (hart != 0 || uint64_t(pc) != expected.pc ||
      uint32_t(instruction) != expected.instruction ||
      uint64_t(data) != expected.data ||
      uint64_t(mstatus) != expected.mstatus || check != bool(expected.check))
    throw std::runtime_error(
        "Dromajo slot order, packing or check mask changed");
  ++calls;
  last_status =
      __real_dromajo_step(hart, pc, instruction, data, mstatus, check);
  return last_status;
}
extern "C" void __wrap_dromajo_raise_trap(int hart, long long cause) {
  if (!frame || frame->reset || !frame->trap || hart != 0 || last_status ||
      uint64_t(cause) != frame->cause)
    throw std::runtime_error("Dromajo trap guard or cause changed");
  while (next_slot < 3 && !(frame->valid >> next_slot & 1))
    ++next_slot;
  if (next_slot != 3)
    throw std::runtime_error("Dromajo trap preceded retirement callbacks");
  ++traps;
  __real_dromajo_raise_trap(hart, cause);
}

static bool read_frame(std::istream &input, Frame &current, bool negative,
                       bool &injected) {
  if (!(input >> current.reset >> current.valid >> current.trap >>
        current.cause))
    return false;
  for (auto &slot : current.slots)
    if (!(input >> slot.pc >> slot.instruction >> slot.data >> slot.mstatus >>
          slot.check))
      throw std::runtime_error("truncated Dromajo frame");
  if (negative && !injected && traps == 2 && !current.reset && !current.trap)
    for (unsigned slot = 0; slot < 2; ++slot)
      if ((current.valid >> slot & 1) && (current.valid >> (slot + 1)) &&
          current.slots[slot].check &&
          ((current.slots[slot].instruction >> 7) & 31)) {
        current.slots[slot].data ^= 1;
        injected = true;
        break;
      }
  frame = &current;
  next_slot = 0;
  return true;
}

#ifndef DROMAJO_REFERENCE
class FrameDriver : public chipyard_arc::Binding {
  std::istream &input;
  Frame current;
  uint64_t next = 778100; // After root reset, before the first active posedge.

public:
  unsigned cycles = 0;
  bool injected = false;
  FrameDriver(chipyard_arc::Runtime &r, std::istream &input)
      : Binding(r, "DromajoFixtureDriver"), input(input) {
    instance_name = "fixture_driver";
  }
  uint64_t next_time() const override { return next; }
  void timed() override {
    if (!read_frame(input, current, true, injected))
      throw std::runtime_error(
          "Dromajo trace finished without rejecting corruption");
    auto packed = [&](const char *name, unsigned bytes,
                      uint64_t Commit::*field) {
      std::vector<uint8_t> bits(3 * bytes);
      for (unsigned slot = 0; slot < 3; ++slot)
        for (unsigned byte = 0; byte < bytes; ++byte)
          bits[slot * bytes + byte] =
              (current.slots[slot].*field) >> (byte * 8);
      runtime.signal(name).assign(bits);
    };
    runtime.signal("reset").set(current.reset);
    runtime.signal("valid").set(current.valid);
    runtime.signal("hartid").set(0);
    packed("pc", 8, &Commit::pc);
    packed("inst", 4, &Commit::instruction);
    packed("wdata", 8, &Commit::data);
    packed("mstatus", 8, &Commit::mstatus);
    unsigned check = 0;
    for (unsigned slot = 0; slot < 3; ++slot)
      check |= current.slots[slot].check << slot;
    runtime.signal("check").set(check);
    runtime.signal("int_xcpt").set(current.trap);
    runtime.signal("cause").set(current.cause);
    ++cycles;
    next += 1000;
  }
};
#endif

int main(int argc, char **argv) {
  if (argc < 2)
    throw std::runtime_error(
        "expected Dromajo frame file and driver arguments");
  auto option = [&](const std::string &prefix) {
    for (int i = 2; i < argc; ++i)
      if (std::string(argv[i]).rfind(prefix, 0) == 0)
        return std::string(argv[i]).substr(prefix.size());
    return std::string();
  };
  bool negative = !option("+negative=").empty();
  bool injected = false;
  std::ifstream input(argv[1]);
  if (!input)
    throw std::runtime_error("cannot read Dromajo frame file");
#ifdef DROMAJO_REFERENCE
  if (dromajo_init(option("+drj_rom=").c_str(), option("+drj_dtb=").c_str(),
                   option("+drj_bin=").c_str()))
    throw std::runtime_error("original Dromajo driver initialization failed");
#else
  auto runtime = chipyard_arc::create_model(argc, argv);
  if (negative) {
    auto driver = std::make_unique<FrameDriver>(*runtime, input);
    auto *observed = driver.get();
    runtime->add_binding(std::move(driver));
    if (runtime->run() != 1 || !observed->injected || last_status != 8191 ||
        initializations != 1 || traps != 2 || calls < 250)
      throw std::runtime_error(
          "real Dromajo failure did not reach Runtime::run");
    std::printf("PASS: real Dromajo mismatch rejected, %u ordered retirements, "
                "%u traps, %u frames, standard run loop failure status\n",
                calls, traps, observed->cycles);
    return 0;
  }
  runtime->initialize();
  auto packed = [&](const char *name, const Frame &f, unsigned bytes,
                    uint64_t Commit::*field) {
    std::vector<uint8_t> bits(3 * bytes);
    for (unsigned slot = 0; slot < 3; ++slot)
      for (unsigned byte = 0; byte < bytes; ++byte)
        bits[slot * bytes + byte] = (f.slots[slot].*field) >> (byte * 8);
    runtime->signal(name).assign(bits);
  };
#endif
  unsigned cycles = 0;
  Frame current;
  while (read_frame(input, current, negative, injected)) {
#ifdef DROMAJO_REFERENCE
    if (!current.reset) {
      for (unsigned slot = 0; slot < 3 && !last_status; ++slot)
        if (current.valid >> slot & 1) {
          auto &value = current.slots[slot];
          dromajo_step(0, value.pc, value.instruction, value.data,
                       value.mstatus, value.check);
        }
      if (current.trap && !last_status)
        dromajo_raise_trap(0, current.cause);
    }
#else
    auto previous_calls = calls, previous_traps = traps;
    runtime->advance(cycles * 1000 + 100, [&] {
      runtime->signal("clock").set(0);
      runtime->signal("reset").set(current.reset);
      runtime->signal("valid").set(7);
      runtime->signal("int_xcpt").set(1);
      runtime->signal("cause").set(~current.cause);
    });
    if (calls != previous_calls || traps != previous_traps)
      throw std::runtime_error("Dromajo callbacks ran on a negative edge");
    runtime->advance(cycles * 1000 + 600, [&] {
      runtime->signal("clock").set(1);
      runtime->signal("valid").set(current.valid);
      runtime->signal("hartid").set(0);
      packed("pc", current, 8, &Commit::pc);
      packed("inst", current, 4, &Commit::instruction);
      packed("wdata", current, 8, &Commit::data);
      packed("mstatus", current, 8, &Commit::mstatus);
      unsigned check = 0;
      for (unsigned slot = 0; slot < 3; ++slot)
        check |= current.slots[slot].check << slot;
      runtime->signal("check").set(check);
      runtime->signal("int_xcpt").set(current.trap);
      runtime->signal("cause").set(current.cause);
    });
#endif
    ++cycles;
    if (last_status)
      break;
    unsigned expected_calls = 0;
    for (unsigned slot = 0; slot < 3; ++slot)
      expected_calls += !current.reset && (current.valid >> slot & 1);
    // Check per-frame completion, including frames with no active slots.
    unsigned consumed = 0;
    for (unsigned slot = 0; slot < next_slot; ++slot)
      consumed += current.valid >> slot & 1;
    if (consumed != expected_calls)
      throw std::runtime_error("Dromajo dropped a retirement callback");
  }
  if (!negative && (calls != std::stoul(option("+expected_steps=")) ||
                    cycles != std::stoul(option("+expected_frames="))))
    throw std::runtime_error("Dromajo did not consume the complete trace");
  if (initializations != 1 || traps != 2 || calls < 250 ||
      (negative ? !injected || last_status != 8191 : last_status != 0))
    throw std::runtime_error(
        "real Dromajo result or activation count mismatch");
  std::printf("PASS: real Dromajo %s, %u ordered retirements, %u traps, "
              "%u frames, reset/mask/edge guards and native failure status\n",
              negative ? "mismatch rejected" : "trace accepted", calls, traps,
              cycles);
}
