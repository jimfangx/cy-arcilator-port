#include "native/runtime.h"
#include "svdpi.h"
extern "C" int declaration_get_cookie();
extern "C" void declaration_set_cookie(int);
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {
std::array<unsigned, 2> seeds{}, steps{}, observers{}, ticks{}, discards{},
    handles{}, handle_uses{};
std::array<uint64_t, 2> handle_storage{};
bool startup;
svScope outside_scope;
std::array<unsigned, 2> package_calls{};
int scope_key, package_keys[2], scope_markers[2];
std::string local_scope(int tag) {
  return std::string("DeclarationDPIProbe/") +
         (tag == 17 ? "first" : "second") + "/rtl/rtl";
}
void ambient() {
  if (svGetScope() != outside_scope)
    throw std::runtime_error("declaration callback scope restoration failed");
}

unsigned index(int tag) {
  if (tag != 17 && tag != 29)
    throw std::runtime_error("declaration tag ABI changed");
  return tag == 29;
}
uint64_t seed(int tag) {
  return 0xfedcba9800000000ULL | (uint64_t(tag) * 0x10203);
}
uint64_t derived(int tag) {
  return seed(tag) ^ 0x8000000100000011ULL ^ uint64_t(tag);
}
void require(bool condition, const char *why) {
  if (!condition)
    throw std::runtime_error(why);
}
} // namespace
extern "C" uint64_t declaration_seed(int tag) {
  unsigned i = index(tag);
  require(startup && ++seeds[i] == 1 && steps[i] == 0,
          "declaration seed repeated or wrong startup order");
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  require(name && local_scope(tag) == name && svGetScopeFromName(name) == scope,
          "declaration context scope mismatch");
  require(!svGetUserData(scope, &scope_key) &&
              svPutUserData(scope, &scope_key, &scope_markers[i]) == 0,
          "declaration context user data/recreation mismatch");
  require(
      declaration_get_cookie() == tag + 7,
      "declaration context export saw uninitialized or wrong instance state");
  declaration_set_cookie(tag + 107);
  require(declaration_get_cookie() == tag + 107,
          "declaration context export write lost its physical instance");
  auto package = svGetScopeFromName("declaration_context_pkg");
  require(package && svSetScope(package) == scope,
          "declaration context could not select another scope");
  return seed(tag);
}
extern "C" uint64_t declaration_step(uint64_t value, int tag) {
  ambient();
  unsigned i = index(tag);
  require(startup && seeds[i] == 1 && ++steps[i] == 1 && value == seed(tag),
          "declaration dependency/order/64-bit ABI mismatch");
  return derived(tag);
}
extern "C" void *declaration_handle(int tag) {
  ambient();
  unsigned i = index(tag);
  require(startup && steps[i] == 1 && ++handles[i] == 1,
          "chandle initializer repeated or reordered");
  return &handle_storage[i];
}
extern "C" uint64_t declaration_handle_value(void *handle, int tag) {
  ambient();
  unsigned i = index(tag);
  require(startup && handles[i] == 1 && ++handle_uses[i] == 1 &&
              handle == &handle_storage[i],
          "declaration chandle dependency/ABI changed");
  return 0x9876543210000000ULL + uint64_t(tag);
}
extern "C" uint64_t declaration_package_seed(int tag) {
  unsigned i = index(tag);
  auto scope = svGetScope();
  auto name = svGetNameFromScope(scope);
  require(startup && handle_uses[i] == 1 && ++package_calls[i] == 1 && name &&
              std::string(name) == "declaration_context_pkg" &&
              svGetScopeFromName(name) == scope,
          "package declaration context/order mismatch");
  require(!svGetUserData(scope, &package_keys[i]) &&
              svPutUserData(scope, &package_keys[i], &scope_markers[i]) == 0,
          "package declaration context user data/recreation mismatch");
  auto local = svGetScopeFromName(local_scope(tag).c_str());
  require(local && svSetScope(local) == scope,
          "package declaration could not select another scope");
  return 0xfedcba9876543210ULL + uint64_t(tag);
}
extern "C" uint64_t declaration_discard(int tag) {
  ambient();
  unsigned i = index(tag);
  require(startup && handle_uses[i] == 1 && ++discards[i] == 1,
          "unused declaration effect dropped, repeated or reordered");
  return 0x8000000012345678ULL;
}
extern "C" void declaration_observe(int tag, uint64_t value, uint64_t next) {
  ambient();
  auto saved = svSetScope(svGetScopeFromName(local_scope(tag).c_str()));
  auto cookie = declaration_get_cookie();
  svSetScope(saved);
  require(cookie == tag + 107,
          "declaration export write was overwritten before time-zero process");
  unsigned i = index(tag);
  require(seeds[0] == 1 && seeds[1] == 1 && steps[0] == 1 && steps[1] == 1 &&
              discards[0] == 1 && discards[1] == 1 && ++observers[i] == 1 &&
              value == seed(tag) && next == derived(tag),
          "time-zero process saw uninitialized declaration");
}
extern "C" uint32_t declaration_tick(int tag, uint32_t data) {
  ambient();
  require(!startup && observers[index(tag)] == 1,
          "clocked DPI ran before initialization");
  ++ticks[index(tag)];
  return data ^ uint32_t(tag);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected binary trace path");
    FILE *trace = std::fopen(argv[1], "wb");
    require(trace, "cannot open declaration trace");
    chipyard_arc::DpiScope outside("declaration-initial-caller");
    auto saved_scope = svSetScope(outside.handle());
    outside_scope = outside.handle();
    const std::array<const char *, 10> names{
        {"first", "first_derived", "first_shared", "first_observed",
         "first_live", "second", "second_derived", "second_shared",
         "second_observed", "second_live"}};
    for (unsigned recreation = 0; recreation < 2; ++recreation) {
      seeds = steps = observers = ticks = discards = handles = handle_uses = {};
      package_calls = {};
      ambient();
      startup = true;
      auto model = chipyard_arc::create_model(argc, argv);
      model->initialize();
      ambient();
      require(package_calls == std::array<unsigned, 2>{{1, 1}},
              "package initializer did not run exactly once before evaluation");
      require(seeds == std::array<unsigned, 2>{{1, 1}} && steps == seeds &&
                  discards == seeds && handles == seeds &&
                  handle_uses == seeds && handles == seeds &&
                  handle_uses == seeds,
              "DPI declarations did not initialize exactly once");
      startup = false;
      unsigned expected_ticks = 0;
      bool previous = false;
      for (unsigned sample = 0; sample < 24; ++sample) {
        bool clock = bool(sample & 1), reset = sample >= 5 && sample < 10;
        uint32_t data = 0xf1234567U + sample * 0x10203U;
        expected_ticks += clock && !previous && !reset;
        model->advance(sample * 1000, [&] {
          model->signal("clock").set(clock);
          model->signal("reset").set(reset);
          model->signal("data").set(data);
        });
        require(observers == std::array<unsigned, 2>{{1, 1}} &&
                    ticks == std::array<unsigned, 2>{{expected_ticks,
                                                      expected_ticks}},
                "ordinary initial/clocked DPI execution count changed");
        for (unsigned held = 0; held < 2; ++held) {
          if (held)
            model->advance(sample * 1000 + 1, [&] {
              model->signal("data").set(data ^ 0xffffffffU);
            });
          require(seeds == std::array<unsigned, 2>{{1, 1}} && steps == seeds &&
                      discards == seeds && handles == seeds &&
                      handle_uses == seeds &&
                      ticks == std::array<unsigned, 2>{{expected_ticks,
                                                        expected_ticks}},
                  "held evaluation repeated declaration/clocked DPI");
          for (auto name : names) {
            auto bytes = model->signal(name).copy();
            require(std::fwrite(bytes.data(), 1, bytes.size(), trace) ==
                        bytes.size(),
                    "cannot write declaration trace");
          }
        }
        previous = clock;
      }
      model.reset();
      ambient();
      require(!svGetScopeFromName(local_scope(17).c_str()) &&
                  !svGetScopeFromName(local_scope(29).c_str()) &&
                  !svGetScopeFromName("declaration_context_pkg"),
              "declaration context registry leaked after model destruction");
    }
    svSetScope(saved_scope);
    require(std::fclose(trace) == 0, "cannot close declaration trace");
    std::puts("PASS native DPI declarations: once-only startup, dependencies, "
              "time-zero observers, clocked calls and recreation");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
