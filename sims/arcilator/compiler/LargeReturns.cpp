// Keep large private LLVM values out of the machine calling convention.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
namespace {
struct Result {
  ReturnInst *ret;
  SmallVector<Value *> fields;
};
struct Plan {
  Function *function;
  Type *type;
  bool flatten = false;
  SmallVector<CallInst *> calls;
  SmallVector<Result> returns;
};
struct LargeReturns : RequiredPassInfoMixin<LargeReturns> {
  // Every result is loaded at the call site before another call can reuse this
  // storage. It is one scratch allocation per caller, not one per call.
  DenseMap<Function *, AllocaInst *> scratch;
  DenseMap<Function *, uint64_t> sizes;
  bool analyze(Function &function, const DataLayout &dl, Plan &plan) {
    auto *type = function.getReturnType();
    if (!type->isSized() || function.isDeclaration() ||
        !function.hasInternalLinkage() || function.isVarArg() ||
        function.getSubprogram() || function.hasFnAttribute(Attribute::Naked) ||
        function.hasFnAttribute(Attribute::AlwaysInline))
      return false;
    if (function.getCallingConv() != CallingConv::C &&
        function.getCallingConv() != CallingConv::Fast &&
        function.getCallingConv() != CallingConv::Cold)
      return false;
    auto size = dl.getTypeAllocSize(type);
    if (size.isScalable() || !size.getFixedValue())
      return false;
    auto *structure = dyn_cast<StructType>(type);
    if (size.getFixedValue() < 512 &&
        (!structure || structure->getNumElements() < 64))
      return false;
    // Appending a destination must not violate a constrained machine ABI.
    for (unsigned i = 0; i < function.arg_size(); ++i)
      for (auto kind : {Attribute::ByVal, Attribute::ByRef, Attribute::StructRet,
                        Attribute::InAlloca, Attribute::Preallocated,
                        Attribute::Nest, Attribute::SwiftSelf,
                        Attribute::SwiftError, Attribute::InReg})
        if (function.getAttributes().hasParamAttr(i, kind))
          return false;
    // A musttail call constrains the caller signature as well as the callee.
    for (auto &block : function)
      for (auto &instruction : block)
        if (auto *call = dyn_cast<CallInst>(&instruction))
          if (call->isMustTailCall())
            return false;
    plan.function = &function;
    plan.type = type;
    plan.flatten = structure != nullptr;
    for (auto &use : function.uses()) {
      auto *call = dyn_cast<CallInst>(use.getUser());
      // A direct call can also pass this function's address as an argument.
      // Only the callee operand is eligible for the private ABI rewrite.
      if (!call || !call->isCallee(&use) || call->isMustTailCall() ||
          call->hasOperandBundles() ||
          call->getCallingConv() != function.getCallingConv() ||
          call->getFunctionType() != function.getFunctionType())
        return false;
      for (auto *resultUser : call->users())
        if (!isa<ExtractValueInst>(resultUser))
          plan.flatten = false;
      for (unsigned i = 0; i < call->arg_size(); ++i)
        for (auto kind : {Attribute::ByVal, Attribute::ByRef, Attribute::StructRet,
                          Attribute::InAlloca, Attribute::Preallocated,
                          Attribute::Nest, Attribute::SwiftSelf,
                          Attribute::SwiftError, Attribute::InReg})
          if (call->getAttributes().hasParamAttr(i, kind))
            return false;
      plan.calls.push_back(call);
    }
    for (auto &block : function) {
      auto *ret = dyn_cast<ReturnInst>(block.getTerminator());
      if (!ret)
        continue;
      Result result{ret, {}};
      if (!plan.flatten) {
        plan.returns.push_back(std::move(result));
        continue;
      }
      result.fields.resize(structure->getNumElements(), nullptr);
      Value *base = ret->getReturnValue();
      while (auto *insert = dyn_cast<InsertValueInst>(base)) {
        auto indices = insert->getIndices();
        if (indices.size() != 1) {
          plan.flatten = false;
          break;
        }
        unsigned i = indices.front();
        if (!result.fields[i])
          result.fields[i] = insert->getInsertedValueOperand();
        base = insert->getAggregateOperand();
      }
      for (unsigned i = 0; i < result.fields.size(); ++i)
        if (!result.fields[i]) {
          auto *constant = dyn_cast<Constant>(base);
          if (!constant ||
              !(result.fields[i] = constant->getAggregateElement(i)))
            plan.flatten = false;
        }
      plan.returns.push_back(std::move(result));
    }
    return !plan.returns.empty();
  }
  AttributeList attributes(LLVMContext &context, AttributeList original,
                           unsigned oldArguments) {
    AttrBuilder fn(context, original.getFnAttrs());
    fn.removeAttribute(Attribute::Memory);
    fn.removeAttribute(Attribute::AlwaysInline);
    fn.addAttribute(Attribute::NoInline);
    SmallVector<AttributeSet> params;
    for (unsigned i = 0; i < oldArguments; ++i) {
      AttrBuilder param(context, original.getParamAttrs(i));
      param.removeAttribute(Attribute::Returned);
      params.push_back(AttributeSet::get(context, param));
    }
    params.push_back(AttributeSet{});
    return AttributeList::get(context, AttributeSet::get(context, fn),
                              AttributeSet{}, params);
  }
  Value *buffer(Function *caller, uint64_t bytes, Align alignment,
                const DataLayout &dl) {
    auto *allocation = scratch.lookup(caller);
    if (!allocation) {
      IRBuilder<> builder(&*caller->getEntryBlock().getFirstInsertionPt());
      allocation = builder.CreateAlloca(
          ArrayType::get(builder.getInt8Ty(), bytes), dl.getAllocaAddrSpace(),
          nullptr, "chipyard.return_buffer");
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
    auto &dl = module.getDataLayout();
    auto *structure = dyn_cast<StructType>(plan.type);
    auto *layout = plan.flatten ? dl.getStructLayout(structure) : nullptr;
    auto alignment = dl.getABITypeAlign(plan.type);
    auto bytes = dl.getTypeAllocSize(plan.type).getFixedValue();
    SmallVector<Type *> arguments(old.getFunctionType()->params());
    arguments.push_back(PointerType::get(context, dl.getAllocaAddrSpace()));
    auto *type = FunctionType::get(Type::getVoidTy(context), arguments, false);
    auto *replacement =
        Function::Create(type, old.getLinkage(), old.getAddressSpace(),
                         old.getName() + ".buffered", &module);
    replacement->copyAttributesFrom(&old);
    replacement->setAttributes(
        attributes(context, old.getAttributes(), old.arg_size()));
    replacement->setCallingConv(old.getCallingConv());
    DenseMap<Value *, Value *> mapped;
    for (unsigned i = 0; i < old.arg_size(); ++i) {
      auto *before = old.getArg(i), *after = replacement->getArg(i);
      after->setName(before->getName());
      mapped[before] = after;
      before->replaceAllUsesWith(after);
    }
    auto *destination = replacement->getArg(old.arg_size());
    destination->setName("chipyard.results");
    replacement->splice(replacement->begin(), &old);
    if (auto *previous = scratch.lookup(&old)) {
      scratch[replacement] = previous;
      sizes[replacement] = sizes.lookup(&old);
      scratch.erase(&old);
      sizes.erase(&old);
    }
    SmallVector<WeakTrackingVH> unused;
    for (auto &result : plan.returns) {
      IRBuilder<> builder(result.ret);
      builder.SetCurrentDebugLocation(result.ret->getDebugLoc());
      unused.push_back(result.ret->getReturnValue());
      if (!plan.flatten) {
        builder.CreateAlignedStore(result.ret->getReturnValue(), destination,
                                   alignment);
        builder.CreateRetVoid();
        result.ret->eraseFromParent();
        continue;
      }
      for (unsigned i = 0; i < result.fields.size(); ++i) {
        Value *field = result.fields[i];
        if (auto found = mapped.find(field); found != mapped.end())
          field = found->second;
        uint64_t offset = layout->getElementOffset(i);
        auto *address = builder.CreateInBoundsGEP(
            builder.getInt8Ty(), destination, builder.getInt64(offset));
        builder.CreateAlignedStore(field, address,
                                   commonAlignment(alignment, offset));
      }
      builder.CreateRetVoid();
      result.ret->eraseFromParent();
    }
    for (auto *call : plan.calls) {
      auto *storage = buffer(call->getFunction(), bytes, alignment, dl);
      IRBuilder<> builder(call);
      builder.SetCurrentDebugLocation(call->getDebugLoc());
      SmallVector<Value *> args(call->args());
      args.push_back(storage);
      SmallVector<OperandBundleDef> bundles;
      call->getOperandBundlesAsDefs(bundles);
      auto *next = builder.CreateCall(replacement, args, bundles);
      next->setCallingConv(call->getCallingConv());
      next->setAttributes(
          attributes(context, call->getAttributes(), call->arg_size()));
      if (!plan.flatten) {
        // Capture the complete result before another call reuses the scratch.
        auto *value = builder.CreateAlignedLoad(plan.type, storage, alignment);
        call->replaceAllUsesWith(value);
        call->eraseFromParent();
        continue;
      }
      SmallVector<ExtractValueInst *> extracts;
      for (auto *user : call->users())
        extracts.push_back(cast<ExtractValueInst>(user));
      DenseMap<unsigned, Value *> loaded;
      // Materialize at the call, even when the old extracts occur much later.
      // No later call or aliased write may replace the captured return value.
      for (auto *extract : extracts) {
        auto indices = extract->getIndices();
        unsigned i = indices.front();
        Value *value = loaded.lookup(i);
        if (!value) {
          auto offset = layout->getElementOffset(i);
          auto *address = builder.CreateInBoundsGEP(
              builder.getInt8Ty(), storage, builder.getInt64(offset));
          value =
              builder.CreateAlignedLoad(structure->getElementType(i), address,
                                        commonAlignment(alignment, offset));
          loaded[i] = value;
        }
        if (indices.size() != 1)
          value = builder.CreateExtractValue(value, indices.drop_front());
        extract->replaceAllUsesWith(value);
        extract->eraseFromParent();
      }
      call->eraseFromParent();
    }
    auto name = old.getName().str();
    old.eraseFromParent();
    replacement->setName(name);
    for (auto &value : unused)
      if (auto *instruction = dyn_cast_or_null<Instruction>(value))
        RecursivelyDeleteTriviallyDeadInstructions(instruction);
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
      errs() << "Buffered " << changed << " large private LLVM returns\n";
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardLargeReturns", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-buffer-large-returns")
                    return false;
                  manager.addPass(LargeReturns());
                  return true;
                });
          }};
}
