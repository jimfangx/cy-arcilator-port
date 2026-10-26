// Lower the port's owned pure wide-shift ABI without round-tripping model IR.
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/Regex.h"
#include <algorithm>
#include <map>

using namespace llvm;
namespace {
struct Shift {
  Function *function;
  unsigned bits, mode;
};
struct ScalarCall {
  CallInst *call;
  Value *data;
  unsigned bits, small;
};

bool hostAttribute(Attribute attribute) {
  if (!attribute.isStringAttribute())
    return false;
  StringRef name = attribute.getKindAsString();
  return name == "target-cpu" || name == "target-features" || name == "tune-cpu";
}

AttributeList withoutHostAttributes(LLVMContext &context, AttributeList attributes) {
  for (StringRef name : {"target-cpu", "target-features", "tune-cpu"})
    attributes = attributes.removeFnAttribute(context, name);
  return attributes;
}

void copyHostAttributes(const Function &from, Function &to) {
  for (Attribute attribute : from.getAttributes().getFnAttrs())
    if (hostAttribute(attribute))
      to.addFnAttr(attribute);
}

// These attributes describe the existing native helpers. They are never
// inferred for arbitrary external functions or caller-provided definitions.
Function *pointerHelper(Module &module, StringRef name, bool scalar,
                        bool bytes = false) {
  auto &context = module.getContext();
  auto *pointer = PointerType::getUnqual(context);
  auto *word = Type::getInt64Ty(context);
  SmallVector<Type *> arguments{pointer,
                                scalar ? static_cast<Type *>(word) : pointer};
  if (bytes) {
    arguments.push_back(pointer);
    arguments.push_back(word);
    arguments.push_back(Type::getInt32Ty(context));
  } else {
    arguments.append(4, word);
    if (!scalar)
      arguments.push_back(Type::getInt32Ty(context));
  }
  auto *type = FunctionType::get(Type::getVoidTy(context), arguments, false);
  auto *function = Function::Create(type, GlobalValue::ExternalLinkage, name);
  function->setDoesNotThrow();
  function->setMemoryEffects(MemoryEffects::argMemOnly());
  for (unsigned i = 0; i < (bytes ? 3u : scalar ? 1u : 2u); ++i) {
    function->addParamAttr(i,
        Attribute::getWithCaptureInfo(context, CaptureInfo::none()));
    function->addParamAttr(i, i ? Attribute::ReadOnly : Attribute::WriteOnly);
  }
  return function;
}

bool validHelper(Module &module, Function *expected) {
  auto *existing = module.getNamedValue(expected->getName());
  if (!existing)
    return true;
  auto *function = dyn_cast<Function>(existing);
  return function && function->isDeclaration() &&
         function->getFunctionType() == expected->getFunctionType() &&
         function->getCallingConv() == CallingConv::C &&
         function->hasExternalLinkage() &&
         withoutHostAttributes(module.getContext(), function->getAttributes()) ==
             expected->getAttributes();
}

Function *installHelper(Module &module, Function *expected) {
  if (auto *existing = module.getFunction(expected->getName())) {
    delete expected;
    return existing;
  }
  module.getFunctionList().push_back(expected);
  return expected;
}

void body(Function &function, unsigned bits, unsigned mode,
          Function *helper, unsigned small = 0) {
  auto &context = function.getContext();
  auto *wide = IntegerType::get(context, bits);
  unsigned rounded = (bits + 7) / 8 * 8;
  auto *storage = IntegerType::get(context, rounded);
  function.setLinkage(GlobalValue::InternalLinkage);
  // opt attaches host CPU/features before running passes. Keep them here and
  // on new specializations just as a separate preparation+O3 pipeline does.
  function.addFnAttr(Attribute::AlwaysInline);
  function.setDoesNotThrow();
  function.setWillReturn();
  function.setMemoryEffects(MemoryEffects::none());
  auto *entry = BasicBlock::Create(context, "", &function);
  IRBuilder<NoFolder> builder(entry);
  Value *data = function.getArg(0), *amount = function.getArg(1);
  data->setName(small ? "small" : "data");
  amount->setName("amount");
  Value *input = nullptr;
  if (!small) {
    auto *allocation = builder.CreateAlloca(storage, nullptr, "input");
    allocation->setAlignment(Align(8));
    input = allocation;
  }
  auto *output = builder.CreateAlloca(storage, nullptr, "output");
  output->setAlignment(Align(8));
  if (small) {
    auto *probe = builder.CreateFreeze(data, "zero_probe");
    auto *zero = builder.CreateICmpEQ(
        probe, ConstantInt::get(data->getType(), 0), "is_zero");
    auto *zeroResult = BasicBlock::Create(context, "zero_result", &function);
    auto *shiftResult = BasicBlock::Create(context, "shift_result", &function);
    builder.CreateCondBr(zero, zeroResult, shiftResult);
    builder.SetInsertPoint(zeroResult);
    builder.CreateRet(ConstantInt::get(wide, 0));
    builder.SetInsertPoint(shiftResult);
  }
  auto *low = builder.CreateTrunc(amount, builder.getInt64Ty(), "low");
  unsigned upperBits = (bits / 64 - 1) * 64;
  auto *slice = builder.CreateLShr(amount, 64, "upper_slice");
  auto *upper = builder.CreateTrunc(slice, IntegerType::get(context, upperBits),
                                    "upper");
  auto *nonzero = builder.CreateICmpNE(
      upper, ConstantInt::get(upper->getType(), 0), "nonzero");
  auto *high = builder.CreateZExt(nonzero, builder.getInt64Ty(), "high");
  Value *tail = builder.getInt64(0);
  if (bits % 64) {
    auto *tailSlice = builder.CreateLShr(amount, bits / 64 * 64, "tail_slice");
    tail = builder.CreateTrunc(tailSlice, builder.getInt64Ty(), "tail");
  }
  if (small) {
    if (small < 64)
      data = builder.CreateZExt(data, builder.getInt64Ty(), "scalar");
    builder.CreateCall(helper, {output, data, low, high, tail,
                                builder.getInt64(bits)});
  } else {
    if (rounded != bits)
      data = builder.CreateZExt(data, storage, "d");
    builder.CreateAlignedStore(data, input, Align(8));
    builder.CreateCall(helper, {output, input, low, high, tail,
                                builder.getInt64(bits), builder.getInt32(mode)});
  }
  auto *value = builder.CreateAlignedLoad(storage, output, Align(8), "value");
  Value *result = value;
  if (rounded != bits)
    result = builder.CreateTrunc(value, wide, "result");
  builder.CreateRet(result);
}

struct NativeArithmetic : RequiredPassInfoMixin<NativeArithmetic> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    SmallVector<Shift> shifts;
    DenseSet<Function *> leftShifts;
    Regex owned("^chipyard_arc_wide_shift_([012])_([0-9]+)$");
    auto fail = [&](StringRef message) {
      module.getContext().emitError(message);
      return PreservedAnalyses::all();
    };
    for (Function &function : module) {
      if (!function.isDeclaration() ||
          !function.getName().starts_with("chipyard_arc_wide_shift_"))
        continue;
      if (function.getName() == "chipyard_arc_wide_shift_words" ||
          function.getName() == "chipyard_arc_wide_shift_bytes")
        continue;
      SmallVector<StringRef> matches;
      unsigned bits, mode;
      if (!owned.match(function.getName(), &matches) ||
          matches[1].getAsInteger(10, mode) ||
          matches[2].getAsInteger(10, bits) || bits <= 1024 ||
          bits > IntegerType::MAX_INT_BITS - 7)
        return fail("invalid generated wide arithmetic declaration");
      auto *type = IntegerType::get(module.getContext(), bits);
      if (function.getFunctionType() != FunctionType::get(type, {type, type}, false) ||
          function.getCallingConv() != CallingConv::C ||
          !function.hasExternalLinkage() ||
          !withoutHostAttributes(module.getContext(),
                                 function.getAttributes()).isEmpty())
        return fail("incompatible generated wide arithmetic declaration");
      shifts.push_back({&function, bits, mode});
      if (mode == 0)
        leftShifts.insert(&function);
    }
    auto *words = pointerHelper(module, "chipyard_arc_wide_shift_words", false);
    auto *scalar = pointerHelper(module, "chipyard_arc_private_shift_scalar", true);
    auto *bytes = pointerHelper(module, "chipyard_arc_wide_shift_bytes", false, true);
    bool valid = true;
    for (auto *helper : {words, scalar, bytes})
      if (auto *existing = module.getFunction(helper->getName()))
        if (existing->isDeclaration() && !validHelper(module, helper))
          valid = false;
    delete bytes;
    if (!valid || (!shifts.empty() && !validHelper(module, words))) {
      delete words;
      delete scalar;
      return fail("incompatible native wide arithmetic helper ABI");
    }
    if (shifts.empty()) {
      delete words;
      delete scalar;
      return PreservedAnalyses::all();
    }

    // Analyze only direct ordinary calls; unusual calls retain their original
    // full-width wrapper. Rewriting musttail, operand bundles or ABI attributes
    // could introduce constraints that the private scalar signature violates.
    SmallVector<ScalarCall> calls;
    std::map<std::pair<unsigned, unsigned>, Function *> narrow;
    for (Function &function : module)
      for (BasicBlock &block : function)
        for (Instruction &instruction : block) {
          auto *call = dyn_cast<CallInst>(&instruction);
          if (!call || !leftShifts.contains(call->getCalledFunction()) ||
              call->getCallingConv() != CallingConv::C ||
              call->getTailCallKind() != CallInst::TCK_None ||
              call->hasOperandBundles() || !call->getAttributes().isEmpty())
            continue;
          unsigned bits = call->getType()->getIntegerBitWidth(), small = 0;
          Value *data = call->getArgOperand(0);
          if (auto *constant = dyn_cast<ConstantInt>(data)) {
            if (!constant->isNegative() && constant->getValue().getActiveBits() <= 64)
              small = std::max(1u, constant->getValue().getActiveBits());
          } else if (auto *extension = dyn_cast<ZExtInst>(data)) {
            auto *type = dyn_cast<IntegerType>(extension->getSrcTy());
            if (type && type->getBitWidth() <= 64) {
              small = type->getBitWidth();
              data = extension->getOperand(0);
            }
          }
          if (!small)
            continue;
          std::string name = "chipyard_arc_private_scalar_" + std::to_string(bits) +
                             "_" + std::to_string(small);
          if (module.getNamedValue(name))
            continue;
          calls.push_back({call, data, bits, small});
          narrow[{bits, small}] = nullptr;
        }

    if (!calls.empty() && !validHelper(module, scalar)) {
      delete words;
      delete scalar;
      return fail("incompatible native wide arithmetic helper ABI");
    }
    copyHostAttributes(*shifts.front().function, *words);
    copyHostAttributes(*shifts.front().function, *scalar);
    words = installHelper(module, words);
    // Original wrappers precede appended scalar specializations and helper
    // declarations in deterministic symbol order, matching text preparation.
    for (const Shift &shift : shifts)
      body(*shift.function, shift.bits, shift.mode, words);
    for (auto &[key, function] : narrow) {
      auto [bits, small] = key;
      auto &context = module.getContext();
      auto *type = FunctionType::get(IntegerType::get(context, bits),
          {IntegerType::get(context, small), IntegerType::get(context, bits)}, false);
      function = Function::Create(type, GlobalValue::InternalLinkage,
          "chipyard_arc_private_scalar_" + std::to_string(bits) + "_" +
              std::to_string(small), module);
      for (const ScalarCall &plan : calls)
        if (plan.bits == bits && plan.small == small) {
          copyHostAttributes(*plan.call->getCalledFunction(), *function);
          break;
        }
    }
    if (!calls.empty()) {
      scalar = installHelper(module, scalar);
      for (auto &[key, function] : narrow)
        body(*function, key.first, 0, scalar, key.second);
    } else
      delete scalar;
    for (const ScalarCall &plan : calls) {
      IRBuilder<NoFolder> builder(plan.call);
      Value *data = plan.data;
      if (auto *constant = dyn_cast<ConstantInt>(data))
        data = ConstantInt::get(IntegerType::get(module.getContext(), plan.small),
                                constant->getValue().getLimitedValue());
      auto *call = builder.CreateCall(narrow[{plan.bits, plan.small}],
          {data, plan.call->getArgOperand(1)});
      call->takeName(plan.call);
      call->copyMetadata(*plan.call);
      plan.call->replaceAllUsesWith(call);
      plan.call->eraseFromParent();
    }
    return PreservedAnalyses::none();
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardNativeArithmetic", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-native-arithmetic")
                    return false;
                  manager.addPass(NativeArithmetic());
                  return true;
                });
          }};
}
