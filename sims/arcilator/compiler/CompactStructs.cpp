// Share large literal tuple types before LLVM text serialization.
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/DenseSet.h"
using namespace mlir;
namespace {
struct CompactStructs : PassWrapper<CompactStructs, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CompactStructs)
  StringRef getArgument() const override {
    return "chipyard-compact-llvm-structs";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<LLVM::LLVMDialect>();
  }
  void runOnOperation() override {
    // Existing identified types have immutable bodies. Retain every literal
    // reachable through them at all of its uses, including extractvalue
    // results, rather than creating two inconsistent versions of that type.
    llvm::DenseSet<Type> visited, retained;
    std::function<void(Type)> protect = [&](Type type) {
      if (!visited.insert(type).second)
        return;
      if (auto tuple = dyn_cast<LLVM::LLVMStructType>(type)) {
        if (!tuple.isIdentified())
          retained.insert(type);
        if (tuple.isOpaque())
          return;
        for (auto field : tuple.getBody())
          protect(field);
        return;
      }
      type.walkImmediateSubElements([](Attribute) {}, protect);
    };
    AttrTypeWalker walker;
    walker.addWalk([&](LLVM::LLVMStructType type) {
      if (type.isIdentified())
        protect(type);
    });
    getOperation().walk([&](Operation *op) {
      walker.walk(op->getAttrDictionary());
      for (auto type : op->getResultTypes())
        walker.walk(type);
      for (auto &region : op->getRegions())
        for (auto &block : region)
          for (auto argument : block.getArguments())
            walker.walk(argument.getType());
    });
    AttrTypeReplacer replacer;
    unsigned changed = 0;
    replacer.addReplacement([&](LLVM::LLVMStructType type)
                                -> std::optional<std::pair<Type, WalkResult>> {
      if (type.isIdentified() || retained.contains(type))
        return std::make_pair(Type(type), WalkResult::skip());
      if (type.getBody().size() < 64)
        return std::nullopt;
      SmallVector<Type> fields;
      for (auto field : type.getBody())
        fields.push_back(replacer.replace(field));
      auto named = LLVM::LLVMStructType::getNewIdentified(
          &getContext(), "chipyard.tuple", fields, type.isPacked());
      ++changed;
      return std::make_pair(Type(named), WalkResult::skip());
    });
    replacer.recursivelyReplaceElementsIn(getOperation(), true, false, true);
    llvm::errs() << "Named " << changed << " large literal LLVM structures\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardCompactStructs",
          LLVM_VERSION_STRING, [] { PassRegistration<CompactStructs>(); }};
}
