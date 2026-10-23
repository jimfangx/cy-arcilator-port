// Keep sensitized combinational DPI caches frozen during native sampling.
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <limits>
using namespace mlir;
using namespace circt;
namespace {
struct SamplingFreeze : PassWrapper<SamplingFreeze, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SamplingFreeze)
  StringRef getArgument() const override {
    return "chipyard-freeze-combinational-dpi";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arc::ArcDialect, comb::CombDialect, hw::HWDialect,
                    func::FuncDialect, scf::SCFDialect>();
  }
  bool contains(const DenseSet<Operation *> &members, Block *block,
                Operation *operation) {
    while (operation && operation->getBlock() != block)
      operation = operation->getParentOp();
    return operation && members.contains(operation);
  }
  bool freeze(arc::CoroutineCallOp call, unsigned phaseIndex) {
    if (phaseIndex >= call.getArgs().size())
      return false;
    auto phase = call.getArgs()[phaseIndex];
    if (!phase.getType().isInteger(8))
      return false;
    auto condition = dyn_cast<scf::IfOp>(call->getParentOp());
    if (!condition || condition.getNumResults() || condition.elseBlock())
      return false;
    Block *block = condition->getBlock();
    DenseSet<Value> inputs(call.getArgs().begin(), call.getArgs().end());
    DenseSet<Operation *> visited;
    SmallVector<Value> work{condition.getCondition()};
    SmallVector<arc::StateReadOp> reads;
    Value wakeupSlot;
    while (!work.empty()) {
      Value value = work.pop_back_val();
      if (inputs.contains(value))
        continue;
      auto *op = value.getDefiningOp();
      if (!op || !visited.insert(op).second)
        continue;
      if (auto read = dyn_cast<arc::StateReadOp>(op)) {
        if (read->getBlock() != block ||
            !read.getState().getDefiningOp<arc::AllocStateOp>())
          return false;
        reads.push_back(read);
        continue;
      }
      if (auto compare = dyn_cast<comb::ICmpOp>(op))
        if (compare.getPredicate() == comb::ICmpPredicate::uge &&
            compare.getLhs().getDefiningOp<arc::CurrentTimeOp>()) {
          auto read = compare.getRhs().getDefiningOp<arc::StateReadOp>();
          if (!read || (wakeupSlot && wakeupSlot != read.getState()))
            return false;
          wakeupSlot = read.getState();
        }
      if (isa<arc::CurrentTimeOp>(op))
        continue;
      if (!isMemoryEffectFree(op))
        return false;
      llvm::append_range(work, op->getOperands());
    }
    if (!wakeupSlot || reads.empty())
      return false;
    Operation *first = condition;
    DenseSet<Value> owned;
    for (auto read : reads) {
      owned.insert(read.getState());
      if (read->isBeforeInBlock(first))
        first = read;
      for (auto *user : read.getState().getUsers())
        if (!isa<arc::StateReadOp, arc::StateWriteOp>(user))
          return false;
    }
    SmallVector<Operation *> range;
    for (auto it = first->getIterator();; ++it) {
      range.push_back(&*it);
      if (&*it == condition)
        break;
      if (&*it == block->getTerminator())
        return false;
    }
    DenseSet<Operation *> members(range.begin(), range.end());
    unsigned previousWrites = 0;
    SmallVector<arc::StateWriteOp> observedWrites;
    for (auto *op : range) {
      if (op == condition)
        continue;
      if (auto read = dyn_cast<arc::StateReadOp>(op)) {
        if (!owned.contains(read.getState()))
          return false;
      } else if (auto write = dyn_cast<arc::StateWriteOp>(op)) {
        if (!owned.contains(write.getState()) ||
            !inputs.contains(write.getValue()))
          return false;
        ++previousWrites;
        if (write.getValue() != phase) {
          int64_t width = hw::getBitWidth(write.getValue().getType());
          if (width < 0)
            return false;
          if (width)
            observedWrites.push_back(write);
        }
      } else if (!isa<arc::CurrentTimeOp>(op) && !isMemoryEffectFree(op))
        return false;
      for (Value result : op->getResults())
        for (auto *user : result.getUsers())
          if (!contains(members, block, user))
            return false;
    }
    if (!previousWrites)
      return false;
    // Fresh arguments must dominate the entire guarded range. LowerState
    // samples them before building these per-instance change detectors.
    for (Value value : call.getArgs())
      if (auto *producer = value.getDefiningOp())
        if (producer->getBlock() == block && !producer->isBeforeInBlock(first))
          return false;
    OpBuilder builder(first);
    auto loc = call.getLoc();
    auto zero8 = hw::ConstantOp::create(builder, loc, APInt(8, 0));
    auto combinational = comb::ICmpOp::create(
        builder, loc, comb::ICmpPredicate::eq, phase, zero8);
    // Preserve the original first-evaluation behavior even if a direct model
    // driver starts in sampling mode: its initial wakeup is zero, not never.
    auto wakeup = arc::StateReadOp::create(builder, loc, wakeupSlot);
    auto zero64 = hw::ConstantOp::create(builder, loc, APInt(64, 0));
    auto initial = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq,
                                        wakeup, zero64);
    Value enabled = comb::OrOp::create(builder, loc, combinational, initial);
    // A real input transition during sampling must still enter the original
    // frozen wait. Its pending activation survives even if the input later
    // returns to the pre-sampling value. Skip only phase-only observation.
    for (auto write : observedWrites) {
      Value fresh = write.getValue();
      Value old = arc::StateReadOp::create(builder, loc, write.getState());
      if (!isa<IntegerType>(fresh.getType())) {
        auto type = builder.getIntegerType(hw::getBitWidth(fresh.getType()));
        fresh = hw::BitcastOp::create(builder, loc, type, fresh);
        old = hw::BitcastOp::create(builder, loc, type, old);
      }
      auto changed = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::ne,
                                          old, fresh);
      enabled = comb::OrOp::create(builder, loc, enabled, changed);
    }
    auto outer = scf::IfOp::create(builder, loc, enabled, false);
    for (auto *op : range)
      op->moveBefore(outer.thenYield());
    return true;
  }
  void runOnOperation() override {
    auto module = getOperation();
    auto references =
        module->getAttrOfType<ArrayAttr>("chipyard.native_sampling_markers");
    if (!references)
      return;
    SmallVector<func::FuncOp> declarations;
    DenseSet<StringAttr> markerNames;
    for (Attribute reference : references) {
      auto name = dyn_cast<FlatSymbolRefAttr>(reference);
      auto marker = name ? module.lookupSymbol<func::FuncOp>(name.getValue())
                         : func::FuncOp{};
      if (!marker || !marker.isExternal() || marker.getNumArguments() != 1 ||
          marker.getNumResults() != 1 ||
          !marker.getArgumentTypes()[0].isInteger(8) ||
          marker.getResultTypes()[0] != marker.getArgumentTypes()[0] ||
          !marker->hasAttr("chipyard.native_sampling_identity")) {
        module.emitError("invalid native sampling marker provenance");
        return signalPassFailure();
      }
      declarations.push_back(marker);
      markerNames.insert(marker.getSymNameAttr());
    }
    constexpr unsigned invalid = std::numeric_limits<unsigned>::max();
    DenseMap<Operation *, unsigned> phases;
    SmallVector<func::CallOp> markers;
    module.walk([&](func::CallOp marker) {
      if (!markerNames.contains(marker.getCalleeAttr().getAttr()))
        return;
      markers.push_back(marker);
      auto coroutine = marker->getParentOfType<arc::CoroutineDefineOp>();
      if (!coroutine)
        return;
      auto argument = dyn_cast<BlockArgument>(marker.getOperand(0));
      unsigned index = invalid;
      if (argument &&
          argument.getArgNumber() < coroutine.getArgumentTypes().size() &&
          argument.getType().isInteger(8))
        index = argument.getArgNumber();
      auto found = phases.find(coroutine);
      if (found == phases.end())
        phases[coroutine] = index;
      else if (found->second != index)
        found->second = invalid;
    });
    SmallVector<arc::CoroutineCallOp> calls;
    module.walk([&](arc::CoroutineCallOp call) { calls.push_back(call); });
    unsigned frozen = 0, fallback = 0;
    for (auto call : calls) {
      auto coroutine = module.lookupSymbol<arc::CoroutineDefineOp>(
          call.getCalleeAttr().getValue());
      auto phase = phases.find(coroutine);
      if (phase == phases.end())
        continue;
      if (phase->second != invalid && freeze(call, phase->second))
        ++frozen;
      else
        ++fallback;
    }
    for (auto marker : markers) {
      marker.getResult(0).replaceAllUsesWith(marker.getOperand(0));
      marker.erase();
    }
    module->removeAttr("chipyard.native_sampling_markers");
    for (auto marker : declarations) {
      if (SymbolTable::symbolKnownUseEmpty(marker, module))
        marker.erase();
      else {
        // Unknown symbol users retain an actual identity implementation,
        // never an unresolved C/C++ dependency.
        auto *entry = marker.addEntryBlock();
        OpBuilder builder(entry, entry->end());
        func::ReturnOp::create(builder, marker.getLoc(), entry->getArgument(0));
        marker->removeAttr("chipyard.native_sampling_identity");
      }
    }
    llvm::errs() << "Froze " << frozen
                 << " combinational DPI coroutine instances; " << fallback
                 << " conservative fallbacks\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardSamplingFreeze", "1",
          [] { PassRegistration<SamplingFreeze>(); }};
}
