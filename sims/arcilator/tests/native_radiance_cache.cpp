#include "native/runtime.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
std::unique_ptr<chipyard_arc::Runtime> runtime;
void set(const char *name, uint64_t value) { runtime->signal(name).set(value); }
uint64_t get(const char *name) { return runtime->signal(name).get(); }
using Line = std::array<uint8_t, 32>;
Line memory(uint32_t address) {
  Line bytes{};
  uint32_t state = address ^ 0x7f4a7c15;
  for (auto &byte : bytes) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = state;
  }
  return bytes;
}
void eval(uint64_t cycle, bool high) {
  runtime->advance(cycle * 1000 + (high ? 500 : 0), [&] { set("clk", high); });
}

struct Response {
  unsigned due;
  uint32_t address, tag;
};
int main() {
  runtime = chipyard_arc::create_model(0, nullptr);
  runtime->initialize();
  runtime->signal("arc_printf_enable").set(1);
  runtime->signal("arc_stop_enable").set(1);
  std::deque<Response> responses;
  bool outstanding = false;
  unsigned issued = 0, received = 0, memory_reads = 0;
  uint32_t requested_address = 0, requested_tag = 0;
  for (unsigned cycle = 0; cycle < 20000 && received < 64; ++cycle) {
    bool reset = cycle < 20;
    // During reset the cache pipeline may expose invalid request contents.
    // Only post-reset handshakes are memory transactions.
    if (reset)
      responses.clear();
    bool request = !reset && !outstanding && issued < 64;
    uint32_t address = 0x10000 + ((issued % 16) * 4),
             tag = (issued * 29 + 7) & 511;
    set("reset", reset);
    set("core_req_valid", request);
    set("core_req_rw", 0);
    set("core_req_addr", address);
    set("core_req_tag", tag);
    set("core_req_byteen", 0xffffffff);
    set("core_rsp_ready", 1);
    set("mem_req_ready", 1);
    bool responding = !responses.empty() && responses.front().due <= cycle;
    set("mem_rsp_valid", responding);
    if (responding) {
      auto line = memory(responses.front().address);
      std::memcpy(runtime->signal("mem_rsp_data").data, line.data(),
                  line.size());
      set("mem_rsp_tag", responses.front().tag);
    }
    eval(cycle, false);
    bool accept_request = request && get("core_req_ready");
    bool accept_response = responding && get("mem_rsp_ready");
    bool accept_memory = !reset && get("mem_req_valid");
    bool complete = !reset && get("core_rsp_valid");
    Response next{cycle + 3, unsigned(get("mem_req_addr")),
                  unsigned(get("mem_req_tag"))};
    if (accept_memory && get("mem_req_rw"))
      throw std::runtime_error("unexpected write during read test");
    if (complete) {
      if (!outstanding) {
        std::fprintf(stderr,
                     "unsolicited response cycle=%u issued=%u received=%u "
                     "memory_reads=%u ready=%llu tag=%llu\n",
                     cycle, issued, received, memory_reads,
                     (unsigned long long)get("core_req_ready"),
                     (unsigned long long)get("core_rsp_tag"));
        return 1;
      }
      auto expected = memory(requested_address);
      if (get("core_rsp_tag") != requested_tag ||
          std::memcmp(runtime->signal("core_rsp_data").data, expected.data(),
                      expected.size())) {
        std::fprintf(
            stderr,
            "bad cache response cycle=%u address=%x tag=%u actual_tag=%llu\n",
            cycle, requested_address, requested_tag,
            (unsigned long long)get("core_rsp_tag"));
        return 1;
      }
    }
    eval(cycle, true);
    if (accept_response)
      responses.pop_front();
    if (accept_memory) {
      responses.push_back(next);
      ++memory_reads;
    }
    if (complete) {
      outstanding = false;
      ++received;
    }
    if (accept_request) {
      outstanding = true;
      requested_address = address;
      requested_tag = tag;
      ++issued;
    }
  }
  if (received != 64 || memory_reads != 16) {
    std::fprintf(stderr,
                 "cache stalled or failed hits: issued=%u responses=%u "
                 "memory_reads=%u pending=%zu\n",
                 issued, received, memory_reads, responses.size());
    return 1;
  }
  runtime.reset();
  std::puts("PASS: Radiance cache, 64 reads, 16 misses, 48 hits, complete "
            "256-bit data and 9-bit tags");
  return 0;
}
