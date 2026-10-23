#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
using namespace chipyard_arc;
extern "C" int sv_counter();
extern "C" int sv_inner_count();
extern "C" int sv_update(int, int *);
extern "C" int sv_roundtrip(int, int *);
extern "C" void chipyard_arc_sv_fflush_all();
static std::array<svScope, 2> scopes;
static std::array<unsigned, 2> outer_calls{}, inner_calls{}, successes{},
    aborts{};
static std::array<unsigned, 3> child_calls{};
static bool bad;
static int bad_child = -1;
static unsigned stops;
extern "C" void __real_chipyard_arc_stop_fail();
extern "C" void __wrap_chipyard_arc_stop_fail() {
  ++stops;
  __real_chipyard_arc_stop_fail();
}
static void check(int tag) {
  if (tag < 0 || tag > 1 || svGetScope() != scopes[tag])
    throw std::runtime_error("task callback scope handle mismatch");
  auto name = svGetNameFromScope(svGetScope());
  std::string expected = std::string("TaskCallbacksHarness/original/rtl/rtl/") +
                         (tag ? "right" : "left");
  if (!name || expected != name || svGetScopeFromName(name) != scopes[tag])
    throw std::runtime_error("task callback physical scope mismatch");
}
extern "C" int host_child(int tag, svBit fixed_target) {
  if (tag < 0 || tag > 1 || (fixed_target && tag))
    throw std::runtime_error("child callback argument changed");
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  std::string expected = std::string("TaskCallbacksHarness/original/rtl/rtl/") +
                         (tag ? "right/own" : "left/own");
  if (!name || expected != name || svGetScopeFromName(name) != scope)
    throw std::runtime_error("qualified child callback scope mismatch");
  ++child_calls[fixed_target ? 2 : tag];
  if (bad_child == int(fixed_target))
    return -1;
  return tag + 11;
}
extern "C" int host_inner(int tag, int *value) {
  check(tag);
  ++inner_calls[tag];
  if (bad && tag == 0)
    return 1;
  *value = sv_counter() * 3 + tag;
  // A nested import may change C's current scope. Both SV and C callers must
  // resume with their own original frame after normal completion.
  svSetScope(scopes[1 - tag]);
  return 0;
}
extern "C" int host_task(int tag, int delta, int *value) {
  check(tag);
  ++outer_calls[tag];
  int updated = -1, inner = -1;
  if (sv_update(delta, &updated) != 0)
    throw std::runtime_error("normal SV task completion was not zero");
  check(tag);
  int status = sv_roundtrip(tag, &inner);
  check(tag);
  if (status) {
    ++aborts[tag];
    return status;
  }
  if (inner != updated * 3 + tag)
    throw std::runtime_error("nested task return/output mismatch");
  *value = updated + inner;
  ++successes[tag];
  return 0;
}
int main(int argc, char **argv) {
  for (unsigned recreation = 0; recreation < 2; ++recreation) {
    outer_calls = {};
    inner_calls = {};
    successes = {};
    aborts = {};
    stops = 0;
    child_calls = {};
    bad = false;
    auto m = create_model(argc, argv);
    m->signal("arc_stop_enable").set(1);
    m->signal("arc_printf_enable").set(0);
    m->initialize();
    m->advance(0, [&] {
      m->signal("clock").set(0);
      m->signal("reset").set(1);
      m->signal("enable").set(0);
    });
    for (unsigned n = 0; n < 2; ++n) {
      std::string name = std::string("TaskCallbacksHarness/original/rtl/rtl/") +
                         (n ? "right" : "left");
      scopes[n] = svGetScopeFromName(name.c_str());
      if (!scopes[n])
        throw std::runtime_error("task scope missing");
    }
    if (scopes[0] == scopes[1])
      throw std::runtime_error("canonical task module scopes collapsed");
    std::array<int, 2> counter{}, observed{};
    std::array<unsigned, 2> completions{};
    for (unsigned cycle = 0; cycle < 48; ++cycle) {
      bool reset = cycle % 13 == 0, enable = cycle % 3 != 0,
           printing = cycle % 4 == 1;
      m->advance(cycle * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
        m->signal("arc_printf_enable").set(printing);
      });
      if (reset) {
        counter = {};
        observed = {};
        completions = {};
      } else if (enable)
        for (unsigned n = 0; n < 2; ++n) {
          counter[n] += n + 1;
          observed[n] = counter[n] * 4 + n;
          ++completions[n];
          if (printing)
            std::fprintf(
                stderr,
                "EXPECTED_TASK_CALLBACK NATIVE_TASK_CALLBACK tag=%u value=%d\n",
                n, counter[n] * 3 + int(n));
        }
      m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
      if (m->signal("first").get() != unsigned(observed[0]) ||
          m->signal("second").get() != unsigned(observed[1]))
        throw std::runtime_error("clocked task output/reset/enable mismatch");
      for (unsigned n = 0; n < 2; ++n) {
        svSetScope(scopes[n]);
        if (sv_counter() != counter[n] ||
            sv_inner_count() != int(completions[n]))
          throw std::runtime_error(
              "task callback captured wrong instance state");
      }
    }
    if (stops || outer_calls != inner_calls || outer_calls != successes ||
        outer_calls[0] != 30 || outer_calls[1] != 30)
      throw std::runtime_error("normal task call count/status mismatch");
    if (child_calls != std::array<unsigned, 3>{30, 30, 60})
      throw std::runtime_error("relative/fixed child call counts changed");
    // Invalid status inside the exported task returns nonzero to its C caller,
    // which propagates it. Neither the exported task's continuation nor the
    // original clocked process may execute afterwards.
    int prior = observed[0];
    unsigned complete = completions[0];
    bad = true;
    m->advance(96, [&] {
      m->signal("clock").set(0);
      m->signal("reset").set(0);
      m->signal("enable").set(1);
    });
    m->advance(97, [&] { m->signal("clock").set(1); });
    svSetScope(scopes[0]);
    if (aborts[0] != 1 || stops != 2 ||
        m->signal("first").get() != unsigned(prior) ||
        sv_inner_count() != int(complete))
      throw std::runtime_error("nested task failure executed a continuation");
    auto before = outer_calls[0];
    m->advance(98, [&] { m->signal("clock").set(0); });
    m->advance(99, [&] { m->signal("clock").set(1); });
    if (outer_calls[0] != before || aborts[0] != 1 || stops != 2)
      throw std::runtime_error("failed callback task restarted");
  }
  // Execute both original $fatal diagnostics, including their lack of a
  // trailing newline. Print and stop controls apply independently, and the
  // exported task must return without running its normal continuation.
  for (unsigned selected = 0; selected < 2; ++selected)
    for (unsigned printing = 0; printing < 2; ++printing) {
      outer_calls = {};
      inner_calls = {};
      stops = 0;
      bad = false;
      bad_child = selected;
      auto m = create_model(argc, argv);
      m->signal("arc_stop_enable").set(1);
      m->signal("arc_printf_enable").set(printing);
      m->initialize();
      m->advance(0, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(1);
        m->signal("enable").set(0);
      });
      for (unsigned n = 0; n < 2; ++n) {
        std::string name =
            std::string("TaskCallbacksHarness/original/rtl/rtl/") +
            (n ? "right" : "left");
        scopes[n] = svGetScopeFromName(name.c_str());
      }
      svSetScope(scopes[1]);
      unsigned marker = selected * 2 + printing;
      std::printf("FATAL_BEGIN_%u\n", marker);
      std::fflush(stdout);
      int value = -1;
      int status = sv_roundtrip(1, &value);
      chipyard_arc_sv_fflush_all();
      std::printf("\nFATAL_END_%u\n", marker);
      std::fflush(stdout);
      check(1);
      if (status != 1 || stops != 1 || sv_inner_count() != 0 ||
          outer_calls != std::array<unsigned, 2>{} ||
          inner_calls != std::array<unsigned, 2>{})
        throw std::runtime_error("exported fatal task executed a continuation");
    }
  std::puts("PASS: clocked nested C/SV task callbacks, two canonical "
            "instances, reset/enable, C completion status, scope restoration, "
            "invalid-status propagation and model recreation");
}
