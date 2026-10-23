// Public and inline DPI scopes must have the same observable behavior.
#if defined(ARC_SCOPE_ABI_PROBE)
#include "native/compat.h"
static_assert(sizeof(chipyard_arc::DpiContext) == sizeof(void *));
extern "C" void legacy_context(chipyard_arc::DpiScope &scope,
                               void (*call)(void *), void *arg) {
  chipyard_arc::DpiContext context(scope);
  call(arg);
}

#elif defined(ARC_SCOPE_SHARED_HOST)
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <thread>
using Function = int (*)(unsigned);
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  for (unsigned recreation = 0; recreation < 2; ++recreation) {
    std::mutex m;
    std::condition_variable cv;
    unsigned waiting = 0;
    bool ready = false;
    Function call = nullptr;
    std::atomic<unsigned> passed{0};
    std::array<std::thread, 4> threads;
    for (unsigned i = 0; i < 4; ++i)
      threads[i] = std::thread([&, i]() {
        std::unique_lock<std::mutex> lock(m);
        ++waiting;
        cv.notify_all();
        cv.wait(lock, [&]() { return ready; });
        lock.unlock();
        if (call(i) != 1)
          std::abort();
        ++passed;
      });
    {
      std::unique_lock<std::mutex> lock(m);
      cv.wait(lock, [&]() { return waiting == 4; });
    }
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
      std::fprintf(stderr, "%s\n", dlerror());
      return 3;
    }
    call = reinterpret_cast<Function>(dlsym(module, "scope_module_test"));
    if (!call)
      return 4;
    {
      std::lock_guard<std::mutex> lock(m);
      ready = true;
    }
    cv.notify_all();
    for (auto &t : threads)
      t.join();
    if (passed != 4 || dlclose(module))
      return 5;
  }
  printf("PASS recreations=2 threads_started_before_dlopen=8 "
         "nested_scopes_and_exception_restore=1024\n");
}

#elif defined(ARC_SCOPE_SHARED_MODULE)
#include "native/scope_guard.h"
#include <cstdlib>
#include <stdexcept>
#include <string>
using namespace chipyard_arc;
extern "C" int scope_module_test(unsigned id) {
  if (svGetScope() != nullptr)
    std::abort();
  DpiScope a("late/a/" + std::to_string(id)), b("late/b/" + std::to_string(id));
  for (unsigned n = 0; n < 128; ++n) {
    {
      detail::ScopedDpiContext outer(a);
      if (svGetScope() != a.handle())
        std::abort();
      try {
        DpiContext inner(b);
        if (svGetScope() != b.handle())
          std::abort();
        throw std::runtime_error("late unwind");
      } catch (const std::runtime_error &) {
      }
      if (svGetScope() != a.handle())
        std::abort();
    }
    if (svGetScope() != nullptr)
      std::abort();
  }
  return 1;
}

#else
#ifndef INLINE_SCOPE
#define INLINE_SCOPE 1
#endif
#include "native/scope_guard.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
using namespace chipyard_arc;
#if INLINE_SCOPE
using Guard = detail::ScopedDpiContext;
#else
using Guard = DpiContext;
#endif
static_assert(sizeof(DpiContext) == sizeof(void *) &&
              sizeof(detail::ScopedDpiContext) == sizeof(DpiContext));
extern "C" void legacy_context(DpiScope &, void (*)(void *), void *);
void check(bool c) {
  if (!c)
    std::abort();
}
struct Args {
  DpiScope *a, *b, *c;
};
void callback(void *opaque) {
  auto &a = *static_cast<Args *>(opaque);
  check(svGetScope() == a.b->handle());
  {
    Guard nested(*a.c);
    check(svGetScope() == a.c->handle());
  }
  check(svGetScope() == a.b->handle());
  svSetScope(a.c->handle());
}
int main() {
  check(svGetScope() == nullptr);
  DpiScope a("root/a"), b("root/b"), c("root/c");
  check(svSetScope(a.handle()) == nullptr);
  int key = 0, data = 5;
  check(svPutUserData(a.handle(), &key, &data) == 0 &&
        svGetUserData(a.handle(), &key) == &data &&
        svGetUserData(b.handle(), &key) == nullptr);
  check(std::strcmp(svGetNameFromScope(a.handle()), "root/a") == 0 &&
        svGetScopeFromName("root/b") == b.handle());
  Args args{&a, &b, &c};
  for (unsigned i = 0; i < 1024; ++i) {
    legacy_context(b, callback, &args);
    check(svGetScope() == a.handle());
    {
      Guard outer(b);
      check(svGetScope() == b.handle());
      {
        DpiContext old(c);
        check(svGetScope() == c.handle());
      }
      check(svGetScope() == b.handle());
    }
    check(svGetScope() == a.handle());
    try {
      Guard inner(b);
      svSetScope(c.handle());
      throw std::runtime_error("scope unwind");
    } catch (const std::runtime_error &) {
    }
    check(svGetScope() == a.handle());
  }
  svScope expired;
  {
    DpiScope temporary("temporary");
    expired = temporary.handle();
    check(svPutUserData(expired, &key, &data) == 0);
    {
      Guard g(temporary);
      check(svGetScope() == expired);
    }
    check(svGetScope() == a.handle());
  }
  check(svGetNameFromScope(expired) == nullptr &&
        svGetScopeFromName("temporary") == nullptr &&
        svGetUserData(expired, &key) == nullptr);
  {
    DpiScope first("duplicate");
    check(svGetScopeFromName("duplicate") == first.handle());
    {
      DpiScope second("duplicate");
      check(svGetScopeFromName("duplicate") == second.handle());
    }
    check(svGetScopeFromName("duplicate") == first.handle());
  }
  check(!svGetScopeFromName("duplicate"));
  std::array<std::thread, 4> threads;
  std::atomic<unsigned> done{0};
  for (unsigned id = 0; id < 4; ++id)
    threads[id] = std::thread([&, id]() {
      check(svGetScope() == nullptr);
      DpiScope own("worker/" + std::to_string(id));
      for (unsigned j = 0; j < 1024; ++j) {
        {
          Guard g(own);
          std::this_thread::yield();
          check(svGetScope() == own.handle());
        }
        check(svGetScope() == nullptr);
      }
      ++done;
    });
  for (auto &t : threads)
    t.join();
  check(done == 4 && svGetScope() == a.handle());
  check(svSetScope(nullptr) == a.handle());
  printf("PASS nested=1024 exceptions=1024 legacy_ABI=1024 thread_scopes=4096 "
         "registry_cleanup=1 scope_bytes=%zu\n",
         sizeof(DpiContext));
}

#endif
