// Source call frames retained while functions and tasks are inlined.
#ifndef CHIPYARD_ARC_SCOPE_FRAMES_H
#define CHIPYARD_ARC_SCOPE_FRAMES_H
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include <string>
namespace chipyard {
// An escaped SV identifier can contain dots and colons. Each offset is the end
// of one whole scope component, including any terminating escape whitespace.
inline llvm::SmallVector<size_t> scopeEnds(llvm::StringRef path) {
  llvm::SmallVector<size_t> ends;
  bool escaped = false;
  for (size_t i = 0; i < path.size(); ++i) {
    if (!escaped && path[i] == '\\')
      escaped = true;
    else if (escaped && llvm::isSpace(path[i]))
      escaped = false;
    else if (!escaped && (path[i] == '.' || path.substr(i).starts_with("::"))) {
      ends.push_back(i);
      if (path[i] == ':')
        ++i;
    }
  }
  if (!path.empty())
    ends.push_back(path.size());
  return ends;
}
inline bool scopePrefix(llvm::StringRef path, llvm::StringRef base) {
  return base.empty() ||
         (path.consume_front(base) &&
          (path.empty() || path.starts_with(".") || path.starts_with("::")));
}
inline mlir::FailureOr<std::string>
rebaseScope(llvm::StringRef path, llvm::StringRef from, llvm::StringRef to) {
  if (scopePrefix(path, from))
    return to.str() + path.drop_front(from.size()).str();
  auto source = scopeEnds(from), target = scopeEnds(to),
       value = scopeEnds(path);
  size_t common = 0;
  while (common < source.size() && common < value.size() &&
         from.take_front(source[common]) == path.take_front(value[common]))
    ++common;
  size_t upward = source.size() - common;
  if (!common || upward > target.size())
    return mlir::failure();
  size_t keep = target.size() - upward;
  return to.take_front(keep ? target[keep - 1] : 0).str() +
         path.drop_front(value[common - 1]).str();
}
// Frames are stored from the innermost call to the outermost. Copying an
// already-inlined body and inlining a previously copied nested call therefore
// produce the same sequence, independently of the inliner's traversal order.
inline void
carryScopeFrames(mlir::Operation *call,
                 llvm::iterator_range<mlir::Region::iterator> blocks) {
  if (auto unresolved = call->getAttr("chipyard.sv_scope_unresolved"))
    for (auto &block : blocks)
      for (auto &op : block)
        op.walk([&](mlir::Operation *nested) {
          if (nested->hasAttr("chipyard.sv_scope_call") ||
              nested->getName().getStringRef() == "sim.fmt.hier_path" ||
              nested->getName().getStringRef() == "sim.func.dpi.call")
            nested->setAttr("chipyard.sv_scope_unresolved", unresolved);
        });
  llvm::SmallVector<mlir::Attribute> extension;
  if (auto frame = call->getAttr("chipyard.sv_scope_call"))
    extension.push_back(frame);
  if (auto ancestors =
          call->getAttrOfType<mlir::ArrayAttr>("chipyard.sv_scope_ancestors"))
    llvm::append_range(extension, ancestors);
  if (extension.empty())
    return;
  for (auto &block : blocks)
    for (auto &op : block)
      op.walk([&](mlir::Operation *nested) {
        auto key =
            nested->getName().getStringRef() == "sim.fmt.hier_path" ||
                    nested->getName().getStringRef() == "sim.func.dpi.call"
                ? "chipyard.sv_scope_frames"
                : "chipyard.sv_scope_ancestors";
        if (llvm::StringRef(key) == "chipyard.sv_scope_ancestors" &&
            !nested->hasAttr("chipyard.sv_scope_call"))
          return;
        llvm::SmallVector<mlir::Attribute> frames;
        if (auto old = nested->getAttrOfType<mlir::ArrayAttr>(key))
          llvm::append_range(frames, old);
        llvm::append_range(frames, extension);
        nested->setAttr(key, mlir::ArrayAttr::get(call->getContext(), frames));
      });
}
inline mlir::FailureOr<std::string>
resolveScopeFrames(mlir::Operation *op, llvm::StringRef lexical,
                   llvm::StringRef canonical, llvm::StringRef physical,
                   bool includeCall = false) {
  llvm::SmallVector<mlir::Attribute> frames;
  if (includeCall)
    if (auto frame = op->getAttr("chipyard.sv_scope_call"))
      frames.push_back(frame);
  if (auto stored =
          op->getAttrOfType<mlir::ArrayAttr>("chipyard.sv_scope_frames"))
    llvm::append_range(frames, stored);
  std::string from = canonical.str(), to = physical.str();
  for (auto attr : llvm::reverse(frames)) {
    auto frame = mlir::dyn_cast<mlir::DictionaryAttr>(attr);
    auto caller =
        frame ? frame.getAs<mlir::StringAttr>("caller") : mlir::StringAttr{};
    auto anchor =
        frame ? frame.getAs<mlir::StringAttr>("anchor") : mlir::StringAttr{};
    auto callee =
        frame ? frame.getAs<mlir::StringAttr>("callee") : mlir::StringAttr{};
    if (!caller || !anchor || !callee ||
        !scopePrefix(caller.getValue(), from) ||
        !scopePrefix(callee.getValue(), anchor.getValue()))
      return op->emitError("invalid source call frame provenance"),
             mlir::failure();
    auto actualCaller = rebaseScope(caller, from, to);
    if (mlir::failed(actualCaller))
      return op->emitError("cannot resolve physical call frame"),
             mlir::failure();
    // An empty anchor denotes a name selected from the fixed elaboration root.
    auto actualAnchor = anchor.getValue().empty()
                            ? mlir::FailureOr<std::string>(std::string())
                            : rebaseScope(anchor, caller, *actualCaller);
    if (mlir::failed(actualAnchor))
      return op->emitError("cannot resolve source lookup anchor"),
             mlir::failure();
    to = *actualAnchor +
         callee.getValue().drop_front(anchor.getValue().size()).str();
    from = callee.str();
  }
  auto result = rebaseScope(lexical, from, to);
  if (mlir::failed(result))
    op->emitError("source scope is outside its physical call frame");
  return result;
}
} // namespace chipyard
#endif
