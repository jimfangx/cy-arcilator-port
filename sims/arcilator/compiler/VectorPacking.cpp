// Use target-legal fixed vectors for pure scalar integer packing DAGs.
// Memory accesses, native ABIs, freezes and callback ordering remain scalar.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"
using namespace llvm;
namespace {
struct VectorPacking : OptionalPassInfoMixin<VectorPacking> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &mam) {
    auto &fam =
        mam.getResult<FunctionAnalysisManagerModuleProxy>(module).getManager();
    unsigned dags = 0, operations = 0, bridges = 0, tiny = 0, costly = 0;
    json::Array costs;
    for (Function &function : module) {
      if (!module.getDataLayout().isLittleEndian())
        continue;
      if (function.isDeclaration() ||
          function.hasFnAttribute(Attribute::OptimizeNone) ||
          function.hasFnAttribute(Attribute::Naked))
        continue;
      auto &tti = fam.getResult<TargetIRAnalysis>(function);
      auto scalar = tti.getRegisterBitWidth(TargetTransformInfo::RGK_Scalar);
      auto fixed =
          tti.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector);
      if (scalar.isScalable() || fixed.isScalable())
        continue;
      unsigned word = scalar.getFixedValue(), maximum = fixed.getFixedValue();
      if (!word || !isPowerOf2_32(word) || maximum <= word)
        continue;
      for (BasicBlock &block : function) {
        if (block.isEHPad())
          continue;
        DenseMap<Instruction *, FixedVectorType *> eligible;
        SmallVector<Instruction *> order;
        for (Instruction &inst : block) {
          auto *type = dyn_cast<IntegerType>(inst.getType());
          if (!type || type->getBitWidth() <= word ||
              type->getBitWidth() > maximum || inst.hasPoisonGeneratingFlags())
            continue;
          unsigned opcode = inst.getOpcode();
          bool supported = opcode == Instruction::And ||
                           opcode == Instruction::Or ||
                           opcode == Instruction::Xor;
          if (auto *select = dyn_cast<SelectInst>(&inst))
            supported = select->getCondition()->getType()->isIntegerTy(1);
          if (auto *cast = dyn_cast<ZExtInst>(&inst))
            supported = cast->getSrcTy()->isIntegerTy();
          if (opcode == Instruction::Shl || opcode == Instruction::LShr) {
            auto *amount = dyn_cast<ConstantInt>(inst.getOperand(1));
            supported = amount && amount->getValue().ult(type->getBitWidth());
          }
          if (!supported)
            continue;
          unsigned padded = PowerOf2Ceil(type->getBitWidth());
          if (padded > maximum)
            continue;
          auto *vector = FixedVectorType::get(
              IntegerType::get(module.getContext(), word), padded / word);
          if (!tti.isTypeLegal(vector))
            continue;
          eligible[&inst] = vector;
          order.push_back(&inst);
        }
        SmallPtrSet<Instruction *, 32> visited;
        for (Instruction *root : order) {
          if (!root || !visited.insert(root).second)
            continue;
          SmallVector<Instruction *> todo{root};
          SmallPtrSet<Instruction *, 32> component;
          while (!todo.empty()) {
            auto *current = todo.pop_back_val();
            component.insert(current);
            auto visit = [&](Value *value) {
              auto *inst = dyn_cast<Instruction>(value);
              if (inst && inst->getType() == root->getType() &&
                  eligible.count(inst) && visited.insert(inst).second)
                todo.push_back(inst);
            };
            for (Value *operand : current->operands())
              visit(operand);
            for (User *user : current->users())
              visit(user);
          }
          if (component.size() < 3) {
            ++tiny;
            continue;
          }
          auto *integer = cast<IntegerType>(root->getType());
          auto *vector = eligible[root];
          auto *padded = IntegerType::get(
              module.getContext(),
              vector->getPrimitiveSizeInBits().getFixedValue());
          SmallPtrSet<Instruction *, 32> escaping;
          InstructionCost originalCost = 0;
          constexpr auto kind = TargetTransformInfo::TCK_RecipThroughput;
          for (Instruction *inst : component) {
            originalCost += tti.getInstructionCost(inst, kind);
            for (User *user : inst->users())
              if (!component.count(dyn_cast<Instruction>(user)))
                escaping.insert(inst);
          }
          DenseMap<Value *, Value *> converted;
          SmallVector<std::pair<Instruction *, Value *>> replacements;
          SmallVector<Instruction *> created;
          unsigned shiftOps = 0, selectOps = 0, extensionOps = 0;
          for (Instruction *inst : order) {
            if (!inst || !component.count(inst))
              continue;
            IRBuilder<ConstantFolder, IRBuilderCallbackInserter> builder(
                module.getContext(), ConstantFolder(),
                IRBuilderCallbackInserter(
                    [&](Instruction *value) { created.push_back(value); }));
            builder.SetInsertPoint(inst);
            builder.SetCurrentDebugLocation(inst->getDebugLoc());
            auto convert = [&](Value *value) -> Value * {
              if (Value *prior = converted.lookup(value))
                return prior;
              Value *wide =
                  builder.CreateZExtOrBitCast(value, padded, "vp.pad");
              Value *result = builder.CreateBitCast(wide, vector, "vp.in");
              converted[value] = result;
              return result;
            };
            Value *result = nullptr;
            if (auto *select = dyn_cast<SelectInst>(inst)) {
              result = builder.CreateSelect(
                  select->getCondition(), convert(select->getTrueValue()),
                  convert(select->getFalseValue()), "vp.select");
              ++selectOps;
            } else if (auto *extension = dyn_cast<ZExtInst>(inst)) {
              result = convert(extension->getOperand(0));
              ++extensionOps;
            } else if (inst->getOpcode() == Instruction::Shl ||
                       inst->getOpcode() == Instruction::LShr) {
              bool left = inst->getOpcode() == Instruction::Shl;
              unsigned amount =
                  cast<ConstantInt>(inst->getOperand(1))->getZExtValue();
              unsigned lanes = vector->getNumElements(), whole = amount / word,
                       part = amount % word;
              Value *input = convert(inst->getOperand(0));
              auto move = [&](int displacement) -> Value * {
                if (!displacement)
                  return input;
                SmallVector<int> indices;
                for (unsigned i = 0; i < lanes; ++i) {
                  int source = int(i) + displacement;
                  indices.push_back(
                      source >= 0 && source < int(lanes) ? source : int(lanes));
                }
                return builder.CreateShuffleVector(
                    input, Constant::getNullValue(vector), indices, "vp.lanes");
              };
              result = move(left ? -int(whole) : int(whole));
              if (part) {
                auto shift = [&](unsigned n) {
                  return ConstantVector::getSplat(
                      ElementCount::getFixed(lanes),
                      ConstantInt::get(vector->getElementType(), n));
                };
                if (left) {
                  Value *carry = builder.CreateLShr(
                      move(-int(whole) - 1), shift(word - part), "vp.carry");
                  result = builder.CreateOr(
                      builder.CreateShl(result, shift(part), "vp.shift"), carry,
                      "vp.combine");
                } else {
                  Value *carry = builder.CreateShl(
                      move(int(whole) + 1), shift(word - part), "vp.carry");
                  result = builder.CreateOr(
                      builder.CreateLShr(result, shift(part), "vp.shift"),
                      carry, "vp.combine");
                }
              }
              if (left && integer != padded) {
                Constant *mask = ConstantInt::get(
                    padded, APInt::getLowBitsSet(padded->getBitWidth(),
                                                 integer->getBitWidth()));
                result = builder.CreateAnd(
                    result, ConstantExpr::getBitCast(mask, vector), "vp.width");
              }
              ++shiftOps;
            } else {
              result = builder.CreateBinOp(
                  static_cast<Instruction::BinaryOps>(inst->getOpcode()),
                  convert(inst->getOperand(0)), convert(inst->getOperand(1)),
                  "vp.op");
            }
            converted[inst] = result;
            if (escaping.count(inst)) {
              Value *bits = builder.CreateBitCast(result, padded, "vp.bits");
              Value *bridge =
                  builder.CreateTruncOrBitCast(bits, integer, "vp.out");
              replacements.emplace_back(inst, bridge);
            }
          }
          InstructionCost vectorCost = 0;
          for (Instruction *inst : created)
            vectorCost += tti.getInstructionCost(inst, kind);
          bool accepted = originalCost.isValid() && vectorCost.isValid() &&
                          vectorCost < originalCost;
          costs.push_back(json::Object{
              {"width", integer->getBitWidth()},
              {"nodes", component.size()},
              {"created_instructions", created.size()},
              {"shifts", shiftOps},
              {"selects", selectOps},
              {"extensions", extensionOps},
              {"escaping_results", escaping.size()},
              {"original_cost",
               originalCost.isValid() ? originalCost.getValue() : -1},
              {"vector_and_bridge_cost",
               vectorCost.isValid() ? vectorCost.getValue() : -1},
              {"accepted", accepted}});
          if (!accepted) {
            for (Instruction *inst : reverse(created)) {
              assert(inst->use_empty());
              inst->eraseFromParent();
            }
            ++costly;
            continue;
          }
          bridges += replacements.size();
          for (auto pair : replacements)
            pair.first->replaceAllUsesWith(pair.second);
          // Internal old integer expressions become dead; delete reverse order.
          for (Instruction *&inst : reverse(order))
            if (inst && component.count(inst)) {
              assert(inst->use_empty() && "old bitwise DAG still used");
              eligible.erase(inst);
              inst->eraseFromParent();
              inst = nullptr;
            }
          ++dags;
          operations += component.size();
        }
      }
    }
    errs() << "Vector packing: "
           << json::Object{{"vector_dags", dags},
                           {"vector_operations", operations},
                           {"scalar_output_bridges", bridges},
                           {"tiny_components", tiny},
                           {"cost_rejected_components", costly},
                           {"costs", std::move(costs)}}
           << "\n";
    return dags ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardVectorPacking", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-vector-packing")
                    return false;
                  manager.addPass(VectorPacking());
                  return true;
                });
          }};
}
