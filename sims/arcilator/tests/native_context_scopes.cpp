#include "native/runtime.h"
#include "svdpi.h"
#include <array>
#include <cstdio>
#include <map>
using namespace chipyard_arc;
extern "C" int context_get();
extern "C" void context_set(int);
extern "C" int context_export_probe(int);
extern "C" int context_export_package(int);
static int key, package_data, startup_key;
static unsigned startup_calls;
static std::map<std::string, svScope> startup_scopes;
static svScope expected_frame;
static std::array<int, 4> markers;
static unsigned ticks, probes, packages, plain_calls;
static std::string scope_for(unsigned tag) {
  return std::string("ContextHarness/") + (tag >> 8 ? "right" : "left") +
         "/rtl/rtl/banks_" + std::to_string((tag & 255) == 1) + ".leaf.dot";
}
static svScope checked(unsigned tag) {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || scope_for(tag) != name || scope != svGetScopeFromName(name))
    throw std::runtime_error("physical declaration scope mismatch");
  auto index = (tag >> 8) * 2 + ((tag & 255) == 1);
  if (svGetUserData(scope, &key) != &markers[index])
    throw std::runtime_error("context user data scope mismatch");
  return scope;
}
extern "C" int context_startup() {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || svGetScopeFromName(name) != scope ||
      !startup_scopes.emplace(name, scope).second)
    throw std::runtime_error("initial context scope registration mismatch");
  for (unsigned i = 0; i < 4; i++)
    if (scope_for((i / 2) * 256 + i % 2) == name) {
      if (svPutUserData(scope, &startup_key, &markers[i]))
        throw std::runtime_error("initial context user data failed");
      ++startup_calls;
      return 1;
    }
  throw std::runtime_error("initial context bound to wrong physical instance");
}
extern "C" int context_tick(int tag) {
  if (tag == 99) {
    if (svGetScope() != expected_frame)
      throw std::runtime_error("plain import inherited context alias scope");
    ++plain_calls;
    return 0;
  }
  checked(unsigned(tag));
  ++ticks;
  int value = context_get() + 1;
  context_set(value);
  if (context_get() != value)
    throw std::runtime_error("context callback export lost its captured state");
  svSetScope(svGetScopeFromName(scope_for((tag & ~255) | 1).c_str()));
  return value;
}
extern "C" int context_plain(int tag) { return context_tick(tag); }
extern "C" int context_probe(int tag) {
  checked(unsigned(tag));
  ++probes;
  return context_get();
}
extern "C" int context_package(int tag) {
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  if (!name || std::string(name) != "context_pkg" ||
      svGetUserData(scope, &key) != &package_data)
    throw std::runtime_error("package context scope mismatch");
  ++packages;
  return tag + 500;
}
int main(int argc, char **argv) {
  std::array<svScope, 5> old{};
  for (unsigned recreation = 0; recreation < 2; recreation++) {
    startup_calls = 0;
    startup_scopes.clear();
    auto m = create_model(argc, argv);
    m->initialize();
    // LLHD initial processes run on the first scheduler evaluation.
    m->advance(0, [&] {
      m->signal("clock").set(0);
      m->signal("reset").set(1);
      m->signal("enable").set(0);
    });
    if (startup_calls != 4)
      throw std::runtime_error("initial context execution count mismatch: " +
                               std::to_string(startup_calls));
    std::array<svScope, 4> scopes;
    for (unsigned i = 0; i < 4; i++) {
      scopes[i] = svGetScopeFromName(scope_for((i / 2) * 256 + i % 2).c_str());
      if (!scopes[i] || svPutUserData(scopes[i], &key, &markers[i]))
        throw std::runtime_error("scope registration/user data failed");
      if (svGetUserData(scopes[i], &startup_key) != &markers[i])
        throw std::runtime_error("initial context user data lost");
      old[i] = scopes[i];
    }
    auto pkg = svGetScopeFromName("context_pkg");
    old[4] = pkg;
    if (!pkg || svPutUserData(pkg, &key, &package_data))
      throw std::runtime_error("package scope registration failed");
    if (svPutUserData(nullptr, &key, &key) != -1 ||
        svPutUserData(pkg, &key, nullptr) != -1 || svGetUserData(nullptr, &key))
      throw std::runtime_error("invalid scope user data accepted");
    ticks = probes = packages = plain_calls = 0;
    std::array<unsigned, 4> expected{};
    unsigned active = 0;
    for (unsigned cycle = 0; cycle < 128; cycle++) {
      bool reset = cycle % 31 == 0, enable = cycle % 5 != 0;
      expected_frame = scopes[0];
      svSetScope(expected_frame);
      m->advance(cycle * 2, [&] {
        m->signal("clock").set(0);
        m->signal("reset").set(reset);
        m->signal("enable").set(enable);
      });
      m->advance(cycle * 2 + 1, [&] { m->signal("clock").set(1); });
      if (reset)
        expected.fill(0);
      else if (enable) {
        ++active;
        for (unsigned i = 0; i < 4; i++)
          expected[i] += i % 2 ? 2 : 3;
      }
      if (m->signal("left").get() || m->signal("right").get() ||
          ticks != active * 10 || plain_calls != active * 2)
        throw std::runtime_error(
            "context effects/alias/order/reset/enable mismatch");
      for (unsigned i = 0; i < 4; i++) {
        svSetScope(scopes[i]);
        if (unsigned(context_get()) != expected[i] ||
            unsigned(context_export_probe((i / 2) * 256 + i % 2)) !=
                expected[i] ||
            context_export_package(9) != 509)
          throw std::runtime_error(
              "exported context frame/captured state mismatch");
      }
    }
    m.reset();
    for (auto scope : old)
      if (svGetUserData(scope, &key) || svGetUserData(scope, &startup_key))
        throw std::runtime_error("context data survived model destruction");
  }
  std::puts("PASS: 1024 physical context scope/export observations, shared RTL "
            "definitions and two outer instances, lexical/absolute imports, "
            "generated/escaped instance names, "
            "package scopes, initial callbacks, alias qualifiers, C/SV "
            "callbacks, scope "
            "restoration, user data and model recreation");
}
