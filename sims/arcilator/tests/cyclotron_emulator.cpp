#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#ifdef CYCLOTRON_REFERENCE
extern "C" void emulator_init(int);
extern "C" void emulator_tick(uint8_t *, uint8_t *, long long *, uint8_t *,
                              int *, long long *, uint8_t *, uint8_t *,
                              uint8_t *, int *, long long *, uint8_t,
                              uint8_t *);
#else
#include "native/runtime.h"
#endif

int main(int argc, char **argv) {
  if (argc != 2)
    throw std::runtime_error("expected emulator reference vector file");
  std::array<uint8_t, 32> ready{}, valid{}, store{}, dready{}, dvalid{},
      dstore{};
  std::array<int, 32> size{}, dsize{};
  std::array<long long, 32> address{}, data{}, ddata{};
  uint8_t finished = 0;
#ifdef CYCLOTRON_REFERENCE
  emulator_init(3);
  std::ofstream vectors(argv[1]);
#else
  auto runtime = chipyard_arc::create_model(argc, argv);
  runtime->initialize();
  std::ifstream vectors(argv[1]);
  auto lane = [&](const char *name, unsigned index, unsigned bytes) {
    auto bits = runtime->signal(name).copy();
    uint64_t result = 0;
    for (unsigned i = 0; i < bytes; ++i)
      result |= uint64_t(bits[index * bytes + i]) << (i * 8);
    return result;
  };
  const std::array<const char *, 7> outputs{
      "a_valid", "a_address", "a_is_store", "a_size",
      "a_data",  "d_ready",   "finished"};
  unsigned observations = 0;
#endif
  if (!vectors)
    throw std::runtime_error("cannot open emulator reference vectors");
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    bool reset = cycle < 3 || cycle % 31 == 0;
    for (unsigned i = 0; i < 3; ++i) {
      ready[i] = (cycle + i) % 5 != 0;
      dvalid[i] = cycle > 3 && i == 0 && cycle % 3 == 0;
      dsize[i] = 2;
      ddata[i] = 0x0000001300000013LL; // Two RISC-V NOP instructions.
    }
#ifdef CYCLOTRON_REFERENCE
    if (reset) {
      valid.fill(0);
      address.fill(0);
      store.fill(0);
      size.fill(0);
      data.fill(0);
      dready.fill(0);
      finished = 0;
    } else {
      emulator_tick(ready.data(), valid.data(), address.data(), store.data(),
                    size.data(), data.data(), dready.data(), dvalid.data(),
                    dstore.data(), dsize.data(), ddata.data(), 0, &finished);
    }
    for (unsigned i = 0; i < 3; ++i)
      vectors << unsigned(valid[i]) << ' ' << uint64_t(address[i]) << ' '
              << unsigned(store[i]) << ' ' << unsigned(size[i]) << ' '
              << uint64_t(data[i]) << ' ' << unsigned(dready[i]) << ' ';
    vectors << unsigned(finished) << '\n';
#else
    std::array<std::vector<uint8_t>, 7> before;
    for (unsigned i = 0; i < outputs.size(); ++i)
      before[i] = runtime->signal(outputs[i]).copy();
    runtime->advance(cycle * 1000 + 100, [&] {
      runtime->signal("clock").set(0);
      runtime->signal("reset").set(reset);
      runtime->signal("a_ready").set(ready[0] | ready[1] << 1 | ready[2] << 2);
      runtime->signal("d_valid").set(dvalid[0]);
      runtime->signal("d_size").set(0x020202);
      std::vector<uint8_t> bytes(24);
      for (unsigned i = 0; i < 24; ++i)
        bytes[i] = uint64_t(ddata[i / 8]) >> (i % 8 * 8);
      runtime->signal("d_data").assign(bytes);
      runtime->signal("inflight").set(0);
    });
    for (unsigned i = 0; i < outputs.size(); ++i)
      if (runtime->signal(outputs[i]).copy() != before[i])
        throw std::runtime_error("emulator outputs changed on a negative edge");
    runtime->advance(cycle * 1000 + 600,
                     [&] { runtime->signal("clock").set(1); });
    auto check = [&](uint64_t actual) {
      uint64_t expected;
      if (!(vectors >> expected) || actual != expected)
        throw std::runtime_error(
            "native emulator differs from original driver");
      ++observations;
    };
    for (unsigned i = 0; i < 3; ++i) {
      check(runtime->signal("a_valid").get() >> i & 1);
      check(lane("a_address", i, 8));
      check(runtime->signal("a_is_store").get() >> i & 1);
      check(lane("a_size", i, 1));
      check(lane("a_data", i, 8));
      check(runtime->signal("d_ready").get() >> i & 1);
    }
    check(runtime->signal("finished").get());
#endif
  }
#ifndef CYCLOTRON_REFERENCE
  uint64_t extra;
  if (vectors >> extra)
    throw std::runtime_error("unused emulator reference vectors");
  std::fprintf(stderr,
               "PASS: %u real emulator output checks against original driver, "
               "512 cycles, 3 packed lanes, positive edges and reset guards\n",
               observations);
#endif
}
