#include "compat.h"
#include "svdpi.h"
#include "vpi_user.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
namespace {
int saved_argc = 0;
char **saved_argv = nullptr;
struct ScopeRegistry {
  std::mutex mutex;
  std::set<svScope> handles;
  std::map<std::string, std::vector<svScope>> names;
  std::map<std::pair<svScope, void *>, void *> user_data;
};
ScopeRegistry &scopes() {
  static ScopeRegistry registry;
  return registry;
}
thread_local svScope current_scope = nullptr;
} // namespace
chipyard_arc::DpiScope::DpiScope(std::string name)
    : full_name(std::move(name)) {
  if (full_name.empty())
    throw std::invalid_argument("empty native DPI scope");
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.handles.insert(handle());
  registry.names[full_name].push_back(handle());
}
chipyard_arc::DpiScope::~DpiScope() {
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.handles.erase(handle());
  for (auto it = registry.user_data.begin(); it != registry.user_data.end();) {
    if (it->first.first == handle())
      it = registry.user_data.erase(it);
    else
      ++it;
  }
  auto found = registry.names.find(full_name);
  auto &entries = found->second;
  entries.erase(std::find(entries.begin(), entries.end(), handle()));
  if (entries.empty())
    registry.names.erase(found);
  if (current_scope == handle())
    current_scope = nullptr;
}
chipyard_arc::DpiContext::DpiContext(DpiScope &scope)
    : previous(std::exchange(current_scope, scope.handle())) {}
chipyard_arc::DpiContext::~DpiContext() { current_scope = previous; }
void chipyard_arc::command_args(int argc, char **argv) {
  saved_argc = argc;
  saved_argv = argv;
}
extern "C" {
svScope svGetScope() { return current_scope; }
svScope svSetScope(const svScope scope) {
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (scope && !registry.handles.count(scope))
    throw std::invalid_argument("unknown native DPI scope");
  return std::exchange(current_scope, scope);
}
const char *svGetNameFromScope(const svScope scope) {
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (!registry.handles.count(scope))
    return nullptr;
  return static_cast<chipyard_arc::DpiScope *>(scope)->name().c_str();
}
svScope svGetScopeFromName(const char *name) {
  if (!name)
    return nullptr;
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto found = registry.names.find(name);
  return found == registry.names.end() ? nullptr : found->second.back();
}
int svPutUserData(const svScope scope, void *key, void *data) {
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (!data || !registry.handles.count(scope))
    return -1;
  registry.user_data[{scope, key}] = data;
  return 0;
}
void *svGetUserData(const svScope scope, void *key) {
  auto &registry = scopes();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (!registry.handles.count(scope))
    return nullptr;
  auto found = registry.user_data.find({scope, key});
  return found == registry.user_data.end() ? nullptr : found->second;
}
int svGetCallerInfo(const char **, int *) { return 0; }
PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info info) {
  if (!info || !saved_argv)
    return 0;
  static char product[] = "Chipyard Arcilator native runtime";
  static char version[] = "1";
  *info = {saved_argc, saved_argv, product, version};
  return 1;
}
}
