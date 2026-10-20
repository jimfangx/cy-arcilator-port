#include "runtime.h"
#include "svdpi.h"
#include <stdexcept>
#include <string_view>
namespace {
// User data belongs to this Runtime's owned scope and is erased by its
// destruction. No pointer-keyed global flag may survive model recreation or
// be confused by another live model with the same public hierarchy names.
char exportFinishKey;
svScope exportFinishScope(chipyard_arc::Runtime &runtime) {
  return runtime.owned_scope(runtime.scope_name() + "/$native_export_finish")
      .handle();
}
} // namespace

extern "C" void chipyard_arc_export_finish() {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime || svPutUserData(exportFinishScope(*runtime), &exportFinishKey,
                                &exportFinishKey))
    throw std::runtime_error("native export finish lacks its model");
  runtime->finish();
}

extern "C" uint64_t chipyard_arc_export_finished() {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime)
    throw std::runtime_error("native task completion lacks its model");
  return svGetUserData(exportFinishScope(*runtime), &exportFinishKey) !=
         nullptr;
}
// Exported SV bodies run outside the hardware root's lexical SSA scope. Query
// this model's current controls without copying caller-dependent reset state
// into a shared helper or changing the original DPI C signature.
extern "C" uint64_t chipyard_arc_runtime_control(uint64_t which) {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime || which > 1)
    throw std::runtime_error("invalid native export runtime control query");
  return runtime->signal(which ? "arc_stop_enable" : "arc_printf_enable")
             .get() != 0;
}

// Physical femtoseconds are independent of the selected SV declaration's
// timeunit. The compiled SV conversion supplies that lexical scale.
extern "C" uint64_t chipyard_arc_export_time() {
  auto *runtime = chipyard_arc::active_runtime();
  if (!runtime)
    throw std::runtime_error("native exported time lacks its model");
  return runtime->time_fs();
}

// Exported bodies are shared by all physical instances of a canonical SV
// module. Select the target calculated by the compiler for the actual selected
// export scope. Resolve through the model's frontend roots, including multiple
// copies of the same imported design; never use the process-global name lookup
// to choose an instance belonging to another live model.
extern "C" uint64_t chipyard_arc_export_scope_lookup(const char *key,
                                                     const char *table,
                                                     uint64_t mode,
                                                     uint64_t reserved) {
  auto *runtime = chipyard_arc::active_runtime();
  auto current = svGetScope();
  const char *name = svGetNameFromScope(current);
  if (!runtime || !name || !key || !table || mode || reserved)
    throw std::runtime_error("native exported context lacks its SV frame");
  std::string_view remaining(table);
  while (!remaining.empty()) {
    auto end = remaining.find('\n');
    if (end == std::string_view::npos)
      throw std::runtime_error("unterminated native exported scope row");
    auto row = remaining.substr(0, end);
    remaining.remove_prefix(end + 1);
    auto split = row.find('\t');
    if (split == std::string_view::npos || row.substr(split, 3) != "\t1\t")
      throw std::runtime_error("invalid native exported scope row");
    auto caller = std::string(row.substr(0, split));
    auto target = std::string(row.substr(split + 3));
    auto &candidate = runtime->context_scope(key, name, 1, caller);
    if (candidate.handle() == current) {
      auto &scope = runtime->context_scope(key, name, 1, target);
      return uint64_t(reinterpret_cast<uintptr_t>(scope.handle()));
    }
  }
  throw std::runtime_error(
      "native exported context has no physical call frame");
}
