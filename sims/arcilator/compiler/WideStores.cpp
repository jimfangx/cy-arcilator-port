// Distribute closed wide store expressions over their incoming CFG edges.
// Source loads and lifetime markers retain their execution and ordering.
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/SimplifyQuery.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
using namespace llvm;
namespace {
struct Edge {
  BasicBlock *block;
  Value *value;
};
struct Plan {
  PHINode *phi;
  StoreInst *store = nullptr;
  SmallVector<Instruction *> expression;
  SmallVector<Edge> edges;
};

static bool pureExpression(Instruction *inst) {
  if (!inst->getType()->isIntegerTy())
    return false;
  if (auto *select = dyn_cast<SelectInst>(inst))
    return select->getCondition()->getType()->isIntegerTy(1);
  if (isa<FreezeInst>(inst))
    return true;
  if (auto *cast = dyn_cast<CastInst>(inst))
    return cast->getOperand(0)->getType()->isIntegerTy();
  switch (inst->getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    return true;
  default:
    return false;
  }
}

static StringRef buildPlan(PHINode *phi, DominatorTree &dt, AAResults &aa,
                           const DataLayout &layout, Plan &plan) {
  auto *join = phi->getParent();
  if (join->isEHPad() || phi->getNumIncomingValues() < 2 ||
      phi->getNumIncomingValues() > 8)
    return "join-or-edge-count";
  Value *current = phi;
  while (current->hasOneUse()) {
    auto *user = dyn_cast<Instruction>(*current->user_begin());
    if (!user || user->getParent() != join)
      return "nonlocal-use";
    if (auto *store = dyn_cast<StoreInst>(user)) {
      auto *type = dyn_cast<IntegerType>(store->getValueOperand()->getType());
      if (!store->isSimple() || store->getValueOperand() != current || !type ||
          type->getBitWidth() <= 1024 || type->getBitWidth() % 8)
        return "store-shape";
      plan.store = store;
      break;
    }
    if (!pureExpression(user) || plan.expression.size() >= 32)
      return "expression-shape-or-cost";
    plan.expression.push_back(user);
    current = user;
  }
  if (!plan.store)
    return "shared-value-or-no-store";

  SmallPtrSet<BasicBlock *, 8> seen;
  for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
    auto *pred = phi->getIncomingBlock(i);
    auto *branch = pred->getTerminator();
    if (!isa<CondBrInst, UncondBrInst>(branch) || pred == join ||
        pred->isEHPad() || !seen.insert(pred).second)
      return "unsupported-incoming-edge";
    unsigned matches = 0;
    for (unsigned s = 0; s < branch->getNumSuccessors(); ++s)
      matches += branch->getSuccessor(s) == join;
    if (matches != 1)
      return "ambiguous-incoming-edge";
    plan.edges.push_back({pred, phi->getIncomingValue(i)});
  }

  // No join-local external dependency is reused from a previous loop
  // iteration, even if it happens to dominate a backedge terminator.
  auto available = [&](Value *value) {
    if (auto *inst = dyn_cast<Instruction>(value))
      if (inst->getParent() == join)
        return false;
    for (auto edge : plan.edges)
      if (!dt.dominates(value, edge.block->getTerminator()))
        return false;
    return true;
  };
  if (!available(plan.store->getPointerOperand()))
    return "unavailable-store-address";
  current = phi;
  for (auto *inst : plan.expression) {
    for (auto &operand : inst->operands())
      if (operand.get() != current && !available(operand.get()))
        return "unavailable-expression-input";
    current = inst;
  }

  MemoryLocation destination(
      plan.store->getPointerOperand(),
      LocationSize::precise(
          layout.getTypeStoreSize(plan.store->getValueOperand()->getType())
              .getFixedValue()));
  for (auto &inst : *join) {
    if (&inst == plan.store)
      break;
    if (isa<PHINode>(inst))
      continue;
    if (auto *life = dyn_cast<LifetimeIntrinsic>(&inst)) {
      // Store motion may cross the end of a distinct private scratch object.
      // In particular it never delays that end or re-reads expired storage.
      if (life->getIntrinsicID() != Intrinsic::lifetime_end ||
          !isa<AllocaInst>(life->getArgOperand(0)) ||
          aa.alias(destination,
                   MemoryLocation::getBeforeOrAfter(life->getArgOperand(0))) !=
              AliasResult::NoAlias)
        return "lifetime-or-alias-barrier";
      continue;
    }
    if (auto *load = dyn_cast<LoadInst>(&inst)) {
      if (!load->isSimple() ||
          aa.alias(destination, MemoryLocation::get(load)) !=
              AliasResult::NoAlias)
        return "read-or-alias-barrier";
      continue;
    }
    if (isa<CallBase, FenceInst>(inst) || inst.mayReadOrWriteMemory() ||
        inst.mayHaveSideEffects())
      return "effect-barrier";
  }
  return "eligible";
}

struct WideStores : RequiredPassInfoMixin<WideStores> {
  PreservedAnalyses run(Module &module, ModuleAnalysisManager &manager) {
    if (module.getDataLayoutStr().empty())
      return PreservedAnalyses::all();
    auto &functions =
        manager.getResult<FunctionAnalysisManagerModuleProxy>(module)
            .getManager();
    unsigned changed = 0, split = 0, computations = 0;
    json::Object rejects;
    StringMap<unsigned> reasons;
    for (auto &function : module) {
      if (function.isDeclaration() ||
          function.hasFnAttribute(Attribute::OptimizeNone) ||
          function.hasFnAttribute(Attribute::Naked) ||
          function.hasPersonalityFn() ||
          function.hasFnAttribute(Attribute::Convergent))
        continue;
      auto &dt = functions.getResult<DominatorTreeAnalysis>(function);
      auto &aa = functions.getResult<AAManager>(function);
      SmallVector<Plan> plans;
      for (auto &block : function)
        for (auto &phi : block.phis())
          if (auto *type = dyn_cast<IntegerType>(phi.getType()))
            if (type->getBitWidth() > 1024 && type->getBitWidth() % 8 == 0) {
              Plan plan{&phi};
              auto result =
                  buildPlan(&phi, dt, aa, module.getDataLayout(), plan);
              if (result == "eligible")
                plans.push_back(std::move(plan));
              else
                ++reasons[result];
            }
      // All analysis queries precede mutation. Independent closed single-use
      // expressions cannot overlap or erase another plan's external operands.
      for (auto &plan : plans) {
        auto *join = plan.phi->getParent();
        for (auto edge : plan.edges) {
          auto *block = edge.block;
          if (block->getTerminator()->getNumSuccessors() != 1) {
            block = SplitEdge(block, join, &dt, nullptr, nullptr,
                              "wide.store.edge");
            assert(block && "Validated ordinary branch edge must split");
            ++split;
          }
          auto *point = block->getTerminator();
          Value *current = edge.value;
          Value *original = plan.phi;
          for (auto *inst : plan.expression) {
            auto *copy = inst->clone();
            for (auto &operand : copy->operands())
              if (operand.get() == original)
                operand.set(current);
            copy->insertBefore(point->getIterator());
            if (auto *value = simplifyInstruction(
                    copy, SimplifyQuery(module.getDataLayout(), copy)
                              .getWithoutUndef())) {
              current = value;
              copy->eraseFromParent();
            } else {
              current = copy;
            }
            original = inst;
            ++computations;
          }
          auto *store = cast<StoreInst>(plan.store->clone());
          store->setOperand(0, current);
          store->insertBefore(point->getIterator());
        }
        plan.store->eraseFromParent();
        for (auto *inst : llvm::reverse(plan.expression))
          inst->eraseFromParent();
        plan.phi->eraseFromParent();
        ++changed;
      }
    }
    for (auto &row : reasons)
      rejects[row.getKey()] = row.getValue();
    errs() << "Wide store distribution: "
           << formatv("{0}",
                      json::Value(json::Object{
                          {"distributed_stores", changed},
                          {"split_edges", split},
                          {"expression_clones_before_folding", computations},
                          {"rejected", std::move(rejects)}}))
           << "\n";
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardWideStores", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name != "chipyard-distribute-wide-stores")
                    return false;
                  manager.addPass(WideStores());
                  return true;
                });
          }};
}
