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

int main() {
  for (unsigned initial : {0u, 1u, 2u, 255u}) {
    for (auto &v : calls)
      v.clear();
    onceCalls = 0;
    auto *m =
        arcRuntimeAllocateInstance(&arcRuntimeModel_SensitizedHarness, nullptr);
    arcRuntimeOnInitialized(m);
    uint32_t rng = 0x12345678;
    for (unsigned step = 0; step < 200; ++step) {
      unsigned phase = step < 4 ? initial : (step % 7 < 4);
      m->modelState[arc_samplingOffset] = phase;
      if (step % 3 == 0 || step < 6) {
        for (auto p : phasePorts) {
          if (!p.input || p.offset == arc_samplingOffset)
            continue;
          for (unsigned byte = 0; byte < (p.bits + 7) / 8; ++byte) {
            rng = rng * 1664525 + 1013904223;
            m->modelState[p.offset + byte] = rng >> 24;
          }
          if (p.bits % 8)
            m->modelState[p.offset + p.bits / 8] &= (1u << (p.bits % 8)) - 1;
        }
      }
      arcRuntimeOnEval(m);
      SensitizedHarness_eval(m->modelState);
      std::printf("%u %u %u", initial, step, onceCalls);
      for (auto p : phasePorts)
        if (!p.input) {
          std::printf(" %s=", p.name);
          for (unsigned i = 0; i < (p.bits + 7) / 8; ++i)
            std::printf("%02x", m->modelState[p.offset + i] &
                                    (i == p.bits / 8 && p.bits % 8
                                         ? (1u << (p.bits % 8)) - 1
                                         : 255));
        }
      for (unsigned site = 0; site < calls.size(); ++site) {
        std::printf(" c%u=", site);
        for (auto value : calls[site])
          std::printf("%08x,", value);
      }
      std::printf("\n");
    }
    arcRuntimeDeleteInstance(m);
  }
}