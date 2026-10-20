// Keep large private helper parameter lists out of the machine ABI.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Alignment.h"

using namespace llvm;
namespace {
struct Plan {
  Function *function;
  StructType *type;
  SmallVector<CallInst *> calls;
};
struct LargeArguments : RequiredPassInfoMixin<LargeArguments> {
  DenseMap<Function *, AllocaInst *> scratch;
  DenseMap<Function *, uint64_t> sizes;

  bool abiParameter(AttributeSet attributes) {
    for (auto kind :
         {Attribute::ByVal, Attribute::ByRef, Attribute::StructRet,
          Attribute::InAlloca, Attribute::Preallocated, Attribute::Nest,
          Attribute::SwiftSelf, Attribute::SwiftError, Attribute::InReg})
      if (attributes.hasAttribute(kind))
        return true;
    return false;
  }
  bool analyze(Function &function, const DataLayout &layout, Plan &plan) {
    if (function.isDeclaration() || !function.hasInternalLinkage() ||
        function.isVarArg() ||
        function.getSubprogram() || function.hasFnAttribute(Attribute::Naked) ||
        function.hasFnAttribute(Attribute::AlwaysInline) ||
        function.hasFnAttribute(Attribute::AllocSize))
      return false;
    if (function.getCallingConv() != CallingConv::C &&
        function.getCallingConv() != CallingConv::Fast &&
        function.getCallingConv() != CallingConv::Cold)
      return false;
    // A musttail call also constrains its caller's signature.
    for (auto &block : function)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction))
          if (call->isMustTailCall())
            return false;
    SmallVector<Type *> fields;
    for (auto &arg : function.args()) {
      if (abiParameter(
              function.getAttributes().getParamAttrs(arg.getArgNo())) ||
          !arg.getType()->isSized() ||
          layout.getTypeAllocSize(arg.getType()).isScalable())
        return false;
      fields.push_back(arg.getType());
    }
    plan.type = StructType::get(function.getContext(), fields);
    auto bytes = layout.getTypeAllocSize(plan.type).getFixedValue();
    if (!bytes || (function.arg_size() < 64 && bytes < 512))
      return false;
    plan.function = &function;
    for (auto &use : function.uses()) {
      auto *call = dyn_cast<CallInst>(use.getUser());
      if (!call || !call->isCallee(&use) || call->isMustTailCall() ||
          call->hasOperandBundles() ||
          call->getFunctionType() != function.getFunctionType() ||
          call->getCallingConv() != function.getCallingConv())
        return false;
      for (unsigned i = 0; i < call->arg_size(); ++i)
        if (abiParameter(call->getAttributes().getParamAttrs(i)))
          return false;
      plan.calls.push_back(call);
    }
    return true;
  }
  AttributeList attributes(LLVMContext &context, AttributeList original) {
    AttrBuilder fn(context, original.getFnAttrs());
    fn.removeAttribute(Attribute::Memory);
    fn.removeAttribute(Attribute::AlwaysInline);
    fn.removeAttribute(Attribute::AllocSize);
    fn.addAttribute(Attribute::NoInline);
    return AttributeList::get(context, AttributeSet::get(context, fn),
                              original.getRetAttrs(), {AttributeSet{}});
  }
  AllocaInst *buffer(Function *caller, uint64_t bytes, Align alignment,
                     const DataLayout &layout) {
    auto *allocation = scratch.lookup(caller);
    if (!allocation) {
      IRBuilder<> builder(&*caller->getEntryBlock().getFirstInsertionPt());
      allocation = builder.CreateAlloca(
          ArrayType::get(builder.getInt8Ty(), bytes),
          layout.getAllocaAddrSpace(), nullptr, "chipyard.argument_buffer");
      allocation->setAlignment(alignment);
      scratch[caller] = allocation;
      sizes[caller] = bytes;
    } else {
      if (bytes > sizes[caller]) {
        allocation->setAllocatedType(
            ArrayType::get(Type::getInt8Ty(caller->getContext()), bytes));
        sizes[caller] = bytes;
      }
      allocation->setAlignment(std::max(allocation->getAlign(), alignment));
    }
    return allocation;
  }
  void rewrite(Plan &plan, Module &module) {
    auto &old = *plan.function;
    auto &context = module.getContext();
    auto &layout = module.getDataLayout();
    auto *structure = layout.getStructLayout(plan.type);
    auto alignment = layout.getABITypeAlign(plan.type);
    auto bytes = layout.getTypeAllocSize(plan.type).getFixedValue();
    auto *pointer = PointerType::get(context, layout.getAllocaAddrSpace());
    auto *type = FunctionType::get(old.getReturnType(), {pointer}, false);
    auto *replacement =
        Function::Create(type, old.getLinkage(), old.getAddressSpace(),
                         old.getName() + ".arguments", &module);
    replacement->copyAttributesFrom(&old);
    replacement->setCallingConv(old.getCallingConv());
    replacement->setAttributes(attributes(context, old.getAttributes()));
    replacement->getArg(0)->setName("chipyard.arguments");
    replacement->splice(replacement->begin(), &old);
    if (auto *previous = scratch.lookup(&old)) {
      scratch[replacement] = previous;
      sizes[replacement] = sizes.lookup(&old);
      scratch.erase(&old);
      sizes.erase(&old);
    }
    IRBuilder<> entry(&*replacement->getEntryBlock().getFirstInsertionPt());
    for (auto &arg : old.args()) {
      auto offset = structure->getElementOffset(arg.getArgNo());
      auto *address = entry.CreateInBoundsGEP(
          entry.getInt8Ty(), replacement->getArg(0), entry.getInt64(offset));
      auto *value = entry.CreateAlignedLoad(arg.getType(), address,
                                            commonAlignment(alignment, offset));
      value->takeName(&arg);
      arg.replaceAllUsesWith(value);
    }
    for (auto *call : plan.calls) {
      auto *storage = buffer(call->getFunction(), bytes, alignment, layout);
      IRBuilder<> builder(call);
      builder.SetCurrentDebugLocation(call->getDebugLoc());
      for (unsigned i = 0; i < call->arg_size(); ++i) {
        auto offset = structure->getElementOffset(i);
        auto *address = builder.CreateInBoundsGEP(builder.getInt8Ty(), storage,
                                                  builder.getInt64(offset));
        builder.CreateAlignedStore(call->getArgOperand(i), address,
                                   commonAlignment(alignment, offset));
      }
      auto *next = builder.CreateCall(replacement, {storage});
      next->setCallingConv(call->getCallingConv());
      next->setAttributes(attributes(context, call->getAttributes()));
      call->replaceAllUsesWith(next);
      call->eraseFromParent();
    }
    auto name = old.getName().str();
    old.eraseFromParent();
    replacement->setName(name);
  }
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    if (module.getDataLayoutStr().empty())
      return PreservedAnalyses::all();
    SmallVector<Function *> functions;
    for (auto &function : module)
      functions.push_back(&function);
    unsigned changed = 0;
    for (auto *function : functions) {
      Plan plan;
      if (!analyze(*function, module.getDataLayout(), plan))
        continue;
      rewrite(plan, module);
      ++changed;
    }
    if (changed)
      errs() << "Buffered " << changed
             << " large private LLVM argument lists\n";
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardLargeArguments", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-buffer-large-arguments")
                    return false;
                  manager.addPass(LargeArguments());
                  return true;
                });
          }};
}
