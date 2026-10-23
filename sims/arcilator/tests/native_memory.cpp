#include "native/runtime.h"
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
  auto model = chipyard_arc::create_model(argc, argv);
  std::array<std::array<unsigned, 9>, 2> memory{};
  std::array<unsigned, 2> initial{410, 277}, before{}, after{}, peer{};
  struct Entry {
    uint64_t address = 0;
    bool valid = false;
  };
  std::array<std::array<Entry, 4>, 2> stack{};
  bool reset = false, enable = false;
  auto reset_memory = [&](unsigned n) {
    for (unsigned i = 0; i < 9; ++i)
      memory[n][i] = (i * 33) ^ initial[n];
  };
  reset_memory(0);
  reset_memory(1);
  auto read = [&](unsigned n, unsigned address) {
    return address < 9 ? memory[n][address] : 0;
  };
  unsigned read_address = 0, address = 0, data = 0, op = 0, mask = 0;
  auto next_stack = [&](unsigned n) {
    auto result = stack[n];
    unsigned operation = op ^ n;
    Entry entry{(uint64_t(initial[n]) << 32) | (data ^ (n ? 8191 : 0)), true};
    if (enable) {
      if (operation == 1) {
        for (unsigned i = 3; i > 0; --i)
          result[i] = stack[n][i - 1];
        result[0] = entry;
      } else if (operation == 2) {
        for (unsigned i = 0; i < 3; ++i)
          result[i] = stack[n][i + 1];
        result[3] = {};
      } else if (operation == 3)
        result[0] = entry;
    }
    return result;
  };
  auto packed_stack = [&](const std::array<Entry, 4> &entries) {
    std::vector<uint8_t> bytes(33);
    for (unsigned i = 0; i < 4; ++i) {
      for (unsigned j = 0; j < 64; ++j)
        if ((entries[i].address >> j) & 1)
          bytes[(i * 65 + j) / 8] |= uint8_t(1u << ((i * 65 + j) % 8));
      if (entries[i].valid)
        bytes[(i * 65 + 64) / 8] |= uint8_t(1u << ((i * 65 + 64) % 8));
    }
    return bytes;
  };
  uint64_t rng = 0xa7351ef6d81c462bULL;
  auto random = [&] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return unsigned(rng);
  };
  unsigned checks = 0;
  auto check = [&] {
    for (unsigned n = 0; n < 2; ++n) {
      std::string suffix = n ? "b" : "a";
      auto equal = [&](const std::string &name, unsigned expected) {
        auto actual = model->signal(name).get();
        if (actual != expected) {
          std::fprintf(stderr,
                       "%s: actual %x expected %x address %u read %u op %u\n",
                       name.c_str(), unsigned(actual), expected, address,
                       read_address, op);
          throw std::runtime_error("native mixed-assignment memory mismatch");
        }
        ++checks;
      };
      equal(suffix, read(n, read_address));
      equal("before_" + suffix, before[n]);
      equal("after_" + suffix, after[n]);
      equal("peer_" + suffix, peer[n]);
      if (model->signal("stack_" + suffix).copy() != packed_stack(stack[n]) ||
          model->signal("next_stack_" + suffix).copy() !=
              packed_stack(next_stack(n)))
        throw std::runtime_error(
            "native 260-bit packed-struct array-slice memory mismatch");
      checks += 2;
    }
  };
  model->initialize();
  model->advance(0);
  check();
  for (unsigned cycle = 0; cycle < 512; ++cycle) {
    address = random() & 31;
    data = random() & 8191;
    op = random() & 3;
    mask = random() & 3;
    reset = cycle == 0 || cycle == 127 || cycle == 301;
    enable = cycle % 11 != 0;
    model->advance(cycle * 100000 + 1, [&] {
      model->signal("clock").set(0);
      model->signal("reset").set(reset);
      model->signal("enable").set(enable);
      model->signal("op").set(op);
      model->signal("address").set(address);
      model->signal("data").set(data);
      model->signal("mask").set(mask);
    });
    check();
    for (unsigned n = 0; n < 2; ++n) {
      stack[n] = reset ? std::array<Entry, 4>{} : next_stack(n);
      peer[n] = read(n, read_address);
      unsigned a = (address + (n ? 3 : 0)) & 31;
      unsigned d = data ^ (n ? 8191 : 0), operation = op ^ n;
      if (reset) {
        reset_memory(n);
        before[n] = after[n] = 0;
      } else if (enable) {
        before[n] = after[n] = read(n, a);
        if (a < 9) {
          unsigned old = memory[n][a];
          if (operation == 0)
            memory[n][a] = d;
          else if (operation == 1) {
            unsigned m = (mask & 1 ? 255 : 0) | (mask & 2 ? 7936 : 0);
            memory[n][a] = (old & ~m) | (d & m);
          } else if (operation == 2) {
            after[n] = (old & ~240u) | (d & 240);
            memory[n][a] = (old & ~255u) | (d & 255);
          } else {
            after[n] = (old & 255) | ((~d) & 7936);
            memory[n][a] = d ^ 255;
          }
        }
      }
    }
    model->advance(cycle * 100000 + 50000,
                   [&] { model->signal("clock").set(1); });
    check();
    for (unsigned i = 0; i < 32; ++i) {
      read_address = i;
      model->advance(cycle * 100000 + 60000 + i,
                     [&] { model->signal("read_address").set(read_address); });
      check();
    }
  }
  std::printf(
      "PASS: %u native memory checks, initialization, two instances, mixed "
      "blocking/NBA order, overlapping partial writes, masked writes, "
      "read-during-write, invalid addresses, peer sampling and 260-bit struct "
      "array slices\n",
      checks);
}
