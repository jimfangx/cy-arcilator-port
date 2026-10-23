// Exercise the unchanged SimBlockDevice.cc/blkdev.cc and real FESVR context.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace chipyard_arc;
namespace {
Runtime *model;
bool initializing;
unsigned initializes, ticks;
std::string disk_path;
std::array<uint64_t, 7> before;
constexpr std::array<const char *, 7> output_names{
    {"bdev_req_ready", "bdev_data_ready", "bdev_resp_valid",
     "bdev_resp_bits_data", "bdev_resp_bits_tag", "bdev_info_nsectors",
     "bdev_info_max_req_len"}};
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::array<uint64_t, 7> outputs() {
  std::array<uint64_t, 7> result;
  for (unsigned i = 0; i < result.size(); ++i)
    result[i] = model->signal(output_names[i]).get();
  return result;
}
void phase() {
  check(model == active_runtime() && svGetScope() &&
            std::string(svGetNameFromScope(svGetScope())) ==
                model->scope_name() + "/disk",
        "block-device callback scope changed");
  if (!initializing)
    check(outputs() == before, "block-device outputs changed before commit");
}
uint64_t initial_word(unsigned i) {
  return 0x8123456700000000ULL ^ (uint64_t(i) * 0x0101010101010101ULL);
}
uint64_t written_word(unsigned tag, unsigned i) {
  return (tag == 1 ? 0xfedcba9800000000ULL : 0x8765432100000000ULL) ^
         (uint64_t(i) * 0x0010203040506071ULL);
}
struct Inputs {
  bool reset = false, request = false, write = true, data = false,
       ready = false;
  uint32_t offset = 0x87654321, length = 0xfedcba98, request_tag = 3,
           data_tag = 2;
  uint64_t word = 0xfedcba9876543210ULL;
};
struct Harness {
  Runtime &runtime;
  uint64_t time = 0;
  void step(Inputs in = {}) {
    auto old = outputs();
    unsigned count = ticks;
    runtime.advance(++time, [&] {
      auto set = [&](const char *name, uint64_t value) {
        runtime.signal(name).set(value);
      };
      set("clock", 0);
      set("reset", in.reset);
      set("bdev_req_valid", in.request);
      set("bdev_req_bits_write", in.write);
      set("bdev_req_bits_offset", in.offset);
      set("bdev_req_bits_len", in.length);
      set("bdev_req_bits_tag", in.request_tag);
      set("bdev_data_valid", in.data);
      set("bdev_data_bits_data", in.word);
      set("bdev_data_bits_tag", in.data_tag);
      set("bdev_resp_ready", in.ready);
    });
    check(ticks == count && outputs() == old,
          "block device changed on falling clock");
    before = outputs();
    runtime.advance(++time, [&] { runtime.signal("clock").set(1); });
    check(ticks == count + unsigned(!in.reset),
          "block-device reset/edge callback count changed");
    auto current = outputs();
    runtime.advance(++time);
    check(ticks == count + unsigned(!in.reset) && outputs() == current,
          "block device repeated a held-clock callback");
    check(!svGetScope(), "block-device callback leaked DPI scope");
  }
  void response(bool valid, uint64_t word = 0, unsigned tag = 0) {
    check(runtime.signal("bdev_resp_valid").get() == valid &&
              runtime.signal("bdev_resp_bits_data").get() == word &&
              runtime.signal("bdev_resp_bits_tag").get() == tag,
          "original block-device response data/tag/backpressure changed");
  }
  void read(unsigned sector, unsigned length, unsigned tag) {
    Inputs in;
    in.request = true;
    in.write = false;
    in.offset = sector;
    in.length = length;
    in.request_tag = tag;
    step(in);
  }
  void write(unsigned sector, unsigned tag) {
    Inputs in;
    in.request = true;
    in.offset = sector;
    in.length = 1;
    in.request_tag = tag;
    step(in);
  }
};
void disabled_initialization() {
  ArcRuntimeModelInfo info{ARC_RUNTIME_API_VERSION, 64, "DisabledBlockInit",
                           nullptr};
  Runtime runtime(0, nullptr, info, +[](void *) {});
  auto binding = make_binding(runtime, "SimBlockDevice");
  uint32_t sectors = 0xdeadbeef, length = 0x87654321;
  binding->ports["bdev_info_nsectors"] =
      Signal{reinterpret_cast<uint8_t *>(&sectors), 32};
  binding->ports["bdev_info_max_req_len"] =
      Signal{reinterpret_cast<uint8_t *>(&length), 32};
  binding->initialize();
  check(sectors == 0, "disabled block-device size output was not initialized");
  // The original else branch assigns nsectors only. Preserve the other bytes.
  check(length == 0x87654321,
        "disabled block-device initialization changed unwritten metadata");
}
} // namespace
extern "C" {
void __real_block_device_init(const char *, int, int *, int *);
void __real_block_device_tick(unsigned char, unsigned char *, unsigned char,
                              int, int, int, unsigned char, unsigned char *,
                              long long, int, unsigned char *, unsigned char,
                              long long *, int *);
void __wrap_block_device_init(const char *filename, int tags, int *sectors,
                              int *length) {
  phase();
  check(initializing && ++initializes == 1 && filename == disk_path &&
            tags == 4,
        "block-device TAG_BITS/path initialization ABI changed");
  __real_block_device_init(filename, tags, sectors, length);
  check(*sectors == 4 && *length == 16, "original disk metadata changed");
  phase();
}
void __wrap_block_device_tick(unsigned char req_valid, unsigned char *req_ready,
                              unsigned char write, int offset, int length,
                              int request_tag, unsigned char data_valid,
                              unsigned char *data_ready, long long data,
                              int data_tag, unsigned char *resp_valid,
                              unsigned char resp_ready,
                              long long *response_data, int *response_tag) {
  phase();
  check(!initializing && !model->signal("reset").get() &&
            model->signal("clock").get(),
        "block-device driver ran during reset or on wrong edge");
  check(req_valid == model->signal("bdev_req_valid").get() &&
            write == model->signal("bdev_req_bits_write").get() &&
            uint32_t(offset) == model->signal("bdev_req_bits_offset").get() &&
            uint32_t(length) == model->signal("bdev_req_bits_len").get() &&
            data_valid == model->signal("bdev_data_valid").get(),
        "block-device request/32-bit offset/length ABI changed");
  check(uint32_t(request_tag) == model->signal("bdev_req_bits_tag").get() &&
            uint32_t(data_tag) == model->signal("bdev_data_bits_tag").get(),
        "block-device tag ABI changed");
  check(uint64_t(data) == model->signal("bdev_data_bits_data").get(),
        "block-device 64-bit data ABI changed");
  check(resp_ready == model->signal("bdev_resp_ready").get(),
        "block-device response-ready ABI changed");
  ++ticks;
  __real_block_device_tick(req_valid, req_ready, write, offset, length,
                           request_tag, data_valid, data_ready, data, data_tag,
                           resp_valid, resp_ready, response_data, response_tag);
  phase();
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected disk path or disabled mode");
    register_builtin_bindings();
    disabled_initialization();
    if (std::string(argv[1]) == "init-only") {
      std::puts(
          "PASS disabled block-device initialization from nonzero storage");
      return 0;
    }
    bool enabled = std::string(argv[1]) != "disabled";
    disk_path = enabled ? argv[1] : "";
    std::string option = "+blkdev=" + disk_path;
    char *args[] = {argv[0], option.data()};
    auto runtime = create_model(enabled ? 2 : 1, args);
    model = runtime.get();
    initializing = true;
    runtime->initialize();
    initializing = false;
    runtime->advance(0, [&] { runtime->signal("reset").set(1); });
    check(initializes == unsigned(enabled),
          "block device initialized without a disk option");
    check(runtime->signal("bdev_info_nsectors").get() == (enabled ? 4 : 0),
          "generated disk size metadata changed");
    if (enabled)
      check(runtime->signal("bdev_info_max_req_len").get() == 16,
            "generated maximum request length changed");
    Harness h{*runtime};
    Inputs reset;
    reset.reset = reset.request = reset.data = reset.ready = true;
    h.step(reset);
    h.response(false);
    h.step();
    if (!enabled) {
      check(!runtime->signal("bdev_req_ready").get() &&
                !runtime->signal("bdev_data_ready").get() &&
                !runtime->signal("bdev_resp_valid").get(),
            "disabled original driver accepted traffic");
      // Its early-return path leaves response data/tag unwritten. Do not
      // invent values for those fields; phase observers retain all seven.
      std::puts(
          "PASS original block device disabled: no init, reset, tick, scope");
      return 0;
    }
    h.response(false);
    check(runtime->signal("bdev_req_ready").get() &&
              runtime->signal("bdev_data_ready").get(),
          "original block device never became ready");
    h.read(0, 1, 3);
    h.response(true, initial_word(0), 3);
    for (unsigned i = 0; i < 3; ++i) {
      h.step();
      h.response(true, initial_word(0), 3);
    }
    h.step(reset);
    h.response(false);
    h.step();
    h.response(true, initial_word(0), 3);
    Inputs consume;
    consume.ready = true;
    for (unsigned i = 0; i < 64; ++i) {
      h.response(true, initial_word(i), 3);
      h.step(consume);
    }
    h.response(false);
    h.write(1, 1);
    h.response(false);
    h.write(2, 2);
    h.response(false);
    for (unsigned i = 0; i < 64; ++i) {
      for (unsigned tag : {1, 2}) {
        Inputs in;
        in.data = true;
        in.data_tag = tag;
        in.word = written_word(tag, i);
        h.step(in);
        if (i < 63)
          h.response(false);
        else
          h.response(true, 0, 1); // Second completion queues behind tag1.
      }
      if (i == 17) {
        h.step(reset);
        h.response(false);
        h.step();
        h.response(false);
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      h.step();
      h.response(true, 0, 1);
    }
    h.step(consume);
    h.response(true, 0, 2);
    h.step();
    h.response(true, 0, 2);
    h.step(consume);
    h.response(false);
    h.read(1, 2, 0); // A two-sector read also flushes the original FILE writes.
    for (unsigned i = 0; i < 128; ++i) {
      h.response(true, written_word(i < 64 ? 1 : 2, i % 64), 0);
      h.step(consume);
    }
    h.response(false);
    check(initializes == 1, "reset reinitialized the original block driver");
    std::puts(
        "PASS original block device: TAG_BITS, 32/64-bit ABI, stalls, tagged "
        "interleaved writes, reset retention, two-sector readback");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
