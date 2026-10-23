// Bound LLVM vectorization and instruction-selection work for packed HW state.
// Temporary optimization attributes are restored before machine-code lowering.
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
using namespace llvm;
namespace {
constexpr StringLiteral marker = "chipyard.temporary.vector.guard";
struct GuardWideVectors : RequiredPassInfoMixin<GuardWideVectors> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    // A repeated begin or colliding private attribute would lose the original
    // flags. Reject it before making any change.
    if (module.getNamedMetadata("chipyard.original.noimplicitfloat")) {
      module.getContext().emitError("vectorization guard already active");
      return PreservedAnalyses::all();
    }
    for (auto &function : module)
      if (function.hasFnAttribute(marker)) {
        module.getContext().emitError(
            "reserved native vectorization attribute");
        return PreservedAnalyses::all();
      }
    unsigned count = 0;
    auto *saved =
        module.getOrInsertNamedMetadata("chipyard.original.noimplicitfloat");
    for (auto &function : module) {
      if (function.hasFnAttribute(Attribute::NoImplicitFloat)) {
        saved->addOperand(MDNode::get(
            module.getContext(),
            MDString::get(module.getContext(), function.getName())));
        function.addFnAttr(marker, "original");
      }
      if (function.isDeclaration() ||
          function.hasFnAttribute(Attribute::NoImplicitFloat))
        continue;
      auto wide = [](Type *type) {
        return type->isIntegerTy() && type->getIntegerBitWidth() > 1024;
      };
      bool needed = wide(function.getReturnType());
      for (auto &arg : function.args())
        needed |= wide(arg.getType());
      for (auto &block : function)
        for (auto &op : block) {
          needed |= wide(op.getType());
          for (auto &operand : op.operands())
            needed |= wide(operand->getType());
        }
      if (!needed)
        continue;
      function.addFnAttr(marker);
      function.addFnAttr(Attribute::NoImplicitFloat);
      ++count;
    }
    errs() << "Temporarily guarded " << count
           << " wide-integer functions against vectorization\n";
    return PreservedAnalyses::none();
  }
};
struct RestoreVectorFlags : RequiredPassInfoMixin<RestoreVectorFlags> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    unsigned count = 0;
    auto *saved = module.getNamedMetadata("chipyard.original.noimplicitfloat");
    if (!saved) {
      module.getContext().emitError(
          "missing original vectorization attributes");
      return PreservedAnalyses::all();
    }
    StringSet<> original;
    for (auto *entry : saved->operands())
      original.insert(cast<MDString>(entry->getOperand(0))->getString());
    for (auto &function : module) {
      bool keep =
          original.contains(function.getName()) ||
          (function.hasFnAttribute(marker) &&
           function.getFnAttribute(marker).getValueAsString() == "original");
      if (!keep && function.hasFnAttribute(Attribute::NoImplicitFloat)) {
        function.removeFnAttr(Attribute::NoImplicitFloat);
        ++count;
      }
      function.removeFnAttr(marker);
    }
    module.eraseNamedMetadata(saved);
    errs() << "Restored target attributes for " << count << " functions\n";
    return PreservedAnalyses::none();
  }
};
struct BoundedBlocks : RequiredPassInfoMixin<BoundedBlocks> {
  unsigned cost(Instruction &op, const DataLayout &layout) {
    uint64_t result = 1;
    auto add = [&](Type *type) {
      if (!type->isSized())
        return;
      auto bits = layout.getTypeSizeInBits(type);
      if (!bits.isScalable())
        result += (bits.getFixedValue() + 63) / 64;
    };
    // Aggregate insert/extract instructions select SSA components; they do
    // not copy the entire container at each update. Charge the selected
    // payload while retaining full costs for actual aggregate loads/stores.
    if (auto *insert = dyn_cast<InsertValueInst>(&op))
      add(insert->getInsertedValueOperand()->getType());
    else if (isa<ExtractValueInst>(op))
      add(op.getType());
    else {
      add(op.getType());
      for (auto &value : op.operands())
        add(value->getType());
    }
    return unsigned(std::min<uint64_t>(result, 65536));
  }
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    unsigned blocks = 0;
    SmallVector<Constant *> addresses;
    for (auto &function : module) {
      if (function.isDeclaration() || function.hasFnAttribute(Attribute::Naked))
        continue;
      bool constrained = false;
      for (auto &b : function)
        for (auto &i : b)
          if (auto *call = dyn_cast<CallInst>(&i))
            constrained |= call->isMustTailCall() ||
                           call->getIntrinsicID() == Intrinsic::localescape;
      if (constrained)
        continue;
      SmallVector<BasicBlock *> original;
      for (auto &block : function)
        original.push_back(&block);
      for (auto *block : original) {
        if (block->isEHPad())
          continue;
        unsigned count = 0;
        for (auto it = block->begin(); it != block->end();) {
          Instruction *op = &*it++;
          if (isa<PHINode>(op) || op->isTerminator())
            continue;
          unsigned weight = cost(*op, module.getDataLayout());
          if (!count || count + weight <= 2048) {
            count += weight;
            continue;
          }
          // The new unconditional edge preserves instruction order and SSA
          // dominance. Functions with constrained entry/tail calls were
          // skipped.
          auto *next = block->splitBasicBlock(op->getIterator(),
                                              "chipyard.codegen.chunk");
          addresses.push_back(BlockAddress::get(&function, next));
          ++blocks;
          block = next;
          it = std::next(op->getIterator());
          count = weight;
        }
      }
    }
    if (!blocks)
      return PreservedAnalyses::all();
    // CodeGenPrepare must retain the boundaries until instruction selection.
    // These private, unreferenced address tables perform no runtime accesses;
    // machine block placement can still arrange the chunks as fallthroughs.
    auto *type = ArrayType::get(PointerType::getUnqual(module.getContext()),
                                addresses.size());
    auto *table = new GlobalVariable(
        module, type, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(type, addresses), "chipyard.codegen.boundaries");
    appendToCompilerUsed(module, {table});
    errs() << "Split " << blocks << " large LLVM basic blocks\n";
    return PreservedAnalyses::none();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardCodegen", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name == "chipyard-guard-wide-vectors")
                    manager.addPass(GuardWideVectors());
                  else if (name == "chipyard-restore-vector-flags")
                    manager.addPass(RestoreVectorFlags());
                  else if (name == "chipyard-bound-codegen-blocks")
                    manager.addPass(BoundedBlocks());
                  else
                    return false;
                  return true;
                });
          }};
}
