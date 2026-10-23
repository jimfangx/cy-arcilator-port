#pragma once
#include "compat.h"
namespace chipyard_arc::detail {
// Trivial TLS has no dynamic initializer. Keep it hidden to this adapter;
// the default TLS model remains valid for a dynamically loaded Runtime.
#if defined(__GNUC__) || defined(__clang__)
extern __thread svScope current_scope __attribute__((visibility("hidden")));
#else
extern thread_local svScope current_scope;
#endif
// The same exchange/restore as public DpiContext, inlined in Runtime loops.
// Every callback retains its scope, including unknown and foreign callbacks.
class ScopedDpiContext {
  svScope previous;

public:
  explicit ScopedDpiContext(DpiScope &scope) noexcept
      : previous(current_scope) {
    current_scope = scope.handle();
  }
  ~ScopedDpiContext() { current_scope = previous; }
  ScopedDpiContext(const ScopedDpiContext &) = delete;
  ScopedDpiContext &operator=(const ScopedDpiContext &) = delete;
};
} // namespace chipyard_arc::detail
