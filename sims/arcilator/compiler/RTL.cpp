#include "ScopeFrames.h"
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Based on the matching CIRCT SDK LLHD loop unroller. In addition to its
// supported loop forms, recognize an equality loop whose first condition is
// already false, and preserve diagnostics when sequential processes are
// inferred.

#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/LLHD/LLHDPasses.h"
#include "circt/Dialect/SV/SVAttributes.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/UnusedOpPruner.h"
#include "mlir/Analysis/CFGLoopInfo.h"
#include "mlir/Analysis/Liveness.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/Inliner.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llhd-unroll-loops"

namespace circt {
namespace llhd {
#define GEN_PASS_DEF_UNROLLLOOPSPASS
#include "circt/Dialect/LLHD/LLHDPasses.h.inc"
} // namespace llhd
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace llhd;
using llvm::SmallDenseSet;
using llvm::SmallSetVector;

//===----------------------------------------------------------------------===//
// Utilities
//===----------------------------------------------------------------------===//

// The SDK reconstructs sim DPI declarations during Moore-to-Core conversion
// without copying custom declaration attrs. Restore the SV ABI provenance
// recorded by the importer before flattening or coalescing declarations. Call
// sites retain their own declaration context even when compatible C imports
// later share a wrapper.
struct RestoreDPIABI : PassWrapper<RestoreDPIABI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RestoreDPIABI)
  StringRef getArgument() const override { return "chipyard-restore-dpi-abi"; }
  void runOnOperation() override {
    auto imports =
        getOperation()->getAttrOfType<DictionaryAttr>("chipyard.dpi_imports");
    if (!imports)
      return;
    bool invalid = false;
    getOperation().walk([&](sim::DPIFuncOp function) {
      auto entry = imports.getAs<DictionaryAttr>(function.getSymName());
      if (!entry)
        return;
      auto abi = entry.getAs<ArrayAttr>("abi");
      auto declaration = entry.getAs<DictionaryAttr>("declaration");
      auto version = entry.getAs<IntegerAttr>("version");
      if (!version || version.getInt() != 1 || !abi || !declaration ||
          abi.size() != function.getDpiFunctionType().getArguments().size()) {
        function.emitError(
            "invalid original SystemVerilog DPI signature metadata");
        invalid = true;
        return;
      }
      function->setAttr("chipyard.dpi_abi", abi);
      function->setAttr("chipyard.dpi_decl", declaration);
    });
    if (invalid) {
      signalPassFailure();
      return;
    }
    getOperation().walk([&](sim::DPICallOp call) {
      auto function = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
          call, call.getCalleeAttr());
      if (function)
        if (auto declaration = function->getAttr("chipyard.dpi_decl"))
          call->setAttr("chipyard.dpi_decl", declaration);
    });
    getOperation()->removeAttr("chipyard.dpi_imports");
  }
};

// The SDK signal promoter handles integer/aggregate storage, but can leave
// scalar real signals behind even when all reads and writes are explicit.
// Store their IEEE bits in an integer signal; arithmetic and DPI values keep
// their floating type through casts at each original read/write point.
struct BitcastFloatSignals
    : PassWrapper<BitcastFloatSignals, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(BitcastFloatSignals)
  StringRef getArgument() const override {
    return "chipyard-bitcast-float-signals";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, llhd::LLHDDialect>();
  }
  void runOnOperation() override {
    SmallVector<SignalOp> signals;
    getOperation().walk([&](SignalOp signal) {
      if (isa<FloatType>(signal.getType().getNestedType()) &&
          llvm::all_of(signal->getUsers(), [](Operation *user) {
            return isa<ProbeOp, DriveOp>(user);
          }))
        signals.push_back(signal);
    });
    for (auto signal : signals) {
      auto fp = cast<FloatType>(signal.getType().getNestedType());
      auto integer = IntegerType::get(&getContext(), fp.getWidth());
      SmallVector<ProbeOp> probes;
      SmallVector<DriveOp> drives;
      for (auto *user : signal->getUsers()) {
        if (auto probe = dyn_cast<ProbeOp>(user))
          probes.push_back(probe);
        else
          drives.push_back(cast<DriveOp>(user));
      }
      OpBuilder builder(signal);
      if (auto init = signal.getInit()) {
        Value bits =
            arith::BitcastOp::create(builder, signal.getLoc(), integer, init);
        signal.getInitMutable().assign(bits);
      }
      signal.getResult().setType(llhd::RefType::get(integer));
      for (auto probe : probes) {
        probe.getResult().setType(integer);
        builder.setInsertionPointAfter(probe);
        Value value = arith::BitcastOp::create(builder, probe.getLoc(), fp,
                                               probe.getResult());
        probe.getResult().replaceUsesWithIf(value, [&](OpOperand &use) {
          return use.getOwner() != value.getDefiningOp();
        });
      }
      for (auto drive : drives) {
        builder.setInsertionPoint(drive);
        Value bits = arith::BitcastOp::create(builder, drive.getLoc(), integer,
                                              drive.getValue());
        drive.getValueMutable().assign(bits);
      }
    }
  }
};

// Arc coroutine frames use hardware bit widths. Carry floating process
// outputs and CFG arguments as IEEE integer bits across suspension points,
// while arithmetic and C calls retain their original floating types.
struct BitcastFloatProcesses
    : PassWrapper<BitcastFloatProcesses, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(BitcastFloatProcesses)
  StringRef getArgument() const override {
    return "chipyard-bitcast-float-processes";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, llhd::LLHDDialect>();
  }
  void runOnOperation() override {
    getOperation().walk([&](ProcessOp process) {
      OpBuilder builder(process);
      auto encode = [&](Value value, Operation *before) -> Value {
        auto fp = dyn_cast<FloatType>(value.getType());
        if (!fp)
          return value;
        builder.setInsertionPoint(before);
        return arith::BitcastOp::create(builder, before->getLoc(),
                                        builder.getIntegerType(fp.getWidth()),
                                        value);
      };
      for (auto result : process.getResults()) {
        auto fp = dyn_cast<FloatType>(result.getType());
        if (!fp)
          continue;
        result.setType(builder.getIntegerType(fp.getWidth()));
        builder.setInsertionPointAfter(process);
        auto restored =
            arith::BitcastOp::create(builder, process.getLoc(), fp, result);
        result.replaceUsesWithIf(restored, [&](OpOperand &use) {
          return use.getOwner() != restored.getOperation();
        });
      }
      for (auto &block : process.getBody()) {
        for (auto arg : block.getArguments()) {
          auto fp = dyn_cast<FloatType>(arg.getType());
          if (!fp)
            continue;
          arg.setType(builder.getIntegerType(fp.getWidth()));
          builder.setInsertionPointToStart(&block);
          auto restored =
              arith::BitcastOp::create(builder, process.getLoc(), fp, arg);
          arg.replaceUsesWithIf(restored, [&](OpOperand &use) {
            return use.getOwner() != restored.getOperation();
          });
        }
      }
      for (auto &block : process.getBody()) {
        auto *term = block.getTerminator();
        if (auto branch = dyn_cast<BranchOpInterface>(term)) {
          for (unsigned i = 0; i < term->getNumSuccessors(); ++i) {
            auto forwarded =
                branch.getSuccessorOperands(i).getMutableForwardedOperands();
            SmallVector<Value> values;
            for (auto value : forwarded.getAsOperandRange())
              values.push_back(encode(value, term));
            forwarded.assign(values);
          }
        }
        auto convert = [&](MutableOperandRange range) {
          SmallVector<Value> values;
          for (auto value : range.getAsOperandRange())
            values.push_back(encode(value, term));
          range.assign(values);
        };
        if (auto wait = dyn_cast<WaitOp>(term)) {
          convert(wait.getYieldOperandsMutable());
          convert(wait.getObservedMutable());
          convert(wait.getDestOperandsMutable());
        } else if (auto halt = dyn_cast<HaltOp>(term))
          convert(halt.getYieldOperandsMutable());
      }
    });
  }
};

/// Clone a list of blocks into a region before the given block.
///
/// See `Region::cloneInto` for the original code that clones an entire region.
static void cloneBlocks(ArrayRef<Block *> blocks, Region &region,
                        Region::iterator before, IRMapping &mapper) {
  // If the list is empty there is nothing to clone.
  if (blocks.empty())
    return;

  // First clone all the blocks and block arguments and map them, but don't yet
  // clone the operations, as they may otherwise add a use to a block that has
  // not yet been mapped
  SmallVector<Block *> newBlocks;
  newBlocks.reserve(blocks.size());
  for (auto *block : blocks) {
    auto *newBlock = new Block();
    mapper.map(block, newBlock);
    for (auto arg : block->getArguments())
      mapper.map(arg, newBlock->addArgument(arg.getType(), arg.getLoc()));
    region.getBlocks().insert(before, newBlock);
    newBlocks.push_back(newBlock);
  }

  // Now follow up with creating the operations, but don't yet clone their
  // regions, nor set their operands. Setting the successors is safe as all have
  // already been mapped. We are essentially just creating the operation results
  // to be able to map them. Cloning the operands and region as well would lead
  // to uses of operations not yet mapped.
  auto cloneOptions =
      Operation::CloneOptions::all().cloneRegions(false).cloneOperands(false);
  for (auto [oldBlock, newBlock] : llvm::zip(blocks, newBlocks))
    for (auto &op : *oldBlock)
      newBlock->push_back(op.clone(mapper, cloneOptions));

  // Finally now that all operation results have been mapped, set the operands
  // and clone the regions.
  SmallVector<Value> operands;
  for (auto [oldBlock, newBlock] : llvm::zip(blocks, newBlocks)) {
    for (auto [oldOp, newOp] : llvm::zip(*oldBlock, *newBlock)) {
      operands.resize(oldOp.getNumOperands());
      llvm::transform(
          oldOp.getOperands(), operands.begin(),
          [&](Value operand) { return mapper.lookupOrDefault(operand); });
      newOp.setOperands(operands);
      for (auto [oldRegion, newRegion] :
           llvm::zip(oldOp.getRegions(), newOp.getRegions()))
        oldRegion.cloneInto(&newRegion, mapper);
    }
  }
}

//===----------------------------------------------------------------------===//
// Loop Unroller
//===----------------------------------------------------------------------===//

namespace {
/// A data structure tracking information on a single loop.
struct Loop {
  Loop(unsigned loopId, CFGLoop &cfgLoop) : loopId(loopId), cfgLoop(cfgLoop) {}
  bool failMatch(const Twine &msg) const;
  bool match();
  void unroll(CFGLoopInfo &cfgLoopInfo);

  /// A numeric identifier for debugging purposes.
  unsigned loopId;
  /// Loop analysis information about this specific loop.
  CFGLoop &cfgLoop;
  /// The CFG edge exiting the loop.
  BlockOperand *exitEdge = nullptr;
  /// The SSA value holding the exit condition.
  Value exitCondition;
  /// Whether the exit condition is inverted, i.e. the contination condition.
  bool exitInverted;
  /// The induction variable.
  Value indVar;
  /// The updated induction variable passed into the next loop iteration.
  Value indVarNext;
  /// The continuation predicate. The loop continues until the induction
  /// variable compared against the end bound no longer matches this predicate.
  comb::ICmpPredicate predicate;
  /// The induction variable increment.
  APInt indVarIncrement;
  /// The initial value for the induction variable.
  APInt beginBound;
  /// The final value for the induction variable.
  APInt endBound;
  /// The number of iterations of the loop.
  unsigned tripCount = 0;
};
} // namespace

static llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const Loop &loop) {
  os << "#" << loop.loopId << " from ";
  loop.cfgLoop.getHeader()->printAsOperand(os);
  os << " to ";
  loop.cfgLoop.getLoopLatch()->printAsOperand(os);
  return os;
}

/// Helper to print a debug message on match failure and return false.
bool Loop::failMatch(const Twine &msg) const {
  LLVM_DEBUG(llvm::dbgs() << "- Ignoring loop " << *this << ": " << msg
                          << "\n");
  return false;
}

/// Check that the loop matches the specific pattern we understand, and extract
/// the loop condition and induction variable.
bool Loop::match() {
  // Ensure that there is a unique exit point and condition for the loop.
  SmallVector<BlockOperand *> exits;
  for (auto *block : cfgLoop.getBlocks())
    for (auto &edge : block->getTerminator()->getBlockOperands())
      if (!cfgLoop.contains(edge.get()))
        exits.push_back(&edge);
  if (exits.size() != 1)
    return failMatch("multiple exits");
  exitEdge = exits.back();

  // The terminator doing the exit must be a conditional branch.
  auto exitBranch = dyn_cast<cf::CondBranchOp>(exitEdge->getOwner());
  if (!exitBranch)
    return failMatch("unsupported exit branch");
  exitCondition = exitBranch.getCondition();
  exitInverted = exitEdge->getOperandNumber() == 1;

  // Determine one of the loop bounds and the induction variable based on the
  // exit condition.
  if (auto icmpOp = exitCondition.getDefiningOp<comb::ICmpOp>()) {
    IntegerAttr boundAttr;
    if (!matchPattern(icmpOp.getRhs(), m_Constant(&boundAttr)))
      return failMatch("non-constant loop bound");
    indVar = icmpOp.getLhs();
    predicate = icmpOp.getPredicate();
    endBound = boundAttr.getValue();
  } else {
    return failMatch("unsupported exit condition");
  }

  // If the exit condition is not inverted, the predicate is the exit predicate.
  // Negate it such that we have a continuation predicate.
  if (!exitInverted)
    predicate = comb::ICmpOp::getNegatedPredicate(predicate);

  // Determine the initial and next value of the induction variable.
  auto *header = cfgLoop.getHeader();
  auto *latch = cfgLoop.getLoopLatch();
  auto indVarArg = dyn_cast<BlockArgument>(indVar);
  if (!indVarArg || indVarArg.getOwner() != header)
    return failMatch("induction variable is not a header block argument");
  IntegerAttr beginBoundAttr;
  for (auto &pred : header->getUses()) {
    auto branchOp = dyn_cast<BranchOpInterface>(pred.getOwner());
    if (!branchOp)
      return failMatch("header predecessor terminator is not a branch op");
    auto indVarValue = branchOp.getSuccessorOperands(
        pred.getOperandNumber())[indVarArg.getArgNumber()];
    IntegerAttr boundAttr;
    if (pred.getOwner()->getBlock() == latch) {
      indVarNext = indVarValue;
    } else if (matchPattern(indVarValue, m_Constant(&boundAttr))) {
      if (!beginBoundAttr)
        beginBoundAttr = boundAttr;
      else if (boundAttr != beginBoundAttr)
        return failMatch("multiple initial bounds");
    } else {
      return failMatch("unsupported induction variable value");
    }
  }
  if (!beginBoundAttr)
    return failMatch("no initial bound");
  beginBound = beginBoundAttr.getValue();

  // Pattern match the increment operation on the induction variable.
  if (auto addOp = indVarNext.getDefiningOp<comb::AddOp>();
      addOp && addOp.getNumOperands() == 2) {
    if (addOp.getOperand(0) != indVarArg)
      return failMatch("increment LHS not the induction variable");
    IntegerAttr incAttr;
    if (!matchPattern(addOp.getOperand(1), m_Constant(&incAttr)))
      return failMatch("increment RHS non-constant");
    indVarIncrement = incAttr.getValue();
  } else {
    return failMatch("unsupported increment");
  }

  // Evaluate the original fixed-width induction arithmetic. Computing a range
  // in a host unsigned integer truncates wide bounds and can incorrectly turn
  // a very long loop into a short or zero-trip loop. Bounded simulation also
  // preserves signed comparisons and wraparound exactly.
  auto continues = [&](const APInt &value) {
    switch (predicate) {
    case comb::ICmpPredicate::eq:
    case comb::ICmpPredicate::ceq:
    case comb::ICmpPredicate::weq:
      return value == endBound;
    case comb::ICmpPredicate::ne:
    case comb::ICmpPredicate::cne:
    case comb::ICmpPredicate::wne:
      return value != endBound;
    case comb::ICmpPredicate::slt:
      return value.slt(endBound);
    case comb::ICmpPredicate::sle:
      return value.sle(endBound);
    case comb::ICmpPredicate::sgt:
      return value.sgt(endBound);
    case comb::ICmpPredicate::sge:
      return value.sge(endBound);
    case comb::ICmpPredicate::ult:
      return value.ult(endBound);
    case comb::ICmpPredicate::ule:
      return value.ule(endBound);
    case comb::ICmpPredicate::ugt:
      return value.ugt(endBound);
    case comb::ICmpPredicate::uge:
      return value.uge(endBound);
    }
    llvm_unreachable("unknown comparison predicate");
  };
  auto value = beginBound;
  for (tripCount = 0; continues(value); ++tripCount) {
    // Match the SDK's bound: expand fewer than 1024 iterations.
    if (tripCount >= 1023)
      return failMatch("loop exceeds the unrolling budget");
    value += indVarIncrement;
  }

  return true;
}

/// Unroll the loop by cloning its body blocks and replacing the induction
/// variable with constant iteration indices.
void Loop::unroll(CFGLoopInfo &cfgLoopInfo) {
  LLVM_DEBUG(llvm::dbgs() << "- Unrolling loop " << *this << "\n");
  UnusedOpPruner pruner;

  // Sort the blocks in the body. This is not strictly necessary, but makes the
  // pass a lot easier to reason about in tests.
  auto *header = cfgLoop.getHeader();
  SmallVector<Block *> orderedBody;
  for (auto &block : *header->getParent())
    if (cfgLoop.contains(&block))
      orderedBody.push_back(&block);

  // Copy the loop body for every iteration of the loop.
  auto *latch = cfgLoop.getLoopLatch();
  OpBuilder builder(indVar.getContext());
  auto indValue = beginBound;
  for (unsigned trip = 0; trip < tripCount; ++trip) {
    // Clone the loop body.
    IRMapping mapper;
    cloneBlocks(orderedBody, *header->getParent(), header->getIterator(),
                mapper);
    auto *clonedHeader = mapper.lookup(header);
    auto *clonedTail = mapper.lookup(latch);

    // Replace the induction variable with the concrete value.
    auto iterIndVar = mapper.lookup(indVar);
    pruner.eraseLaterIfUnused(iterIndVar);
    builder.setInsertionPointAfterValue(iterIndVar);
    iterIndVar.replaceAllUsesWith(
        hw::ConstantOp::create(builder, iterIndVar.getLoc(), indValue));

    // Update all edges to the original loop header to point to the cloned loop
    // header. Leave the original back-edge untouched.
    for (auto &blockOperand : llvm::make_early_inc_range(header->getUses()))
      if (blockOperand.getOwner()->getBlock() != latch)
        blockOperand.set(clonedHeader);

    // Update the back-edge in the cloned latch to point to the original loop
    // header, i.e. the next iteration, instead of the cloned loop header.
    for (auto &blockOperand : clonedTail->getTerminator()->getBlockOperands())
      if (blockOperand.get() == clonedHeader)
        blockOperand.set(header);

    // Remove the exit edge in the cloned body, since we statically know that
    // the loop will continue.
    auto exitBranchOp =
        cast<cf::CondBranchOp>(mapper.lookup(exitEdge->getOwner()));
    Block *continueDest = exitBranchOp.getTrueDest();
    ValueRange continueDestOperands = exitBranchOp.getTrueDestOperands();
    if (exitEdge->getOperandNumber() == 0) {
      continueDest = exitBranchOp.getFalseDest();
      continueDestOperands = exitBranchOp.getFalseDestOperands();
    }
    builder.setInsertionPoint(exitBranchOp);
    cf::BranchOp::create(builder, exitBranchOp.getLoc(), continueDest,
                         continueDestOperands);
    pruner.eraseLaterIfUnused(exitBranchOp.getOperands());
    exitBranchOp.erase();

    // Add the new blocks to the loop body.
    for (auto *block : orderedBody) {
      auto *newBlock = mapper.lookup(block);
      cfgLoop.addBasicBlockToLoop(newBlock, cfgLoopInfo);
    }

    // Increment the induction variable value.
    indValue += indVarIncrement;
  }

  // Now that the loop body has been cloned once for each trip throughout the
  // loop, we can clean up the final iteration by always breaking out of the
  // loop. Start by replacing the induction variable with the final value.
  pruner.eraseLaterIfUnused(indVar);
  builder.setInsertionPointAfterValue(indVar);
  indVar.replaceAllUsesWith(
      hw::ConstantOp::create(builder, indVar.getLoc(), indValue));
  indVar = {};

  // Remove the continue edge of the exit branch in the loop body, since we
  // statically know that the loop will exit.
  auto exitBranchOp = cast<cf::CondBranchOp>(exitEdge->getOwner());
  Block *exitDest = exitBranchOp.getTrueDest();
  ValueRange exitDestOperands = exitBranchOp.getTrueDestOperands();
  if (exitEdge->getOperandNumber() == 1) {
    exitDest = exitBranchOp.getFalseDest();
    exitDestOperands = exitBranchOp.getFalseDestOperands();
  }
  builder.setInsertionPoint(exitBranchOp);
  cf::BranchOp::create(builder, exitBranchOp.getLoc(), exitDest,
                       exitDestOperands);
  pruner.eraseLaterIfUnused(exitBranchOp.getOperands());
  exitBranchOp.erase();
  exitEdge = nullptr;

  // Prune any body blocks that have become unreachable.
  auto &region = *header->getParent();
  SmallPtrSet<Block *, 8> reachable;
  SmallVector<Block *> worklist;
  reachable.insert(&region.front());
  worklist.push_back(&region.front());
  while (!worklist.empty())
    for (auto *succ : worklist.pop_back_val()->getSuccessors())
      if (reachable.insert(succ).second)
        worklist.push_back(succ);

  SmallVector<Block *> deadBlocks;
  for (auto &block : region) {
    if (reachable.contains(&block))
      continue;
    block.dropAllDefinedValueUses();
    deadBlocks.push_back(&block);
  }

  for (auto *block : deadBlocks) {
    cfgLoopInfo.removeBlock(block);
    block->erase();
  }

  // Remove any unused operations and block arguments.
  pruner.eraseNow();

  // Collapse trivial branches to avoid carrying a lot of useless blocks around
  // especially when unrolling nested loops.
  for (auto &block : *header->getParent()) {
    if (!cfgLoop.contains(&block))
      continue;
    while (true) {
      auto branchOp = dyn_cast<cf::BranchOp>(block.getTerminator());
      if (!branchOp)
        break;
      auto *otherBlock = branchOp.getDest();
      if (!cfgLoop.contains(otherBlock) || !otherBlock->getSinglePredecessor())
        break;
      for (auto [blockArg, branchArg] :
           llvm::zip(otherBlock->getArguments(), branchOp.getDestOperands()))
        blockArg.replaceAllUsesWith(branchArg);
      block.getOperations().splice(branchOp->getIterator(),
                                   otherBlock->getOperations());
      branchOp.erase();
      cfgLoopInfo.removeBlock(otherBlock);
      otherBlock->erase();
    }
  }
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct UnrollLoopsPass
    : public llhd::impl::UnrollLoopsPassBase<UnrollLoopsPass> {
  StringRef getArgument() const override { return "chipyard-unroll-loops"; }
  void runOnOperation() override;
  void runOnOperation(Operation *op);
  bool unrollLoops(Operation *op);
};
} // namespace

void UnrollLoopsPass::runOnOperation() {
  for (auto op : getOperation().getOps<CombinationalOp>())
    runOnOperation(op);
  for (auto op : getOperation().getOps<ProcessOp>())
    runOnOperation(op);
}

void UnrollLoopsPass::runOnOperation(Operation *op) {
  // Unrolling a loop may open up opportunities to unroll its nested loops.
  // Iterate until all possible loops are unrolled or the limit is reached.
  for (unsigned i = 0; i < 16; ++i)
    if (!unrollLoops(op))
      break;
}

bool UnrollLoopsPass::unrollLoops(Operation *op) {
  auto &body = op->getRegion(0);
  // There's nothing to do if we only have a single block. MLIR even refuses to
  // compute a dominator tree in that case.
  if (body.hasOneBlock())
    return false;

  // Find the loops.
  LLVM_DEBUG(llvm::dbgs() << "Unrolling loops in " << op->getLoc() << "\n");
  DominanceInfo domInfo(op);
  CFGLoopInfo cfgLoopInfo(domInfo.getDomTree(&body));

  // We only support simple loops where there is a single back-edge to the
  // header, and the latch block has a back-edge to a single header. Create a
  // data structure for each loop we can potentially unroll. The loops are in
  // preorder, with outer loops appearing before their child loops.
  SmallVector<Loop> loops;
  bool retry = false;
  for (auto *cfgLoop : cfgLoopInfo.getLoopsInPreorder()) {
    // To simplify unrolling we need a unique latch block branching back to the
    // header.
    auto *header = cfgLoop->getHeader();
    auto *latch = cfgLoop->getLoopLatch();
    if (!latch)
      continue;

    LLVM_DEBUG({
      llvm::dbgs() << "- ";
      cfgLoop->print(llvm::dbgs(), false, false);
      llvm::dbgs() << "\n";
    });
    Loop loop(loops.size(), *cfgLoop);

    // Ensure that the header block is only a header for the current loop. This
    // simplifies unrolling.
    auto *parent = cfgLoop->getParentLoop();
    while (parent && parent->getHeader() != header)
      parent = parent->getParentLoop();
    if (parent) {
      loop.failMatch("header block shared across multiple loops");
      continue;
    }

    // Ensure that the latch block is only a latch for the current loop. This
    // simplifies unrolling.
    parent = cfgLoop->getParentLoop();
    while (parent && !parent->isLoopLatch(latch))
      parent = parent->getParentLoop();
    if (parent) {
      loop.failMatch("latch block shared across multiple loops");
      continue;
    }

    // Check if the loop body matches the pattern we can unroll.
    if (loop.match()) {
      loops.push_back(std::move(loop));
      continue;
    }

    // If an enclosing loop is unrolled, we retry the unrolling.
    retry |= llvm::any_of(loops, [&](const Loop &other) {
      return other.cfgLoop.contains(cfgLoop);
    });
  }

  if (loops.empty())
    return false;

  // Dump some debugging information about the loops we've found.
  LLVM_DEBUG({
    auto &os = llvm::dbgs();
    for (auto &loop : loops) {
      os << "- Loop " << loop << ":\n";
      os << "  - ";
      loop.cfgLoop.print(os, false, false);
      os << "\n";
      os << "  - Exit: ";
      loop.exitEdge->get()->printAsOperand(os);
      os << " if ";
      if (loop.exitInverted)
        os << "not ";
      os << loop.exitCondition;
      os << "\n";
      os << "  - Induction variable: ";
      loop.indVar.printAsOperand(os, OpPrintingFlags().useLocalScope());
      os << ", from " << loop.beginBound << ", while " << loop.predicate << " "
         << loop.endBound << ", increment " << loop.indVarIncrement << "\n";
      os << "  - Trip count: " << loop.tripCount << "\n";
    }
  });

  // Unroll the loops. Handling the loops in reverse unrolls inner loops before
  // their parent loops.
  for (auto &loop : llvm::reverse(loops))
    loop.unroll(cfgLoopInfo);

  return retry;
}

namespace {
// These modules describe native HW interfaces for the Verilog type checker.
// Externalize them before any hierarchy inlining: their bodies are never a
// simulation implementation. The host importer reconnects each declaration to
// the existing native HW module, adapting its ports in both directions.
struct NativeBoundaries
    : PassWrapper<NativeBoundaries, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeBoundaries)
  StringRef getArgument() const override {
    return "chipyard-native-boundaries";
  }
  void runOnOperation() override {
    for (auto module :
         llvm::make_early_inc_range(getOperation().getOps<hw::HWModuleOp>())) {
      auto attrs = sv::getSVAttributes(module);
      if (!attrs || !llvm::any_of(attrs, [](Attribute attr) {
            return cast<sv::SVAttributeAttr>(attr).getName() ==
                   "chipyard_arc_native_boundary";
          }))
        continue;
      OpBuilder builder(module);
      auto external = hw::HWModuleExternOp::create(
          builder, module.getLoc(), module.getModuleNameAttr(),
          module.getPortList(), StringRef(), module.getParameters());
      external.setPrivate();
      module.erase();
    }
  }
};

// Complete the SDK's register-preset inference after finite startup loops have
// exposed constant drives. The SDK's desequencer may already have created the
// register before those loops became constants, and its preset is integer-only.
// Pack aggregate registers while preserving their exact bit layout and all
// register attributes. Only concurrent immediate constant drives are presets;
// conditional, delayed, aliased, or additional nonconstant drivers stay intact.
struct InferLatePresets
    : PassWrapper<InferLatePresets, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InferLatePresets)
  StringRef getArgument() const override {
    return "chipyard-infer-late-presets";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<seq::SeqDialect, hw::HWDialect>();
  }
  bool constantBits(Value value, APInt &bits) {
    Attribute attr;
    if (!matchPattern(value, m_Constant(&attr)))
      return false;
    if (auto integer = dyn_cast<IntegerAttr>(attr)) {
      bits = integer.getValue();
      return true;
    }
    auto array = dyn_cast<ArrayAttr>(attr);
    return array &&
           succeeded(hw::aggregateAttrToAPInt(value.getType(), array, bits));
  }
  LogicalResult infer(llhd::SignalOp signal) {
    SmallVector<llhd::DriveOp> initializers;
    llhd::DriveOp clocked;
    std::optional<APInt> preset;
    bool conflicting = false;
    for (auto *user : signal->getUsers()) {
      if (user->getBlock() != signal->getBlock())
        return success();
      if (isa<llhd::ProbeOp>(user))
        continue;
      auto drive = dyn_cast<llhd::DriveOp>(user);
      if (!drive || drive.getEnable())
        return success();
      llhd::TimeAttr time;
      if (!matchPattern(drive.getTime(), m_Constant(&time)) || time.getTime() ||
          time.getDelta() || time.getEpsilon() != 1)
        return success();
      APInt value;
      if (constantBits(drive.getValue(), value)) {
        conflicting |= preset && *preset != value;
        preset = value;
        initializers.push_back(drive);
      } else {
        if (clocked)
          return success();
        clocked = drive;
      }
    }
    if (!clocked || !preset)
      return success();
    auto reg = clocked.getValue().getDefiningOp<seq::FirRegOp>();
    if (!reg || reg->getBlock() != signal->getBlock())
      return success();
    if (conflicting)
      return signal.emitError("conflicting constant register initializers");
    if (auto previous = reg.getPresetAttr())
      if (previous.getValue() != *preset)
        return signal.emitError("register preset conflicts with initializer");
    OpBuilder builder(reg);
    auto type = builder.getIntegerType(preset->getBitWidth());
    auto loc = reg.getLoc();
    auto packed = [&](Value value) -> Value {
      return builder.createOrFold<hw::BitcastOp>(loc, type, value);
    };
    if (reg.getType() != type) {
      auto next = packed(reg.getNext());
      auto reset = reg.getResetValue() ? packed(reg.getResetValue()) : Value{};
      auto replacement = cast<seq::FirRegOp>(builder.clone(*reg));
      replacement.getNextMutable().assign(next);
      if (reset)
        replacement.getResetValueMutable().assign(reset);
      replacement.getResult().setType(type);
      Value unpacked = builder.createOrFold<hw::BitcastOp>(
          loc, reg.getType(), replacement.getResult());
      reg.replaceAllUsesWith(unpacked);
      reg.erase();
      reg = replacement;
    }
    reg.setPresetAttr(builder.getIntegerAttr(type, *preset));
    for (auto drive : initializers)
      drive.erase();
    return success();
  }
  void runOnOperation() override {
    for (auto signal : getOperation().getOps<llhd::SignalOp>())
      if (failed(infer(signal))) {
        signalPassFailure();
        return;
      }
  }
};

// Generate loops can emit the same continuous constant assignment several
// times. The SDK signal promoter expects one whole-value driver. Coalesce only
// signals whose entire driver set agrees on the same immediate constant: keep
// dynamic, conditional, delayed, conflicting, or escaped driver sets intact.
struct CoalesceConstantDrives
    : PassWrapper<CoalesceConstantDrives, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CoalesceConstantDrives)
  StringRef getArgument() const override {
    return "chipyard-coalesce-constant-drives";
  }
  void runOnOperation() override {
    unsigned count = 0;
    for (auto signal : getOperation().getOps<llhd::SignalOp>()) {
      SmallVector<llhd::DriveOp> drives;
      Attribute constant;
      bool unsupported = false;
      for (auto *user : signal->getUsers()) {
        if (user->getBlock() != signal->getBlock()) {
          unsupported = true;
          break;
        }
        if (isa<llhd::ProbeOp>(user))
          continue;
        auto drive = dyn_cast<llhd::DriveOp>(user);
        llhd::TimeAttr time;
        Attribute value;
        if (!drive || drive.getEnable() ||
            !matchPattern(drive.getTime(), m_Constant(&time)) ||
            time.getTime() || time.getDelta() || time.getEpsilon() != 1 ||
            !matchPattern(drive.getValue(), m_Constant(&value)) ||
            (constant && constant != value)) {
          unsupported = true;
          break;
        }
        constant = value;
        drives.push_back(drive);
      }
      if (unsupported || drives.size() < 2)
        continue;
      for (auto drive : llvm::drop_begin(drives))
        drive.erase();
      ++count;
    }
    if (count)
      llvm::errs() << "Coalesced identical constant drivers on " << count
                   << " signals\n";
  }
};

// The SDK drive hoister only processes llhd.process regions. Its earlier
// lowering can leave writes inside llhd.combinational regions, preventing
// control-flow removal and signal promotion. Temporarily expose those regions
// to the same hoister, then restore their combinational execution semantics.
// No optimization or process scheduling runs while the terminators are halts.
struct HoistCombinationalDrives
    : PassWrapper<HoistCombinationalDrives, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(HoistCombinationalDrives)
  StringRef getArgument() const override {
    return "chipyard-hoist-combinational-drives";
  }
  void runOnOperation() override {
    constexpr StringLiteral marker = "chipyard.restore_combinational";
    bool changed = false;
    for (auto comb : llvm::make_early_inc_range(
             getOperation().getOps<llhd::CombinationalOp>())) {
      bool hasDrive = false;
      comb.walk([&](llhd::DriveOp) { hasDrive = true; });
      if (!hasDrive)
        continue;
      OpBuilder builder(comb);
      auto process = llhd::ProcessOp::create(builder, comb.getLoc(),
                                             comb.getResultTypes());
      process->setAttrs(comb->getAttrs());
      process->setAttr(marker, builder.getUnitAttr());
      process.getBody().takeBody(comb.getBody());
      for (auto &block : process.getBody())
        if (auto yield = dyn_cast<llhd::YieldOp>(block.getTerminator())) {
          builder.setInsertionPoint(yield);
          llhd::HaltOp::create(builder, yield.getLoc(), yield.getOperands());
          yield.erase();
        }
      comb.replaceAllUsesWith(process.getResults());
      comb.erase();
      changed = true;
    }
    if (!changed)
      return;
    OpPassManager pipeline(hw::HWModuleOp::getOperationName());
    pipeline.addPass(llhd::createHoistSignalsPass());
    if (failed(runPipeline(pipeline, getOperation()))) {
      signalPassFailure();
      return;
    }
    for (auto process :
         llvm::make_early_inc_range(getOperation().getOps<llhd::ProcessOp>())) {
      if (!process->hasAttr(marker))
        continue;
      OpBuilder builder(process);
      auto comb = llhd::CombinationalOp::create(builder, process.getLoc(),
                                                process.getResultTypes());
      comb->setAttrs(process->getAttrs());
      comb->removeAttr(marker);
      comb.getBody().takeBody(process.getBody());
      for (auto &block : comb.getBody())
        if (auto halt = dyn_cast<llhd::HaltOp>(block.getTerminator())) {
          builder.setInsertionPoint(halt);
          llhd::YieldOp::create(builder, halt.getLoc(), halt.getOperands());
          halt.erase();
        }
      process.replaceAllUsesWith(comb.getResults());
      process.erase();
    }
  }
};

// A process result denotes its most recently yielded value. Capturing that
// result as an external coroutine argument creates a false combinational loop.
// Carry it across suspension edges explicitly instead. Before the first wait
// there is no yielded value: reject reads on such paths rather than invent one.
static LogicalResult threadProcessResults(ProcessOp process) {
  SmallVector<unsigned> results;
  SmallVector<SmallVector<OpOperand *>> uses;
  for (auto result : process.getResults()) {
    SmallVector<OpOperand *> internal;
    for (auto &use : result.getUses())
      if (process->isProperAncestor(use.getOwner()))
        internal.push_back(&use);
    if (!internal.empty()) {
      results.push_back(result.getResultNumber());
      uses.push_back(std::move(internal));
    }
  }
  if (results.empty())
    return success();

  auto &body = process.getBody();
  SmallPtrSet<Block *, 8> beforeWait;
  SmallVector<Block *> work{&body.front()};
  while (!work.empty()) {
    auto *block = work.pop_back_val();
    if (!beforeWait.insert(block).second || isa<WaitOp>(block->getTerminator()))
      continue;
    for (auto *successor : block->getSuccessors())
      work.push_back(successor);
  }
  for (auto &list : uses)
    for (auto *use : list)
      if (beforeWait.contains(
              body.findAncestorBlockInRegion(*use->getOwner()->getBlock())))
        return process.emitError("reads its own result before yielding it");

  DenseMap<Block *, SmallVector<Value>> local;
  for (auto &block : body)
    if (!beforeWait.contains(&block))
      for (auto index : results)
        local[&block].push_back(block.addArgument(
            process.getResult(index).getType(), process.getLoc()));

  // Rewrite saved operand pointers before appending branch operands, which
  // may reallocate the terminator's operand storage.
  for (auto [index, list] : llvm::enumerate(uses))
    for (auto *use : list) {
      auto *block =
          body.findAncestorBlockInRegion(*use->getOwner()->getBlock());
      use->set(local.lookup(block)[index]);
    }
  for (auto &block : body) {
    auto *terminator = block.getTerminator();
    if (auto wait = dyn_cast<WaitOp>(terminator)) {
      if (local.count(wait.getDest()))
        for (auto index : results)
          wait.getDestOperandsMutable().append(wait.getYieldOperands()[index]);
      continue;
    }
    if (auto branch = dyn_cast<BranchOpInterface>(terminator))
      for (unsigned i = 0; i < terminator->getNumSuccessors(); ++i)
        if (local.count(terminator->getSuccessor(i)))
          branch.getSuccessorOperands(i).append(local.lookup(&block));
  }
  return success();
}

// Physical times passed across a wait can be stored as their original i64
// femtosecond value. Arc's frame allocator cannot size !llhd.time fields. Only
// unwrap arguments whose incoming values are all int_to_time conversions;
// retain general LLHD times with delta/epsilon components unchanged.
static void normalizePhysicalTimeArguments(ProcessOp process) {
  bool changed;
  do {
    changed = false;
    for (auto &block : process.getBody()) {
      if (block.hasNoPredecessors())
        continue;
      for (auto arg : block.getArguments()) {
        if (!isa<llhd::TimeType>(arg.getType()))
          continue;
        SmallVector<std::pair<OpOperand *, Value>> incoming;
        bool unsupported = false;
        for (auto &edge : block.getUses()) {
          auto *terminator = edge.getOwner();
          unsigned index;
          if (auto wait = dyn_cast<WaitOp>(terminator))
            index = wait.getDestOperands().getBeginOperandIndex() +
                    arg.getArgNumber();
          else if (auto branch = dyn_cast<BranchOpInterface>(terminator)) {
            auto operands =
                branch.getSuccessorOperands(edge.getOperandNumber());
            if (operands.isOperandProduced(arg.getArgNumber())) {
              unsupported = true;
              break;
            }
            index = operands.getOperandIndex(arg.getArgNumber());
          } else {
            unsupported = true;
            break;
          }
          auto &operand = terminator->getOpOperand(index);
          auto cast = operand.get().getDefiningOp<llhd::IntToTimeOp>();
          if (!cast) {
            unsupported = true;
            break;
          }
          incoming.emplace_back(&operand, cast.getOperand());
        }
        if (unsupported || incoming.empty())
          continue;
        OpBuilder builder = OpBuilder::atBlockBegin(&block);
        arg.setType(builder.getI64Type());
        auto time = llhd::IntToTimeOp::create(builder, arg.getLoc(), arg);
        arg.replaceAllUsesExcept(time, time);
        for (auto [operand, value] : incoming)
          operand->set(value);
        changed = true;
      }
    }
  } while (changed);
}

// An activation-local time read is an ordinary data input to its process. Move
// it to the enclosing graph so register inference can consume it. A direct SSA
// use across a wait must retain the earlier sampled time; leave those reads in
// the coroutine. Values passed through explicit block arguments remain latched
// at the original use point, even when the time read is outside the process.
static void hoistActivationTimeReads(ProcessOp process) {
  SmallVector<arc::CurrentTimeOp> times, movable;
  for (auto &block : process.getBody())
    for (auto &op : block)
      if (auto time = dyn_cast<arc::CurrentTimeOp>(op))
        times.push_back(time);
  if (times.empty())
    return;
  Liveness liveness(process);
  for (auto time : times) {
    bool acrossWait = llvm::any_of(process.getBody(), [&](Block &block) {
      return isa<WaitOp>(block.getTerminator()) &&
             liveness.getLiveOut(&block).count(time.getResult());
    });
    auto context = time.getArcContext();
    if (!acrossWait &&
        (context.getDefiningOp<arc::InferredContextOp>() ||
         context.getParentRegion()->isProperAncestor(&process.getBody())))
      movable.push_back(time);
  }
  OpBuilder builder(process);
  for (auto time : movable) {
    Value context = time.getArcContext();
    if (!context.getParentRegion()->isProperAncestor(&process.getBody()))
      context = arc::InferredContextOp::create(builder, time.getLoc());
    Value replacement =
        arc::CurrentTimeOp::create(builder, time.getLoc(), context);
    time.replaceAllUsesWith(replacement);
    time.erase();
  }
}

// Inline a task only after proving its entire call graph cannot suspend.
// Cloning its CFG at the call site retains branches, return values, references
// and side effects at the original process activation.
struct TaskInliner : InlinerInterface {
  using InlinerInterface::InlinerInterface;
  bool isLegalToInline(Operation *call, Operation *callee,
                       bool) const override {
    return isa<llhd::CoroutineOp>(callee) &&
           mayHaveSSADominance(*call->getParentRegion()) &&
           call->getParentWithTrait<ProceduralRegion>();
  }
  bool isLegalToInline(Region *, Region *, bool, IRMapping &) const override {
    return true;
  }
  bool isLegalToInline(Operation *, Region *, bool,
                       IRMapping &) const override {
    return true;
  }
  bool shouldAnalyzeRecursively(Operation *) const override { return false; }
  bool allowSingleBlockOptimization(
      iterator_range<Region::iterator> blocks) const override {
    return !llvm::hasSingleElement(blocks) ||
           isa<llhd::ReturnOp>(blocks.begin()->getTerminator());
  }
  void processInlinedCallBlocks(
      Operation *call, iterator_range<Region::iterator> blocks) const override {
    chipyard::carryScopeFrames(call, blocks);
    auto lookup = call->getAttrOfType<StringAttr>("chipyard.sv_call_lookup");
    if (!lookup || lookup.getValue() != "absolute")
      return;
    for (auto &block : blocks)
      for (auto &op : block)
        op.walk([&](Operation *nested) {
          if (isa<sim::FormatHierPathOp>(nested))
            nested->setAttr("chipyard.sv_format_lookup", lookup);
          auto key = isa<sim::DPICallOp>(nested) ? "chipyard.dpi_context_lookup"
                                                 : "chipyard.sv_call_lookup";
          if (auto origin = nested->getAttrOfType<StringAttr>(key);
              origin && origin.getValue() == "lexical")
            nested->setAttr(key, lookup);
        });
  }
  void handleTerminator(Operation *op, Block *continuation) const override {
    if (isa<ub::UnreachableOp>(op))
      return;
    if (!isa<llhd::ReturnOp>(op))
      return InlinerInterface::handleTerminator(op, continuation);
    OpBuilder builder(op);
    cf::BranchOp::create(builder, op->getLoc(), continuation,
                         op->getOperands());
    op->erase();
  }
  void handleTerminator(Operation *op, ValueRange results) const override {
    if (isa<ub::UnreachableOp>(op))
      return;
    if (!isa<llhd::ReturnOp>(op))
      return InlinerInterface::handleTerminator(op, results);
    for (auto [result, value] : llvm::zip(results, op->getOperands()))
      result.replaceAllUsesWith(value);
  }
};

struct InstantTasks : PassWrapper<InstantTasks, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InstantTasks)
  StringRef getArgument() const override {
    return "chipyard-inline-instant-tasks";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<cf::ControlFlowDialect>();
  }
  void runOnOperation() override {
    // A fixed point proves every transitive callee synchronous. Recursive,
    // unknown, or suspending call graphs never enter the eligible set.
    SmallVector<Operation *> pending;
    getOperation().walk([&](Operation *op) {
      if (isa<llhd::CoroutineOp, func::FuncOp>(op))
        pending.push_back(op);
    });
    llvm::SmallPtrSet<Operation *, 32> eligible;
    SymbolTableCollection symbols;
    bool changed;
    do {
      changed = false;
      for (auto *callee : pending) {
        if (eligible.contains(callee))
          continue;
        bool safe = true;
        callee->walk([&](Operation *op) {
          if (op == callee)
            return;
          if (isa<llhd::WaitOp, llhd::HaltOp>(op) ||
              op->hasTrait<ProceduralRegion>())
            safe = false;
          if (auto call = dyn_cast<CallOpInterface>(op)) {
            auto *target = call.resolveCallableInTable(&symbols);
            if (!target ||
                (!isa<sim::DPIFuncOp>(target) && !eligible.contains(target)))
              safe = false;
          }
        });
        // An external C function cannot suspend an LLHD process. Empty LLHD
        // coroutines, however, have no implementation to inline.
        if (isa<llhd::CoroutineOp>(callee) && callee->getRegion(0).empty())
          safe = false;
        if (safe) {
          eligible.insert(callee);
          changed = true;
        }
      }
    } while (changed);

    TaskInliner inliner(&getContext());
    InlinerConfig config;
    unsigned count = 0;
    do {
      SmallVector<llhd::CallCoroutineOp> calls;
      getOperation().walk([&](llhd::CallCoroutineOp call) {
        auto *target = call.resolveCallableInTable(&symbols);
        if (target && eligible.contains(target))
          calls.push_back(call);
      });
      changed = !calls.empty();
      for (auto call : calls) {
        auto task =
            cast<llhd::CoroutineOp>(call.resolveCallableInTable(&symbols));
        if (failed(inlineCall(inliner, config.getCloneCallback(), call, task,
                              &task.getBody()))) {
          call.emitError("cannot inline non-suspending task");
          signalPassFailure();
          return;
        }
        call.erase();
        ++count;
      }
    } while (changed);
    // Their original templates carry no physical call frame and must not be
    // rebound as if they were live context-DPI callers after all uses inline.
    SmallVector<llhd::CoroutineOp> unused;
    for (auto task : getOperation().getOps<llhd::CoroutineOp>())
      if (eligible.contains(task) && task.isPrivate() &&
          SymbolTable::symbolKnownUseEmpty(task, getOperation()))
        unused.push_back(task);
    for (auto task : unused)
      task.erase();
    if (count)
      llvm::errs() << "Inlined " << count << " non-suspending task calls\n";
  }
};

struct SplitPrints : PassWrapper<SplitPrints, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitPrints)
  StringRef getArgument() const override {
    return "chipyard-prepare-rtl-processes";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arc::ArcDialect, sim::SimDialect>();
  }
  void runOnOperation() override {
    SmallVector<llhd::CurrentTimeOp> times;
    getOperation().walk([&](llhd::CurrentTimeOp op) { times.push_back(op); });
    for (auto time : times) {
      OpBuilder builder(time);
      Value context = arc::InferredContextOp::create(builder, time.getLoc());
      Value value = arc::CurrentTimeOp::create(builder, time.getLoc(), context);
      Value converted =
          llhd::IntToTimeOp::create(builder, time.getLoc(), value);
      time.replaceAllUsesWith(converted);
      time.erase();
    }
    SmallVector<sim::FormatStringOp> formats;
    getOperation().walk([&](sim::FormatStringOp op) { formats.push_back(op); });
    for (auto format : formats) {
      auto constant = format.getValue().getDefiningOp<sim::StringConstantOp>();
      if (!constant || format.getSpecifierWidth().value_or(0) != 0)
        continue;
      OpBuilder builder(format);
      auto literal = sim::FormatLiteralOp::create(builder, format.getLoc(),
                                                  constant.getLiteralAttr());
      format.replaceAllUsesWith(literal.getResult());
      format.erase();
      if (constant->use_empty())
        constant.erase();
    }
    SmallVector<ProcessOp> work;
    for (auto process : getOperation().getOps<ProcessOp>())
      if (process.getNumResults())
        work.push_back(process);
    for (auto process : work) {
      normalizePhysicalTimeArguments(process);
      hoistActivationTimeReads(process);
      if (failed(threadProcessResults(process))) {
        signalPassFailure();
        return;
      }
      SmallVector<Operation *> prints;
      bool unsupported = false;
      for (auto &block : process.getBody()) {
        for (auto &op : block) {
          if (isa<sim::PrintFormattedProcOp>(op))
            prints.push_back(&op);
          // Context time is fixed throughout an evaluation, including staged
          // native clock updates. Reading it in the diagnostic clone yields
          // the same timestamp; arbitrary memory reads remain unsupported.
          else if (!isa<WaitOp, HaltOp, arc::CurrentTimeOp>(op) &&
                   !isMemoryEffectFree(&op))
            unsupported = true;
        }
      }
      if (unsupported || prints.empty())
        continue;
      OpBuilder builder(process);
      auto effects = ProcessOp::create(builder, process.getLoc(), TypeRange{});
      IRMapping mapping;
      process.getBody().cloneInto(&effects.getBody(), mapping);
      for (auto &block : effects.getBody()) {
        if (auto wait = dyn_cast<WaitOp>(block.getTerminator()))
          wait.getYieldOperandsMutable().clear();
        else if (auto halt = dyn_cast<HaltOp>(block.getTerminator()))
          halt.getYieldOperandsMutable().clear();
      }
      for (auto *print : prints)
        print->erase();
    }
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK ::mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardRTL", "1", []() {
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<RestoreDPIABI>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<BitcastFloatSignals>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<BitcastFloatProcesses>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<InstantTasks>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<UnrollLoopsPass>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<SplitPrints>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<NativeBoundaries>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<InferLatePresets>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<CoalesceConstantDrives>();
            });
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<HoistCombinationalDrives>();
            });
          }};
}
