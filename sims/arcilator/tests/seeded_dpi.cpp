#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "seeded_dpi_layout.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#ifdef ARC_NATIVE_SEEDED
#include "native/runtime.h"
#else
extern "C" void SeededDPIHarness_eval(void *);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_SeededDPIHarness;
#endif

static unsigned calls = 0;
extern "C" uint64_t chipyard_seeded_partial(uint32_t mode, uint32_t *low,
                                            uint32_t *acc, uint32_t data,
                                            bool *flag, uint32_t *storage,
                                            uint32_t *wide) {
  ++calls;
  if (*low || *flag || wide[0] || wide[1] || wide[2] || wide[3] ||
      (wide[4] & 1))
    throw std::runtime_error(
        "DPI output buffer was not initialized on this call");
  if (*storage != data)
    throw std::runtime_error("DPI ref buffer lost its original contents");
  auto initial = *acc;
  if (mode & 1)
    *low = data ^ 0xdeadbeefu;
  if (mode & 2)
    *acc += data;
  if (mode & 4)
    *flag = true;
  // Write only selected words, leaving the remaining declared bits unwritten.
  if (mode & 8) {
    wide[1] = data;
    if (mode & 32)
      wide[4] = 1;
  }
  *storage = data ^ 0x13579bdfu;
  return (uint64_t(mode) << 32) | (data ^ initial);
}

int main(int argc, char **argv) {
#ifdef ARC_NATIVE_SEEDED
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
  auto *instance =
      arcRuntimeAllocateInstance(&arcRuntimeModel_SeededDPIHarness, nullptr);
  arcRuntimeOnInitialized(instance);
  auto address = [&](unsigned offset) { return instance->modelState + offset; };
#endif
  auto set = [&](unsigned offset, uint32_t value) {
    std::memcpy(address(offset), &value, 4);
  };
  auto read = [&](unsigned offset, unsigned bytes) {
    uint64_t value = 0;
    std::memcpy(&value, address(offset), bytes);
    return value;
  };
  unsigned expectedCalls = 0, evaluations = 0;
  auto eval = [&](uint32_t mode, uint32_t data, uint32_t seed, bool active) {
    set(modeOffset, mode);
    set(dataOffset, data);
    set(seedOffset, seed);
    if (active)
      ++expectedCalls;
    ++evaluations;
#ifdef ARC_NATIVE_SEEDED
    model->advance(time++);
#else
    arcRuntimeOnEval(instance);
    SeededDPIHarness_eval(instance->modelState);
#endif
    std::array<uint8_t, 17> wide{};
    if (mode & 8) {
      std::memcpy(wide.data() + 4, &data, 4);
      if (mode & 32)
        wide[16] = 1;
    }
    if (calls != expectedCalls ||
        read(lowOffset, 4) != ((mode & 1) ? data ^ 0xdeadbeefu : 0) ||
        read(accOffset, 4) != uint32_t(seed + ((mode & 2) ? data : 0)) ||
        (read(flagOffset, 1) & 1) != bool(mode & 4) ||
        std::memcmp(address(wideOffset), wide.data(), 16) ||
        ((address(wideOffset)[16] ^ wide[16]) & 1) ||
        read(resultOffset, 8) != ((uint64_t(mode) << 32) | (data ^ seed)) ||
        read(storageOffset, 4) != (data ^ 0x13579bdfu))
      throw std::runtime_error(
          "DPI partial outputs, inout/ref, ABI or activation mismatch");
  };
  eval(0, 0, 0, true);
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    uint32_t mode = (cycle * 29) % 64;
    uint32_t data = 0xa5a50001u ^ (cycle * 0x13579bdu);
    uint32_t seed = 0x5a5a0007u + cycle * 0x112233u;
    eval(mode, data, seed, true);
    eval(mode, data, seed, false);
    set(unrelatedOffset, ~cycle);
    eval(mode, data, seed, false);
    seed ^= 0x80000001u;
    eval(mode, data, seed, true);
  }
#ifndef ARC_NATIVE_SEEDED
  arcRuntimeDeleteInstance(instance);
#endif
  std::printf(
      "PASS: %u partial-output DPI evaluations, per-call zero seeds, "
      "129-bit word writes, Boolean outputs, inout copy-in/out, ref "
      "identity, interleaved formals, aliases and stable-input guards\n",
      evaluations);
}
