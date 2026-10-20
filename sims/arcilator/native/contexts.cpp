#include "runtime.h"
#include <cctype>
namespace {
std::string parent(const std::string &path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos)
    throw std::invalid_argument("native context scope lacks its parent");
  return path.substr(0, slash);
}
std::string suffix(const std::string &path) {
  std::string result;
  size_t at = 0;
  while (at < path.size()) {
    if (path[at++] != '.')
      throw std::invalid_argument("invalid native DPI relative scope");
    std::string component;
    if (at < path.size() && path[at] == '\\') {
      ++at;
      while (at < path.size() &&
             !std::isspace(static_cast<unsigned char>(path[at])))
        component += path[at++];
      if (at == path.size())
        throw std::invalid_argument(
            "unterminated escaped native scope component");
      ++at;
    } else {
      while (at < path.size() &&
             (std::isalnum(static_cast<unsigned char>(path[at])) ||
              path[at] == '_' || path[at] == '$'))
        component += path[at++];
    }
    if (component.empty())
      throw std::invalid_argument("empty native scope component");
    while (at < path.size() && path[at] == '[') {
      size_t start = at++;
      if (at < path.size() && path[at] == '-')
        ++at;
      size_t digits = at;
      while (at < path.size() &&
             std::isdigit(static_cast<unsigned char>(path[at])))
        ++at;
      if (at == digits || at == path.size() || path[at++] != ']')
        throw std::invalid_argument("invalid native scope index");
      component += path.substr(start, at - start);
    }
    result += '/' + component;
  }
  return result;
}
} // namespace
namespace chipyard_arc {
DpiScope &Runtime::owned_scope(const std::string &name) {
  if (name == root_scope.name())
    return root_scope;
  auto &scope = export_scopes[name];
  if (!scope)
    scope = std::make_unique<DpiScope>(name);
  return *scope;
}
void Runtime::add_context_root(const std::string &key,
                               const std::string &path) {
  if (key.empty() ||
      !context_roots.emplace(std::make_pair(key, path), &owned_scope(path))
           .second)
    throw std::runtime_error("duplicate native context frontend root");
}
DpiScope &Runtime::context_scope(const std::string &key,
                                 const std::string &caller, unsigned mode,
                                 const std::string &path) {
  if (mode == 2)
    return owned_scope(path);
  if (mode == 0)
    return owned_scope(caller + suffix(path));
  if (mode != 1)
    throw std::invalid_argument("invalid native context scope mode");
  std::string base;
  for (auto &[identity, scope] : context_roots) {
    const auto &[candidateKey, candidatePath] = identity;
    if (candidateKey == key &&
        (caller == candidatePath || caller.compare(0, candidatePath.size() + 1,
                                                   candidatePath + "/") == 0) &&
        candidatePath.size() > base.size())
      base = candidatePath;
  }
  if (base.empty())
    throw std::runtime_error(
        "no physical frontend root for context import in " + caller);
  return owned_scope(base + suffix(path));
}
namespace {
class ContextRoot : public Binding {
public:
  using Binding::Binding;
  void configure() override {
    runtime.add_context_root(
        text("ROOTKEY"), parent(runtime.scope_name() + "/" + instance_name));
  }
  void initialize() override {
    auto &scope =
        runtime.owned_scope(parent(runtime.scope_name() + "/" + instance_name));
    port("scope").set(uint64_t(reinterpret_cast<uintptr_t>(scope.handle())));
  }
};
class ContextScope : public Binding {
public:
  using Binding::Binding;
  void initialize() override {
    auto mode = text("MODE");
    if (mode != "local" && mode != "root" && mode != "global")
      throw std::invalid_argument("unknown native scope binding mode");
    auto &scope = runtime.context_scope(
        text("ROOTKEY"), parent(runtime.scope_name() + "/" + instance_name),
        mode == "local"  ? 0
        : mode == "root" ? 1
                         : 2,
        text("SCOPE"));
    port("scope").set(uint64_t(reinterpret_cast<uintptr_t>(scope.handle())));
  }
};
template <class T> void add(const std::string &name) {
  register_binding(name, [](Runtime &r, const std::string &n) {
    return std::make_unique<T>(r, n);
  });
}
} // namespace
void register_context_bindings() {
#define ARC_CONTEXT_MODEL(type, name) add<type>(name);
#include "models.def"
#undef ARC_CONTEXT_MODEL
}
} // namespace chipyard_arc
extern "C" void chipyard_arc_context_keep_root(uint64_t handle) {
  if (!svGetNameFromScope(reinterpret_cast<svScope>(uintptr_t(handle))))
    throw std::runtime_error("native context frontend root is not initialized");
}
extern "C" uint64_t chipyard_arc_context_resolve(const char *key,
                                                 const char *path,
                                                 uint64_t mode,
                                                 uint64_t reserved) {
  auto *runtime = chipyard_arc::active_runtime();
  auto *caller = svGetNameFromScope(svGetScope());
  if (!runtime || !caller || !key || !path || reserved)
    throw std::runtime_error("native context import lacks its SV call frame");
  auto &scope = runtime->context_scope(key, caller, unsigned(mode), path);
  return uint64_t(reinterpret_cast<uintptr_t>(scope.handle()));
}
