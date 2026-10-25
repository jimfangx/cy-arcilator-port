// Original Radiance trace reader/logger with generated native model bindings.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
using namespace chipyard_arc;
namespace {
Runtime *model;
bool initializing, with_source;
unsigned reader_inits, logger_inits, queries, logs;
uint64_t logger_cycle;
std::vector<uint8_t> before;
constexpr std::array<const char *, 7> names{
    {"trace_valid", "trace_address", "trace_is_store", "trace_size",
     "trace_data", "trace_finished", "log_ready"}};
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::vector<uint8_t> outputs() {
  std::vector<uint8_t> result;
  for (auto name : names) {
    auto value = model->signal(name).copy();
    result.insert(result.end(), value.begin(), value.end());
  }
  return result;
}
void phase(const char *instance) {
  check(model == active_runtime() && svGetScope() &&
            std::string(svGetNameFromScope(svGetScope())) ==
                model->scope_name() + "/" + instance,
        "memory trace callback scope changed");
  if (!initializing)
    check(outputs() == before,
          "memory trace outputs changed before deferred commit");
}
uint64_t lane(const char *name, unsigned index, unsigned bits) {
  auto value = model->signal(name).copy();
  uint64_t result = 0;
  for (unsigned bit = 0; bit < bits; ++bit)
    result |=
        uint64_t(
            (value[(index * bits + bit) / 8] >> ((index * bits + bit) % 8)) & 1)
        << bit;
  return result;
}
void words(const char *name, std::array<uint64_t, 3> data, unsigned bytes) {
  std::vector<uint8_t> value;
  for (auto word : data)
    for (unsigned i = 0; i < bytes; ++i)
      value.push_back(word >> (i * 8));
  model->signal(name).assign(value);
}
std::vector<uint8_t> decode(const std::string &text) {
  check(text.size() == 110, "memory trace oracle byte count changed");
  std::vector<uint8_t> result;
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    check(c >= 'a' && c <= 'f', "invalid memory trace oracle hex digit");
    return c - 'a' + 10;
  };
  for (unsigned i = 0; i < text.size(); i += 2)
    result.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
  return result;
}
} // namespace
extern "C" {
void __real_memtrace_init(const char *, bool);
void __real_memtrace_query(unsigned char, unsigned long, int, unsigned char *,
                           unsigned long *, unsigned char *, unsigned char *,
                           unsigned long *, unsigned char *);
int __real_memtracelogger_init(int, const char *, const char *);
void __real_memtracelogger_log(int, unsigned char, unsigned long, int, int,
                               unsigned long, unsigned char, int, unsigned long,
                               unsigned char *);
void __wrap_memtrace_init(const char *filename, bool has_source) {
  phase("reader");
  check(initializing && ++reader_inits == 1 && filename &&
            std::string(filename) == "unused-reader",
        "memory reader initialization ABI changed");
  __real_memtrace_init(filename, has_source);
  check(has_source == with_source, "one-bit HAS_SOURCE ABI changed");
  phase("reader");
}
int __wrap_memtracelogger_init(int response, const char *filename,
                               const char *suffix) {
  phase("logger");
  check(initializing && ++logger_inits == 1 && filename && suffix &&
            std::string(filename) == "unused-logger" &&
            std::string(suffix) == ".native-log",
        "memory logger initialization ABI changed");
  int handle = __real_memtracelogger_init(response, filename, suffix);
  check(response == int(!with_source), "one-bit IS_RESPONSE ABI changed");
  check(handle == 0, "original logger handle changed");
  phase("logger");
  return handle;
}
void __wrap_memtrace_query(unsigned char ready, unsigned long cycle, int index,
                           unsigned char *valid, unsigned long *address,
                           unsigned char *store, unsigned char *size,
                           unsigned long *data, unsigned char *finished) {
  phase("reader");
  check(!initializing && model->signal("clock").get() &&
            !model->signal("reset").get() &&
            model->signal("trace_ready").get() && ready,
        "reader called while stalled/reset or wrong edge");
  check(cycle == model->signal("trace_cycle").get() &&
            index == int(queries % 3),
        "original memory trace lane/cycle order changed");
  ++queries;
  __real_memtrace_query(ready, cycle, index, valid, address, store, size, data,
                        finished);
  phase("reader");
}
void __wrap_memtracelogger_log(int handle, unsigned char valid,
                               unsigned long cycle, int index, int source,
                               unsigned long address, unsigned char store,
                               int size, unsigned long data,
                               unsigned char *ready) {
  phase("logger");
  check(!initializing && model->signal("clock").get() &&
            !model->signal("reset").get(),
        "logger called during reset or wrong edge");
  check(handle == 0 && index == int(logs % 3) && cycle == logger_cycle &&
            valid == ((model->signal("log_valid").get() >> index) & 1) &&
            uint32_t(source) == lane("log_source", index, 32) &&
            address == lane("log_address", index, 64) &&
            store == ((model->signal("log_is_store").get() >> index) & 1) &&
            size == int(lane("log_size", index, 8)) &&
            data == lane("log_data", index, 64),
        "original memory logger packed-lane/cycle ABI changed");
  ++logs;
  __real_memtracelogger_log(handle, valid, cycle, index, source, address, store,
                            size, data, ready);
  phase("logger");
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 5,
          "expected source mode, original trace, oracle and output files");
    with_source = std::string(argv[1]) == "with-source";
    check(with_source || std::string(argv[1]) == "without-source",
          "unknown source-column mode");
    char name[] = "memory-trace-model-test";
    std::string option = "+memtracefile=" + std::string(argv[2]);
    char *args[] = {name, option.data()};
    auto runtime = create_model(2, args);
    model = runtime.get();
    initializing = true;
    runtime->initialize();
    initializing = false;
    runtime->advance(0, [&] { runtime->signal("reset").set(1); });
    std::ifstream oracle(argv[3]);
    FILE *file = std::fopen(argv[4], "wb");
    check(oracle && file, "cannot open owned memory trace oracle/output");
    uint64_t time = 0, cycle;
    unsigned ready, reset, valid, step = 0;
    std::string expected;
    while (oracle >> cycle >> ready >> reset >> logger_cycle >> valid >>
           expected) {
      auto old = outputs();
      unsigned q = queries, l = logs;
      runtime->advance(++time, [&] {
        runtime->signal("clock").set(0);
        runtime->signal("reset").set(reset);
        runtime->signal("trace_cycle").set(cycle);
        runtime->signal("trace_ready").set(ready);
        runtime->signal("log_valid").set(valid);
        words("log_source", {33, uint32_t(-7), 0x12345678}, 4);
        std::array<uint64_t, 3> address, data, size;
        unsigned store = 0;
        for (unsigned i = 0; i < 3; ++i) {
          address[i] = 0x8000000000010000ULL | (uint64_t(step) << 8) | i;
          data[i] = 0xfedcba9876500000ULL ^ (uint64_t(step) * 0x10001) ^
                    (uint64_t(i) * 0x12345);
          unsigned sizes[] = {3, 0, 7};
          size[i] = ((valid >> i) & 1) ? sizes[i] : 0x80 + i;
          store |= ((step + i) & 1) << i;
        }
        words("log_address", address, 8);
        words("log_data", data, 8);
        words("log_size", size, 1);
        runtime->signal("log_is_store").set(store);
      });
      check(queries == q && logs == l && outputs() == old,
            "memory trace changed on falling/setup phase");
      before = outputs();
      runtime->advance(++time, [&] { runtime->signal("clock").set(1); });
      check(queries == q + ((!reset && ready) ? 3 : 0) &&
                logs == l + (!reset ? 3 : 0),
            "memory trace per-edge callback counts changed");
      auto value = outputs();
      check(value == decode(expected),
            "memory trace outputs or hold/reset changed");
      check(std::fwrite(value.data(), value.size(), 1, file) == 1,
            "memory trace output write failed");
      q = queries;
      l = logs;
      runtime->advance(++time);
      check(queries == q && logs == l && outputs() == value && !svGetScope(),
            "memory trace repeated held-clock callback or leaked scope");
      ++step;
    }
    check(oracle.eof() && step == 12 && reader_inits == 1 &&
              logger_inits == 1 && queries == 18 && logs == 30,
          "memory trace complete scenario counts changed");
    check(!std::fclose(file), "memory trace output close failed");
    std::fflush(nullptr);
    std::printf(
        "PASS original memory trace: mode=%s steps=12 queries=18 logs=30\n",
        argv[1]);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
