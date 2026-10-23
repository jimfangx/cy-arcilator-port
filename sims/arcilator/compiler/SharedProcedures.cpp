// Forward private procedure values through one invocation-local workspace.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Transforms/Utils/Local.h"
#include <optional>
using namespace llvm;
namespace {
struct Slot {
  uint64_t offset;
  Type *type;
};
struct Procedure {
  Function *function;
  CallInst *call;
  Slot result;
  SmallVector<Slot> arguments;
};
struct SharedProcedures : RequiredPassInfoMixin<SharedProcedures> {
  static constexpr uint64_t maxWorkspace = 1024 * 1024;
  DenseMap<Function *, uint64_t> workspaceBytes;
  bool parameterABI(AttributeSet attrs) {
    for (auto kind :
         {Attribute::ByVal, Attribute::ByRef, Attribute::StructRet,
          Attribute::InAlloca, Attribute::Preallocated, Attribute::Nest,
          Attribute::SwiftSelf, Attribute::SwiftError, Attribute::InReg})
      if (attrs.hasAttribute(kind))
        return true;
    return false;
  }
  bool sized(Type *type, const DataLayout &layout) {
    return type->isSized() && !layout.getTypeAllocSize(type).isScalable() &&
           layout.getTypeAllocSize(type).getFixedValue() != 0;
  }
  CallInst *eligible(Function &fn, const DataLayout &layout) {
    if (fn.isDeclaration() || !fn.hasInternalLinkage() || fn.isVarArg() ||
        fn.getSubprogram() || fn.hasGC() ||
        !fn.hasFnAttribute(Attribute::NoInline) ||
        fn.hasFnAttribute(Attribute::Naked) ||
        fn.hasFnAttribute(Attribute::AlwaysInline) ||
        fn.hasFnAttribute(Attribute::NoIPA) ||
        fn.hasFnAttribute(Attribute::OptimizeNone) ||
        fn.hasFnAttribute(Attribute::PresplitCoroutine) ||
        fn.hasFnAttribute(Attribute::AllocSize) || !fn.hasOneUse() ||
        (!fn.getReturnType()->isVoidTy() && !sized(fn.getReturnType(), layout)))
      return nullptr;
    if (fn.getCallingConv() != CallingConv::C &&
        fn.getCallingConv() != CallingConv::Fast &&
        fn.getCallingConv() != CallingConv::Cold)
      return nullptr;
    auto &use = *fn.use_begin();
    auto *call = dyn_cast<CallInst>(use.getUser());
    if (!call || !call->isCallee(&use) || call->isMustTailCall() ||
        call->hasOperandBundles() || call->getFunction() == &fn ||
        call->getFunction()->hasGC() ||
        call->getFunction()->hasFnAttribute(Attribute::Naked) ||
        call->getFunction()->hasFnAttribute(Attribute::NoIPA) ||
        call->getFunction()->hasFnAttribute(Attribute::PresplitCoroutine) ||
        call->getFunctionType() != fn.getFunctionType() ||
        call->getCallingConv() != fn.getCallingConv() ||
        call->hasFnAttr(Attribute::AllocSize))
      return nullptr;
    for (auto &arg : fn.args())
      if (!sized(arg.getType(), layout) ||
          parameterABI(fn.getAttributes().getParamAttrs(arg.getArgNo())) ||
          parameterABI(call->getAttributes().getParamAttrs(arg.getArgNo())))
        return nullptr;
    for (auto &block : fn)
      for (auto &op : block)
        if (auto *nested = dyn_cast<CallInst>(&op))
          if (nested->isMustTailCall())
            return nullptr;
    // Small arithmetic helpers should remain available to normal inlining.
    if (fn.arg_size() < 64 &&
        (fn.getReturnType()->isVoidTy() ||
         layout.getTypeAllocSize(fn.getReturnType()).getFixedValue() < 512))
      return nullptr;
    return call;
  }
  AttributeList attributes(LLVMContext &context, AttributeList previous) {
    AttrBuilder function(context, previous.getFnAttrs());
    function.removeAttribute(Attribute::Memory);
    function.removeAttribute(Attribute::AllocSize);
    function.removeAttribute(Attribute::AlwaysInline);
    function.addAttribute(Attribute::NoInline);
    return AttributeList::get(context, AttributeSet::get(context, function),
                              AttributeSet{}, {AttributeSet{}});
  }
  std::optional<Slot> fieldSlot(Value *value,
                                const DenseMap<Value *, Slot> &results,
                                const DataLayout &layout) {
    SmallVector<ExtractValueInst *> path;
    Value *root = value;
    while (auto *extract = dyn_cast<ExtractValueInst>(root)) {
      path.push_back(extract);
      root = extract->getAggregateOperand();
    }
    auto found = results.find(root);
    if (found == results.end())
      return std::nullopt;
    auto slot = found->second;
    for (auto *extract : reverse(path))
      for (unsigned index : extract->getIndices()) {
        if (auto *type = dyn_cast<StructType>(slot.type)) {
          slot.offset += layout.getStructLayout(type)->getElementOffset(index);
          slot.type = type->getElementType(index);
        } else if (auto *type = dyn_cast<ArrayType>(slot.type)) {
          slot.offset +=
              index *
              layout.getTypeAllocSize(type->getElementType()).getFixedValue();
          slot.type = type->getElementType();
        } else
          return std::nullopt;
      }
    if (slot.type != value->getType())
      return std::nullopt;
    return slot;
  }
  bool rewrite(BasicBlock &block, SmallVector<Procedure> &plans,
               const DataLayout &layout, uint64_t &sharedBytes,
               uint64_t &originalArgumentBytes, uint64_t &newArgumentBytes) {
    if (plans.size() < 2)
      return false;
    DenseMap<CallInst *, unsigned> order;
    unsigned ordinal = 0;
    for (auto &op : block)
      if (auto *call = dyn_cast<CallInst>(&op))
        order[call] = ordinal++;
    llvm::sort(plans, [&](const Procedure &a, const Procedure &b) {
      return order.lookup(a.call) < order.lookup(b.call);
    });
    DenseMap<Value *, Slot> slots, results;
    DenseMap<CallInst *, SmallVector<std::pair<Value *, Slot>>> stores;
    uint64_t bytes = 0, oldBytes = 0, storedBytes = 0;
    bool tooLarge = false;
    Align alignment(8);
    auto allocate = [&](Type *type) {
      auto align = layout.getABITypeAlign(type);
      alignment = std::max(alignment, align);
      auto size = layout.getTypeAllocSize(type).getFixedValue();
      if (align.value() > maxWorkspace || size > maxWorkspace ||
          bytes > maxWorkspace) {
        tooLarge = true;
        return Slot{0, type};
      }
      bytes = alignTo(bytes, align.value());
      Slot slot{bytes, type};
      if (bytes > maxWorkspace - size)
        tooLarge = true;
      else
        bytes += size;
      return slot;
    };
    for (auto &plan : plans) {
      if (!plan.function->getReturnType()->isVoidTy()) {
        plan.result = allocate(plan.function->getReturnType());
        results[plan.call] = plan.result;
      }
    }
    for (auto &plan : plans)
      for (Value *arg : plan.call->args()) {
        oldBytes += layout.getTypeAllocSize(arg->getType()).getFixedValue();
        if (auto found = slots.find(arg); found != slots.end()) {
          plan.arguments.push_back(found->second);
          continue;
        }
        Slot slot;
        if (auto forwarded = fieldSlot(arg, results, layout)) {
          slot = *forwarded;
        } else {
          slot = allocate(arg->getType());
          stores[plan.call].push_back({arg, slot});
          storedBytes +=
              layout.getTypeAllocSize(arg->getType()).getFixedValue();
        }
        slots[arg] = slot;
        plan.arguments.push_back(slot);
      }
    // Bound extra stack storage. Unsupported groups retain their original ABI.
    auto *caller = block.getParent();
    if (tooLarge || bytes > maxWorkspace ||
        workspaceBytes.lookup(caller) > maxWorkspace - bytes)
      return false;
    workspaceBytes[caller] += bytes;
    auto &context = caller->getContext();
    IRBuilder<> entry(&*caller->getEntryBlock().getFirstInsertionPt());
    auto *workspace = entry.CreateAlloca(
        ArrayType::get(entry.getInt8Ty(), bytes), layout.getAllocaAddrSpace(),
        nullptr, "chipyard.procedure_workspace");
    workspace->setAlignment(alignment);
    auto address = [&](IRBuilder<> &b, Value *base, Slot slot) {
      return b.CreateInBoundsGEP(b.getInt8Ty(), base, b.getInt64(slot.offset));
    };
    for (auto &plan : plans) {
      auto &old = *plan.function;
      auto *type = FunctionType::get(Type::getVoidTy(context),
                                     {workspace->getType()}, false);
      auto *replacement =
          Function::Create(type, old.getLinkage(), old.getAddressSpace(),
                           old.getName() + ".workspace", old.getParent());
      replacement->copyAttributesFrom(&old);
      replacement->setAttributes(attributes(context, old.getAttributes()));
      // Read-only workspace parameters would otherwise be expanded back into
      // large scalar signatures by interprocedural argument promotion.
      if (old.getReturnType()->isVoidTy())
        replacement->addFnAttr(Attribute::NoIPA);
      replacement->setCallingConv(old.getCallingConv());
      replacement->getArg(0)->setName("chipyard.workspace");
      replacement->splice(replacement->begin(), &old);
      if (auto used = workspaceBytes.find(&old); used != workspaceBytes.end()) {
        workspaceBytes[replacement] = used->second;
        workspaceBytes.erase(used);
      }
      IRBuilder<> body(&*replacement->getEntryBlock().getFirstInsertionPt());
      for (auto [arg, slot] : zip(old.args(), plan.arguments)) {
        auto *value = body.CreateAlignedLoad(
            arg.getType(), address(body, replacement->getArg(0), slot),
            commonAlignment(alignment, slot.offset));
        value->takeName(&arg);
        arg.replaceAllUsesWith(value);
      }
      for (auto &bodyBlock : *replacement)
        if (auto *ret = dyn_cast<ReturnInst>(bodyBlock.getTerminator())) {
          if (!ret->getReturnValue())
            continue;
          IRBuilder<> result(ret);
          result.CreateAlignedStore(
              ret->getReturnValue(),
              address(result, replacement->getArg(0), plan.result),
              commonAlignment(alignment, plan.result.offset));
          result.CreateRetVoid();
          ret->eraseFromParent();
        }
      IRBuilder<> site(plan.call);
      site.SetCurrentDebugLocation(plan.call->getDebugLoc());
      for (auto [value, slot] : stores[plan.call])
        site.CreateAlignedStore(value, address(site, workspace, slot),
                                commonAlignment(alignment, slot.offset));
      auto *next = site.CreateCall(replacement, {workspace});
      next->setCallingConv(plan.call->getCallingConv());
      next->setAttributes(attributes(context, plan.call->getAttributes()));
      if (!old.getReturnType()->isVoidTy()) {
        auto *value = site.CreateAlignedLoad(
            old.getReturnType(), address(site, workspace, plan.result),
            commonAlignment(alignment, plan.result.offset));
        value->takeName(plan.call);
        plan.call->replaceAllUsesWith(value);
      }
      plan.call->eraseFromParent();
      auto name = old.getName().str();
      old.eraseFromParent();
      replacement->setName(name);
    }
    // Do not keep full aggregate reloads used only to repack the next call.
    SmallVector<WeakTrackingVH> dead;
    for (auto &op : block)
      if (isa<ExtractValueInst, LoadInst>(op) &&
          isInstructionTriviallyDead(&op))
        dead.push_back(&op);
    for (auto &handle : dead)
      if (auto *op = dyn_cast_or_null<Instruction>(handle))
        RecursivelyDeleteTriviallyDeadInstructions(op);
    sharedBytes += bytes;
    originalArgumentBytes += oldBytes;
    newArgumentBytes += storedBytes;
    return true;
  }
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    if (module.getDataLayoutStr().empty())
      return PreservedAnalyses::all();
    // Assembly can refer to a private symbol by spelling, outside LLVM's use
    // graph. Preserve those ABIs rather than assuming their only use is a call.
    if (!module.getModuleInlineAsm().empty())
      return PreservedAnalyses::all();
    for (auto &function : module)
      for (auto &block : function)
        for (auto &op : block)
          if (auto *call = dyn_cast<CallBase>(&op))
            if (call->isInlineAsm())
              return PreservedAnalyses::all();
    workspaceBytes.clear();
    MapVector<BasicBlock *, SmallVector<Procedure>> groups;
    for (auto &function : module)
      if (auto *call = eligible(function, module.getDataLayout()))
        groups[call->getParent()].push_back({&function, call, {}, {}});
    uint64_t bytes = 0, originalBytes = 0, storedBytes = 0;
    unsigned changed = 0;
    for (auto &[block, plans] : groups)
      if (rewrite(*block, plans, module.getDataLayout(), bytes, originalBytes,
                  storedBytes))
        changed += plans.size();
    if (!changed)
      return PreservedAnalyses::all();
    errs() << "Shared workspace for " << changed << " private procedures; "
           << bytes << " workspace bytes, " << originalBytes << " -> "
           << storedBytes << " argument-store bytes per group execution\n";
    return PreservedAnalyses::none();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardSharedProcedures", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-share-procedure-values")
                    return false;
                  manager.addPass(SharedProcedures());
                  return true;
                });
          }};
}
