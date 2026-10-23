// Exercise native mixed-edge bindings with the original debug_rob.cc driver.
#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace chipyard_arc;
namespace {
constexpr uint32_t hart_a = 0x80000011, hart_b = 0x12345678;
constexpr uint64_t tag_a = 0xfedcba9800000022ULL, tag_b = 0x8123456700000022ULL,
                   tag_c = 0xaabbccdd00000033ULL, tag_d = 0x9988776600000044ULL;
Runtime *model;
std::array<unsigned, 3> calls{};
constexpr std::array<const char *, 9> names{
    {"out_valid", "out_iaddr", "out_insn", "out_priv", "out_exception",
     "out_interrupt", "out_cause", "out_tval", "out_wdata"}};
using Snapshot = std::array<std::vector<uint8_t>, names.size()>;
Snapshot before;
struct RobTrace {
  uint8_t valid = 0, priv = 0, exception = 0, interrupt = 0;
  uint64_t pc = 0, insn = 0, cause = 0, tval = 0;
  std::array<uint64_t, 8> data{};
};
std::optional<RobTrace> wanted_pop;
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
Snapshot outputs() {
  Snapshot value;
  for (unsigned i = 0; i < names.size(); ++i)
    value[i] = model->signal(names[i]).copy();
  return value;
}
void phase(const char *scope) {
  check(model == active_runtime() && svGetScope() &&
            std::string(svGetNameFromScope(svGetScope())) ==
                model->scope_name() + "/" + scope,
        "DebugROB callback scope changed");
  check(outputs() == before, "DebugROB outputs changed before deferred commit");
}
std::array<uint64_t, 8> words(unsigned id) {
  std::array<uint64_t, 8> value;
  for (unsigned i = 0; i < value.size(); ++i)
    value[i] = 0x8123456700000000ULL ^ (uint64_t(id) * 0x0101010101010101ULL) ^
               (uint64_t(i) * 0x0102040810204081ULL);
  return value;
}
std::vector<uint8_t> bytes(const std::array<uint64_t, 8> &value) {
  std::vector<uint8_t> result;
  for (auto word : value)
    for (unsigned i = 0; i < 8; ++i)
      result.push_back(word >> (i * 8));
  return result;
}
RobTrace trace(unsigned id) {
  RobTrace t;
  t.valid = 1;
  t.priv = id & 7;
  t.exception = id == 4;
  t.interrupt = id == 5;
  t.pc = 0x8000000000100000ULL + id * 4;
  t.insn = 0xfedcba9800000000ULL ^ (uint64_t(id) * 0x10001);
  t.cause = 0x8000000000000000ULL | id;
  t.tval = 0xfeed000000000000ULL ^ (uint64_t(id) * 0x10203);
  t.data = words(id);
  return t;
}
RobTrace with_data(RobTrace t, unsigned id) {
  t.data = words(id);
  return t;
}
struct Push {
  RobTrace value{};
  bool should = false, has = false;
  uint64_t tag = tag_a;
  uint32_t hart = hart_a;
};
struct Writeback {
  bool valid = false;
  uint64_t tag = tag_a;
  std::array<uint64_t, 8> data = words(19);
  uint32_t hart = hart_a;
};
void public_value(const RobTrace &t) {
  check(model->signal("out_valid").get() == t.valid &&
            model->signal("out_iaddr").get() == t.pc &&
            model->signal("out_insn").get() == t.insn &&
            model->signal("out_priv").get() == t.priv &&
            model->signal("out_exception").get() == t.exception &&
            model->signal("out_interrupt").get() == t.interrupt &&
            model->signal("out_cause").get() == t.cause &&
            model->signal("out_tval").get() == t.tval &&
            model->signal("out_wdata").copy() == bytes(t.data),
        "DebugROB published trace/512-bit payload changed");
}
struct Harness {
  Runtime &runtime;
  FILE *file;
  uint64_t time = 0;
  RobTrace pending{};
  unsigned cycles = 0;
  void cycle(const Push &push, const Writeback &wb, uint32_t hart,
             std::optional<RobTrace> result, bool reset = false) {
    auto old = outputs();
    auto counts = calls;
    runtime.advance(++time, [&] {
      runtime.signal("reset").set(reset);
      runtime.signal("push_hartid").set(push.hart);
      runtime.signal("push_should_wb").set(push.should);
      runtime.signal("push_has_wb").set(push.has);
      runtime.signal("push_wb_tag").set(push.tag);
      runtime.signal("push_valid").set(push.value.valid);
      runtime.signal("push_iaddr").set(push.value.pc);
      runtime.signal("push_insn").set(push.value.insn);
      runtime.signal("push_priv").set(push.value.priv);
      runtime.signal("push_exception").set(push.value.exception);
      runtime.signal("push_interrupt").set(push.value.interrupt);
      runtime.signal("push_cause").set(push.value.cause);
      runtime.signal("push_tval").set(push.value.tval);
      runtime.signal("push_wdata").assign(bytes(push.value.data));
      runtime.signal("wb_hartid").set(wb.hart);
      runtime.signal("wb_valid").set(wb.valid);
      runtime.signal("wb_tag").set(wb.tag);
      runtime.signal("wb_data").assign(bytes(wb.data));
      runtime.signal("pop_hartid").set(hart);
    });
    check(calls == counts && outputs() == old,
          "DebugROB callback or output changed while clock held low");
    before = outputs();
    wanted_pop.reset();
    runtime.advance(++time, [&] { runtime.signal("clock").set(1); });
    check(calls[0] == counts[0] + unsigned(!reset) &&
              calls[1] == counts[1] + unsigned(!reset) && calls[2] == counts[2],
          "DebugROB rising/reset callback counts changed");
    public_value(pending);
    auto visible = outputs();
    for (const auto &value : visible)
      check(std::fwrite(value.data(), value.size(), 1, file) == 1,
            "DebugROB trace write failed");
    auto rising_counts = calls;
    runtime.advance(++time);
    check(outputs() == visible && calls == rising_counts && !svGetScope(),
          "DebugROB held-high evaluation changed outputs or leaked scope");
    counts = calls;
    before = outputs();
    wanted_pop = result;
    runtime.advance(++time, [&] { runtime.signal("clock").set(0); });
    check(calls[0] == counts[0] && calls[1] == counts[1] &&
              calls[2] == counts[2] + unsigned(bool(result)) &&
              outputs() == visible,
          "DebugROB falling callback count or deferred output changed");
    if (result)
      pending = *result;
    runtime.advance(++time);
    check(calls[0] == counts[0] && calls[1] == counts[1] &&
              calls[2] == counts[2] + unsigned(bool(result)) &&
              outputs() == visible && !svGetScope(),
          "DebugROB held-low evaluation repeated a callback or leaked scope");
    ++cycles;
  }
};
void check_words(const long long *data, const char *port) {
  auto value = model->signal(port).copy();
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 8; ++j)
      check(uint8_t(uint64_t(data[i]) >> (j * 8)) == value[i * 8 + j],
            "DebugROB 512-bit push payload ABI changed");
}
} // namespace
extern "C" {
void __real_debug_rob_push_trace(int, char, char, long long, char, long long,
                                 long long, int, char, char, long long,
                                 long long, long long *);
void __real_debug_rob_push_wb(int, char, long long, long long *);
void __real_debug_rob_pop_trace(int, char *, long long *, long long *, int *,
                                char *, char *, long long *, long long *,
                                long long *);
void __wrap_debug_rob_push_trace(int hart, char should, char has, long long tag,
                                 char valid, long long pc, long long insn,
                                 int priv, char exception, char interrupt,
                                 long long cause, long long tval,
                                 long long *data) {
  phase("push");
  check(model->signal("clock").get() && !model->signal("reset").get(),
        "DebugROB push callback edge/reset gate changed");
  check(uint32_t(hart) == model->signal("push_hartid").get() &&
            should == model->signal("push_should_wb").get() &&
            has == model->signal("push_has_wb").get() &&
            uint64_t(tag) == model->signal("push_wb_tag").get() &&
            valid == model->signal("push_valid").get() &&
            uint64_t(pc) == model->signal("push_iaddr").get() &&
            uint64_t(insn) == model->signal("push_insn").get() &&
            priv == model->signal("push_priv").get() &&
            exception == model->signal("push_exception").get() &&
            interrupt == model->signal("push_interrupt").get() &&
            uint64_t(cause) == model->signal("push_cause").get() &&
            uint64_t(tval) == model->signal("push_tval").get(),
        "DebugROB trace argument ABI changed");
  check_words(data, "push_wdata");
  ++calls[0];
  __real_debug_rob_push_trace(hart, should, has, tag, valid, pc, insn, priv,
                              exception, interrupt, cause, tval, data);
  phase("push");
}
void __wrap_debug_rob_push_wb(int hart, char valid, long long tag,
                              long long *data) {
  phase("wb");
  check(model->signal("clock").get() && !model->signal("reset").get(),
        "DebugROB writeback callback edge/reset gate changed");
  check(uint32_t(hart) == model->signal("wb_hartid").get() &&
            valid == model->signal("wb_valid").get(),
        "DebugROB writeback hart/valid ABI changed");
  check(uint64_t(tag) == model->signal("wb_tag").get(),
        "DebugROB 64-bit writeback tag ABI changed");
  check_words(data, "wb_data");
  ++calls[1];
  __real_debug_rob_push_wb(hart, valid, tag, data);
  phase("wb");
}
void __wrap_debug_rob_pop_trace(int hart, char *valid, long long *pc,
                                long long *insn, int *priv, char *exception,
                                char *interrupt, long long *cause,
                                long long *tval, long long *data) {
  phase("pop");
  check(!model->signal("clock").get() && !model->signal("reset").get() &&
            bool(wanted_pop),
        "DebugROB pop callback edge/reset gate changed");
  check(uint32_t(hart) == model->signal("pop_hartid").get(),
        "DebugROB pop hart ABI changed");
  ++calls[2];
  __real_debug_rob_pop_trace(hart, valid, pc, insn, priv, exception, interrupt,
                             cause, tval, data);
  const auto &t = *wanted_pop;
  check(*valid == t.valid && uint64_t(*pc) == t.pc &&
            uint64_t(*insn) == t.insn && *priv == t.priv &&
            *exception == t.exception && *interrupt == t.interrupt &&
            uint64_t(*cause) == t.cause && uint64_t(*tval) == t.tval,
        "original DebugROB queue/trace ordering changed");
  for (unsigned i = 0; i < 8; ++i)
    check(uint64_t(data[i]) == t.data[i],
          "original DebugROB writeback match changed");
  phase("pop");
}
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "expected owned DebugROB trace filename");
    auto runtime = create_model(argc, argv);
    model = runtime.get();
    runtime->initialize();
    runtime->advance(0, [&] { runtime->signal("reset").set(1); });
    FILE *file = std::fopen(argv[1], "wb");
    check(file, "cannot open owned DebugROB trace");
    Harness h{*runtime, file};
    RobTrace zero;
    auto x = trace(1), y = trace(2), z = trace(3), v = trace(4), u = trace(5),
         t = trace(6), w = trace(7), q = trace(8);
    h.cycle({}, {}, hart_a, std::nullopt, true);
    h.cycle({}, {}, hart_a, std::nullopt); // Registered reset blocks this pop.
    h.cycle({}, {}, hart_a, zero);
    h.cycle({x, true, false, tag_a}, {}, hart_a, zero);
    h.cycle({y, false, true}, {}, hart_a, zero); // Cannot bypass waiting X.
    h.cycle({z, false, true, tag_b, hart_b},
            {true, tag_a ^ (uint64_t(1) << 48), words(9)}, hart_a, zero);
    h.cycle({}, {true, tag_a, words(10)}, hart_b, z);
    h.cycle({}, {}, hart_a, with_data(x, 10));
    h.cycle({}, {}, hart_a, y);
    h.cycle({}, {}, hart_a, zero);
    h.cycle({}, {true, tag_b, words(11), hart_b}, hart_b, zero);
    h.cycle({t, true, false, tag_b, hart_b}, {}, hart_b, with_data(t, 11));
    // The pop payload/valid is retained throughout reset and its release guard.
    h.cycle({v, false, true}, {true, tag_c, words(12)}, hart_b, std::nullopt,
            true);
    h.cycle({}, {}, hart_b, std::nullopt);
    h.cycle({}, {}, hart_b, zero);
    h.cycle({}, {}, hart_a, zero); // The attempted reset push was suppressed.
    h.cycle({v, false, false}, {}, hart_a, v);
    h.cycle({u, true, true, tag_c}, {true, tag_c, words(12)}, hart_a, u);
    h.cycle({}, {}, hart_a, zero);
    h.cycle({w, true, false, tag_c + 1}, {}, hart_a, zero);
    h.cycle({q, true, false, tag_d}, {true, tag_d, words(13)}, hart_a, zero);
    h.cycle({}, {true, tag_c + 1, words(14)}, hart_a, std::nullopt, true);
    h.cycle({}, {}, hart_a, std::nullopt);
    h.cycle({}, {true, tag_c + 1, words(14)}, hart_a, with_data(w, 14));
    h.cycle({}, {}, hart_a, with_data(q, 13));
    h.cycle({}, {}, hart_a, zero);
    h.cycle({}, {}, hart_b, zero);
    check(h.cycles == 27 && calls == std::array<unsigned, 3>{{24, 24, 21}},
          "DebugROB complete scenario callback counts changed");
    check(!std::fclose(file), "DebugROB trace close failed");
    std::puts("PASS original DebugROB: cycles=27 push=24 wb=24 pop=21 harts=2 "
              "words=8");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
