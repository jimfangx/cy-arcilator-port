#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "sensitized_dpi_layout.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef ARC_NATIVE_SENSITIVITY
#include "native/runtime.h"
#endif

#ifndef ARC_NATIVE_SENSITIVITY
extern "C" void SensitizedHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_SensitizedHarness;
#endif
static std::array<std::vector<uint32_t>, 55> calls;
static unsigned onceCalls = 0;
extern "C" uint32_t chipyard_sensitized_record(uint32_t site, uint32_t data) {
  calls.at(site).push_back(data);
  return data ^ (site * 17u) ^ (uint32_t(calls[site].size()) * 0x9e3779b9u);
}
extern "C" void chipyard_sensitized_once() { ++onceCalls; }
extern "C" bool chipyard_sensitized_mixed(bool mode, uint32_t data,
                                          uint32_t *accumulator, uint32_t *low,
                                          bool *flag) {
  calls[30].push_back((uint32_t(mode) << 31) ^ data ^ *accumulator);
  *accumulator += data ^ (uint32_t(calls[30].size()) * 0x137u);
  *low = *accumulator ^ 0x5a5a5a5au;
  *flag = mode ^ (*low & 1);
  return (*accumulator >> 31) != 0;
}

int main(int argc, char **argv) {
#ifdef ARC_NATIVE_SENSITIVITY
  auto model = chipyard_arc::create_model(argc, argv);
  model->initialize();
  uint64_t time = 0;
  auto address = [&](unsigned offset) -> uint8_t * {
    for (auto port : dpiPorts)
      if (offset >= port.offset && offset < port.offset + port.bytes)
        return model->signal(port.name).data + offset - port.offset;
    throw std::runtime_error("DPI fixture offset is not a public port");
  };
#else
  auto *m =
      arcRuntimeAllocateInstance(&arcRuntimeModel_SensitizedHarness, nullptr);
  arcRuntimeOnInitialized(m);
  auto address = [&](unsigned offset) { return m->modelState + offset; };
#endif
  std::array<std::vector<uint32_t>, 55> expectedCalls;
  auto record = [&](unsigned site, uint32_t data) {
    expectedCalls[site].push_back(data);
    return data ^ (site * 17u) ^
           (uint32_t(expectedCalls[site].size()) * 0x9e3779b9u);
  };
  struct Inputs {
    uint32_t a = 0, b = 0;
    uint64_t aggregateLow = 0, arrayBits = 0, floatBits = 0;
    uint8_t aggregateHigh = 0;
    bool clock = false, reset = false, enable = false, select = false;
    uint8_t noise = 0, unrelated = 0;
  } input, previous;
  uint32_t bare = 0, gated = 0, comb = 0, mixedAcc = 0, mixedLow = 0;
  uint32_t constant = 0, clocked = 0, procedural = 0, proceduralGate = 0;
  uint32_t sampled = 0, bareClone = 0;
  bool mixedFlag = false, mixedValid = false, started = false;
  uint32_t aggregateValue = 0, arrayValue = 0, floatValue = 0, clockValue = 0;
  auto set = [&](unsigned offset, uint32_t value, unsigned bytes) {
    std::memcpy(address(offset), &value, bytes);
  };
  auto read = [&](unsigned offset, unsigned bytes) {
    uint32_t value = 0;
    std::memcpy(&value, address(offset), bytes);
    return value;
  };
  unsigned evaluations = 0, expectedOnce = 2;
  auto set64 = [&](unsigned offset, uint64_t value) {
    std::memcpy(address(offset), &value, 8);
  };
  auto read64 = [&](unsigned offset) {
    uint64_t value;
    std::memcpy(&value, address(offset), 8);
    return value;
  };
  auto eval = [&] {
    bool aChanged = !started || input.a != previous.a;
    if ((!started || input.select != previous.select) && input.select)
      ++expectedOnce;
    if (!started)
      constant = record(40, 123);
    if (aChanged) {
      bare = record(0, input.a);
      bareClone = record(0, input.a);
    }
    if (aChanged || input.enable != previous.enable)
      gated = input.enable ? record(1, input.a) : 0;
    if (aChanged || input.select != previous.select ||
        input.noise != previous.noise) {
      uint32_t value;
      if (input.select) {
        record(10, input.a);
        uint32_t first = record(10, input.a);
        value = first ^ record(11, first);
      } else {
        value = record(12, input.a);
      }
      comb = value ^ input.noise;
    }
    if (aChanged || input.b != previous.b || input.select != previous.select) {
      expectedCalls[30].push_back((uint32_t(input.select) << 31) ^ input.a ^
                                  input.b);
      mixedAcc =
          input.b + (input.a ^ (uint32_t(expectedCalls[30].size()) * 0x137u));
      mixedLow = mixedAcc ^ 0x5a5a5a5au;
      mixedFlag = input.select ^ (mixedLow & 1);
      mixedValid = (mixedAcc >> 31) != 0;
    }
    if (started && !previous.clock && input.clock) {
      sampled = input.reset ? 0 : comb;
      proceduralGate = input.enable ? record(22, input.a) : 0;
      if (input.enable)
        clocked = record(21, input.a);
      if (input.reset)
        procedural = 0;
      else if (input.enable)
        procedural = record(20, input.a);
    }
    if (!started || input.aggregateLow != previous.aggregateLow ||
        input.aggregateHigh != previous.aggregateHigh)
      aggregateValue = record(50, uint32_t(input.aggregateLow >> 33) |
                                      (uint32_t(input.aggregateHigh) << 31));
    if (!started || input.arrayBits != previous.arrayBits)
      arrayValue = record(51, uint32_t(input.arrayBits));
    if (!started || input.noise != previous.noise)
      record(52, input.noise);
    if (!started || input.floatBits != previous.floatBits)
      floatValue = record(53, uint32_t(input.floatBits));
    if (!started || input.clock != previous.clock)
      clockValue = record(54, input.clock);
    set(clockOffset, input.clock, 1);
    set(resetOffset, input.reset, 1);
    set(enableOffset, input.enable, 1);
    set(selectOffset, input.select, 1);
    set(aOffset, input.a, 4);
    set(bOffset, input.b, 4);
    set(noiseOffset, input.noise, 1);
    set(unrelatedOffset, input.unrelated, 1);
    set64(aggregate_bitsOffset, input.aggregateLow);
    set(aggregate_bitsOffset + 8,
        input.aggregateHigh | ((evaluations & 1) ? 0xfe : 0), 1);
    set64(array_bitsOffset, input.arrayBits);
    set64(float_bitsOffset, input.floatBits);
#ifdef ARC_NATIVE_SENSITIVITY
    model->advance(time++);
#else
    arcRuntimeOnEval(m);
    SensitizedHarness_eval(m->modelState);
#endif
    ++evaluations;
    if (calls != expectedCalls || onceCalls != expectedOnce ||
        !((read(bareOffset, 4) == bare &&
           read(bare_cloneOffset, 4) == bareClone) ||
          (read(bareOffset, 4) == bareClone &&
           read(bare_cloneOffset, 4) == bare)) ||
        read(gatedOffset, 4) != gated || read(combOffset, 4) != comb ||
        read(mixed_accOffset, 4) != mixedAcc ||
        read(mixed_lowOffset, 4) != mixedLow ||
        bool(read(mixed_flagOffset, 1)) != mixedFlag ||
        bool(read(mixed_validOffset, 1)) != mixedValid ||
        read(constantOffset, 4) != constant ||
        read(clockedOffset, 4) != clocked ||
        read(proceduralOffset, 4) != procedural ||
        read(passthroughOffset, 1) != input.unrelated ||
        read64(aggregateOffset) !=
            ((input.aggregateLow & ((UINT64_C(1) << 33) - 1)) |
             (uint64_t(aggregateValue) << 33)) ||
        (read(aggregateOffset + 8, 1) & 1) != (aggregateValue >> 31) ||
        read64(arrayOffset) !=
            ((input.arrayBits & UINT64_C(0xffffffff00000000)) | arrayValue) ||
        read(float_valueOffset, 4) != floatValue ||
        read(sensitive_clockOffset, 4) != clockValue ||
        read(procedural_gateOffset, 4) != proceduralGate ||
        read(sampled_combOffset, 4) != sampled) {
      std::fprintf(stderr,
                   "DPI mismatch at eval %u; bare %x/%x gated %x/%x comb %x/%x "
                   "clocked %x/%x procedural %x/%x once %u\n",
                   evaluations, read(bareOffset, 4), bare, read(gatedOffset, 4),
                   gated, read(combOffset, 4), comb, read(clockedOffset, 4),
                   clocked, read(proceduralOffset, 4), procedural, onceCalls);
      for (unsigned i = 0; i < calls.size(); ++i)
        if (calls[i] != expectedCalls[i])
          std::fprintf(stderr, "site %u callbacks %zu/%zu\n", i,
                       calls[i].size(), expectedCalls[i].size());
      throw std::runtime_error("combinational/procedural DPI activation, "
                               "values, ABI or call order changed");
    }
    previous = input;
    started = true;
  };
  eval();
  eval();
  for (unsigned cycle = 0; cycle < 256; ++cycle) {
    input.clock = false;
    input.reset = cycle % 17 == 0;
    input.enable = cycle % 3 != 1;
    input.a = 0x12345678u ^ ((cycle / 3) * 0x1001001u);
    input.b = 0x87654321u ^ ((cycle / 5) * 0x01234567u);
    input.select = cycle % 4 < 2;
    input.noise = uint8_t(cycle / 7);
    input.aggregateLow = (uint64_t(cycle / 3) << 33) | (cycle & 1);
    input.aggregateHigh = (cycle / 13) & 1;
    input.arrayBits = (uint64_t(cycle) << 32) | (cycle / 11);
    const uint64_t fp[] = {0,
                           UINT64_C(0x8000000000000000),
                           UINT64_C(0x7ff8000000000001),
                           UINT64_C(0xfff8000000000001),
                           UINT64_C(0x7ff0000000000000),
                           1,
                           UINT64_C(0x7fefffffffffffff),
                           UINT64_C(0x3ff0000000000000)};
    input.floatBits = fp[(cycle / 4) % 8];
    eval();
    eval();
    input.unrelated ^= 0xa5;
    eval();
    input.clock = true;
    eval();
    eval();
    input.noise ^= 0x5a;
    eval();
    eval();
    input.a ^= cycle & 1;
    input.enable = !input.enable;
    eval();
    eval();
    input.clock = false;
    eval();
  }
#ifndef ARC_NATIVE_SENSITIVITY
  arcRuntimeDeleteInstance(m);
#endif
  std::printf(
      "PASS: %u DPI evaluations, stable-input/unrelated-input guards, "
      "full region sensitivity, identical calls and alias order, "
      "branch effects, enable/X-to-zero, Boolean/inout/output ABI, "
      "aggregate snapshots/results, NaN/signed-zero/clock observations, "
      "once-only calls and preserved clocked activations\n",
      evaluations);
}
