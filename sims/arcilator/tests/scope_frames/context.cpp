#include "native/runtime.h"
#include "svdpi.h"
#include <cstdio>
#include <map>
using namespace chipyard_arc;
static std::map<std::pair<std::string, int>, unsigned> calls;
static std::map<std::string, unsigned> empty_calls;
static svScope first_scope, second_scope;
static unsigned stops;
extern "C" void __real_chipyard_arc_stop_fail();
extern "C" void __wrap_chipyard_arc_stop_fail() {
  ++stops;
  __real_chipyard_arc_stop_fail();
}
extern "C" int scope_probe(int tag) {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || scope != svGetScopeFromName(name))
    throw std::runtime_error("context scope handle mismatch");
  std::string path = name;
  if (path != "FrameTop/first/own" && path != "FrameTop/second/own")
    throw std::runtime_error("wrong scope: " + path);
  if (path == "FrameTop/first/own")
    first_scope = scope;
  else
    second_scope = scope;
  ++calls[{path, tag}];
  return tag + 7;
}
extern "C" int scope_empty() {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || scope != svGetScopeFromName(name))
    throw std::runtime_error("bare DPI task lost scope handle");
  std::string path = name;
  if (path != "FrameTop/first/own" && path != "FrameTop/second/own")
    throw std::runtime_error("bare DPI task wrong physical scope");
  ++empty_calls[path];
  return 0;
}
int main(int argc, char **argv) {
  auto m = create_model(argc, argv);
  m->signal("arc_stop_enable").set(1);
  m->signal("arc_printf_enable").set(1);
  m->initialize();
  m->advance(0, [] {});
  for (auto tag : {3, 5, 7, 11, 13, 17})
    for (auto side : {"first", "second"}) {
      unsigned expected = tag < 10 ? 1 : std::string(side) == "first" ? 2 : 0;
      if (calls[{std::string("FrameTop/") + side + "/own", tag}] != expected)
        throw std::runtime_error("wrong physical context call count");
    }
  if (calls.size() != 12 || !first_scope || !second_scope ||
      first_scope == second_scope)
    throw std::runtime_error("physical scope instances collapsed");
  if (stops)
    throw std::runtime_error("context return-value assertion fired");
  if (empty_calls["FrameTop/first/own"] != 4 ||
      empty_calls["FrameTop/second/own"] != 2 || empty_calls.size() != 2)
    throw std::runtime_error("bare DPI task physical call count mismatch");
  auto empty_before = empty_calls;
  auto before = calls;
  m->advance(1, [] {});
  if (calls != before || empty_calls != empty_before)
    throw std::runtime_error("context process restarted");
  std::puts("PASS: 18 direct/relative/root/nested/task context-DPI callbacks "
            "use distinct physical scope handles");
}
