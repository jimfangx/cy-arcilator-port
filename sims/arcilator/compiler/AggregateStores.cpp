// Materialize single-use SSA aggregate update chains at their final store.
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace llvm;
namespace {
struct AggregateStores : RequiredPassInfoMixin<AggregateStores> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    SmallVector<StoreInst *> pending;
    for (auto &function : module) {
      if (function.hasFnAttribute(Attribute::Naked))
        continue;
      for (auto &block : function)
        for (auto &op : block)
          if (auto *store = dyn_cast<StoreInst>(&op))
            pending.push_back(store);
    }
    uint64_t stores = 0, updates = 0;
    for (unsigned cursor = 0; cursor < pending.size(); ++cursor) {
      auto *store = pending[cursor];
      if (store->isVolatile() || store->isAtomic())
        continue;
      auto *type = dyn_cast<StructType>(store->getValueOperand()->getType());
      // Homogeneous scalable-vector structures can be stored as a whole,
      // but LLVM forbids addressing their fields with getelementptr.
      if (!type || !type->isSized() || type->getNumElements() < 64 ||
          module.getDataLayout().getTypeAllocSize(type).isScalable())
        continue;
      SmallVector<InsertValueInst *> chain;
      Value *base = store->getValueOperand();
      while (auto *insert = dyn_cast<InsertValueInst>(base)) {
        if (!insert->hasOneUse())
          break;
        chain.push_back(insert);
        base = insert->getAggregateOperand();
      }
      if (chain.size() < 8)
        continue;
      // Every inserted value already dominates this store. Materialize only
      // here: moving writes earlier could expose partial state to a callback
      // or overwrite an aliased input before its original load.
      IRBuilder<> builder(store);
      builder.SetCurrentDebugLocation(store->getDebugLoc());
      builder.CreateAlignedStore(base, store->getPointerOperand(),
                                 store->getAlign());
      // The oldest update runs first, preserving repeated and overlapping
      // nested field assignments. No new memory access inherits whole-object
      // TBAA or range metadata that may be invalid for a selected field.
      for (auto *insert : llvm::reverse(chain)) {
        SmallVector<Value *> indices;
        indices.push_back(builder.getInt32(0));
        Type *selected = type;
        for (unsigned index : insert->getIndices()) {
          if (auto *structure = dyn_cast<StructType>(selected)) {
            indices.push_back(builder.getInt32(index));
            selected = structure->getElementType(index);
          } else {
            // Array indices in insertvalue are unsigned; a 32-bit GEP index
            // above INT32_MAX would otherwise become a negative offset.
            indices.push_back(builder.getInt64(index));
            selected = cast<ArrayType>(selected)->getElementType();
          }
        }
        auto *pointer =
            builder.CreateGEP(type, store->getPointerOperand(), indices);
        auto *field = builder.CreateAlignedStore(
            insert->getInsertedValueOperand(), pointer, Align(1));
        // A selected field can itself contain an aggregate construction.
        pending.push_back(field);
      }
      store->eraseFromParent();
      for (auto *insert : chain) {
        assert(insert->use_empty());
        insert->eraseFromParent();
      }
      ++stores;
      updates += chain.size();
    }
    if (!stores)
      return PreservedAnalyses::all();
    errs() << "Materialized " << updates << " aggregate field updates at "
           << stores << " original stores\n";
    return PreservedAnalyses::none();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardAggregateStores", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-materialize-aggregate-stores")
                    return false;
                  manager.addPass(AggregateStores());
                  return true;
                });
          }};
}
