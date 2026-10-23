#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  std::array<unsigned, 3> pending{};
  std::array<uint64_t, 3> requests{}, responses{};
  unsigned callbacks = 0, resets = 0, stalls = 0;
  bool finished = false;
  auto lane = [&](const char *name, unsigned index, unsigned bytes) {
    auto value = runtime->signal(name).copy();
    uint64_t result = 0;
    for (unsigned byte = 0; byte < bytes; ++byte)
      result |= uint64_t(value[index * bytes + byte]) << (byte * 8);
    return result;
  };
  const std::array<const char *, 7> outputs{
      "a_valid", "a_address", "a_is_store", "a_size",
      "a_data",  "d_ready",   "finished"};
  for (unsigned cycle = 0; cycle < 502000 && !finished; ++cycle) {
    bool reset = cycle < 3 || cycle % 4096 == 17;
    unsigned ready = 0, valid = 0, total_pending = 0;
    for (unsigned i = 0; i < 3; ++i) {
      ready |= unsigned(pending[i] < 8 && (cycle + i * 3) % 5 != 0) << i;
      valid |= unsigned(pending[i] != 0) << i;
      total_pending += pending[i];
    }
    std::array<std::vector<uint8_t>, 7> before;
    for (unsigned i = 0; i < outputs.size(); ++i)
      before[i] = runtime->signal(outputs[i]).copy();
    runtime->advance(cycle * 1000 + 100, [&] {
      runtime->signal("clock").set(1);
      runtime->signal("reset").set(reset);
      runtime->signal("a_ready").set(ready);
      runtime->signal("d_valid").set(valid);
      runtime->signal("d_size").set(0x020202);
      runtime->signal("inflight").set(total_pending != 0);
    });
    for (unsigned i = 0; i < outputs.size(); ++i)
      if (runtime->signal(outputs[i]).copy() != before[i])
        throw std::runtime_error("fuzzer outputs changed on a positive edge");
    runtime->advance(cycle * 1000 + 600,
                     [&] { runtime->signal("clock").set(0); });
    if (reset) {
      ++resets;
      for (auto name : outputs)
        for (auto byte : runtime->signal(name).copy())
          if (byte)
            throw std::runtime_error("fuzzer reset did not clear outputs");
      continue;
    }
    ++callbacks;
    unsigned a_valid = runtime->signal("a_valid").get();
    unsigned d_ready = runtime->signal("d_ready").get();
    if (runtime->signal("a_is_store").get())
      throw std::runtime_error("read-only fuzzer returned a store");
    for (unsigned i = 0; i < 3; ++i) {
      auto address = lane("a_address", i, 8);
      if (address < 0x80000000ULL || address >= 0x80000040ULL ||
          lane("a_size", i, 1) != 2 || lane("a_data", i, 8) != 0)
        throw std::runtime_error("real fuzzer lane packing or size mismatch");
      if ((valid & d_ready) & (1u << i)) {
        if (!pending[i])
          throw std::runtime_error("fuzzer accepted an unmatched response");
        --pending[i];
        ++responses[i];
      }
      if ((ready & a_valid) & (1u << i)) {
        ++pending[i];
        ++requests[i];
      }
      stalls += ((valid & ~d_ready) | (a_valid & ~ready)) >> i & 1;
    }
    finished = runtime->signal("finished").get();
    if (finished && (a_valid || pending[0] || pending[1] || pending[2]))
      throw std::runtime_error("real fuzzer finished with outstanding traffic");
  }
  if (!finished || callbacks < 500001 || callbacks > 500010 || resets < 100 ||
      !stalls)
    throw std::runtime_error("real fuzzer stop/drain or edge count mismatch");
  for (unsigned i = 0; i < 3; ++i)
    if (requests[i] != responses[i] || requests[i] < 10000)
      throw std::runtime_error("real fuzzer did not balance every active lane");
  std::fprintf(stderr,
               "PASS: real Cyclotron fuzzer, %u callbacks, %u reset edges, "
               "%u stalls, balanced requests/responses %llu/%llu/%llu\n",
               callbacks, resets, stalls,
               static_cast<unsigned long long>(requests[0]),
               static_cast<unsigned long long>(requests[1]),
               static_cast<unsigned long long>(requests[2]));
}
