// Certify an idle sampling transition from allocated Arc IR.
// The runtime calls this only on a settled snapshot after changing exactly
// the declared phase-clock inputs. Unknown effects retain normal evaluation.
#include "circt/Dialect/Arc/ArcConstants.h"
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <optional>
using namespace mlir;
using namespace circt;
namespace {
struct IdleClock : PassWrapper<IdleClock, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(IdleClock)
  IdleClock() = default;
  IdleClock(const IdleClock &other) : PassWrapper(other) {}
  Option<bool> enabled{*this, "enabled", llvm::cl::init(true)};
  Option<std::string> heldInputs{*this, "held-inputs", llvm::cl::init("")};
  Option<bool> stableEvaluator{*this, "stable-evaluator",
                               llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-certify-idle-clock";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<func::FuncDialect, scf::SCFDialect, hw::HWDialect,
             comb::CombDialect, arc::ArcDialect, arith::ArithDialect>();
  }
  struct Range {
    uint64_t start, bytes;
    Type type;
    Operation *op;
  };
  DenseMap<Value, Range> ranges;
  DenseMap<Operation *, bool> pureFunctions;
  DenseSet<Operation *> checking;
  bool resetWakeup = false;
  bool pureCall(func::CallOp call) {
    auto f = getOperation().lookupSymbol<func::FuncOp>(call.getCallee());
    if (!f)
      return false;
    auto found = pureFunctions.find(f);
    if (found != pureFunctions.end())
      return found->second;
    if (!checking.insert(f).second)
      return false;
    bool valid = true;
    for (Type ty : f.getArgumentTypes())
      valid &= bool(arc::computeLLVMBitWidth(ty));
    for (Type ty : f.getResultTypes())
      valid &= bool(arc::computeLLVMBitWidth(ty));
    if (f.isExternal())
      valid &= f->hasAttr("chipyard.native_pure_wide_shift");
    else if (!llvm::hasSingleElement(f.getBody()))
      valid = false;
    else
      for (auto &op : f.getBody().front())
        if (!isa<func::ReturnOp>(op) && !pure(&op))
          valid = false;
    checking.erase(f);
    return pureFunctions[f] = valid;
  }
  bool pure(Operation *op) {
    if (auto call = dyn_cast<func::CallOp>(op))
      return pureCall(call);
    if (op->getNumRegions() || !isMemoryEffectFree(op) || !isSpeculatable(op))
      return false;
    auto dialect = op->getName().getDialectNamespace();
    return dialect == "comb" || dialect == "hw" || dialect == "arith" ||
           op->getName().getStringRef().starts_with("sim.fmt.") ||
           op->getName().getStringRef() == "sim.stderr_stream";
  }
  bool collect(arc::ModelOp model) {
    auto storageFail = [](unsigned) { return false; };
    ranges.clear();
    resetWakeup = false;
    auto &body = model.getBodyBlock();
    if (body.getNumArguments() != 1 || !model.getStorageBytes())
      return storageFail(__LINE__);
    Value storage = body.getArgument(0);
    SmallVector<Range> all;
    for (auto *user : storage.getUsers()) {
      if (auto context = dyn_cast<arc::AsContextOp>(user)) {
        if (context->getBlock() != &body)
          return storageFail(__LINE__);
        for (auto *use : context.getResult().getUsers()) {
          auto wake = dyn_cast<arc::SetNextWakeupOp>(use);
          APInt value;
          if (!wake || wake->getBlock() != &body ||
              !matchPattern(wake.getTime(), m_ConstantInt(&value)) ||
              !value.isAllOnes())
            return storageFail(__LINE__);
          resetWakeup = true;
        }
        continue;
      }
      if (user->getNumResults() != 1 ||
          (user->getBlock() != &body && !isa<arc::StorageGetOp>(user)))
        return storageFail(__LINE__);
      Type ty = user->getResult(0).getType();
      uint64_t bytes;
      if (auto state = dyn_cast<arc::StateType>(ty)) {
        auto bits = arc::computeLLVMBitWidth(state.getType());
        if (!bits)
          return storageFail(__LINE__);
        bytes = *bits / 8 + (*bits % 8 != 0);
        if (!isa<arc::StorageGetOp, arc::AllocStateOp, arc::RootInputOp,
                 arc::RootOutputOp>(user))
          return storageFail(__LINE__);
      } else if (auto memory = dyn_cast<arc::MemoryType>(ty)) {
        if (!isa<arc::StorageGetOp, arc::AllocMemoryOp>(user))
          return storageFail(__LINE__);
        auto stride = user->getAttrOfType<IntegerAttr>("stride");
        uint64_t step =
            stride ? stride.getValue().getZExtValue() : memory.getStride();
        if (memory.getNumWords() && step > UINT64_MAX / memory.getNumWords())
          return storageFail(__LINE__);
        bytes = step * memory.getNumWords();
      } else
        return storageFail(__LINE__);
      auto offset = user->getAttrOfType<IntegerAttr>("offset");
      if (!offset || offset.getValue().isNegative())
        return storageFail(__LINE__);
      uint64_t start = offset.getValue().getZExtValue();
      if (start < arc::kStateOffset || start > *model.getStorageBytes() ||
          bytes > *model.getStorageBytes() - start)
        return storageFail(__LINE__);
      Range range{start, bytes, ty, user};
      ranges[user->getResult(0)] = range;
      all.push_back(range);
    }
    llvm::sort(all, [](auto &a, auto &b) { return a.start < b.start; });
    // Every alias must denote exactly the same object. Partial overlaps, two
    // descriptors for distinct objects, and escaped storage are unsupported.
    for (size_t i = 0; i < all.size(); ++i)
      for (size_t j = i + 1;
           j < all.size() && all[j].start < all[i].start + all[i].bytes; ++j)
        if (all[i].start != all[j].start || all[i].bytes != all[j].bytes ||
            all[i].type != all[j].type ||
            (!isa<arc::StorageGetOp>(all[i].op) &&
             !isa<arc::StorageGetOp>(all[j].op))) {
          llvm::errs() << "Alias rejection " << all[i].start << "/"
                       << all[i].bytes << " " << all[i].type << " and "
                       << all[j].start << "/" << all[j].bytes << " "
                       << all[j].type << "\n";
          return storageFail(__LINE__);
        }
    // References may only be read/written by native state/memory operations.
    for (auto &[value, range] : ranges)
      for (auto *user : value.getUsers())
        if (!isa<arc::StateReadOp, arc::StateWriteOp, arc::MemoryReadOp,
                 arc::MemoryWriteOp>(user)) {
          llvm::errs() << "Reference user rejection " << user->getName()
                       << "\n";
          return storageFail(__LINE__);
        }
    return true;
  }
  void certify(arc::ModelOp model) {
    auto fail = [&](StringRef why) {
      llvm::errs() << "Idle clock fallback: " << why << "\n";
    };
    auto effect = getOperation()->getAttrOfType<BoolAttr>(
        "chipyard.output_readback_safe");
    auto time = getOperation()->getAttrOfType<BoolAttr>(
        "chipyard.source_time_dependence");
    auto outputs =
        model->getAttrOfType<ArrayAttr>("chipyard.write_only_outputs");
    if (!effect || !effect.getValue() || !time || time.getValue() || !outputs ||
        (model.getTraceTapsAttr() && !model.getTraceTapsAttr().empty()) ||
        getOperation()->getAttr("chipyard.sampling_inputs")) {
      fail("source/observation contract");
      return;
    }
    if (!collect(model)) {
      fail("storage aliases");
      return;
    }
    SmallVector<StringRef> names;
    StringRef(heldInputs).split(names, ':', -1, false);
    std::map<uint64_t, std::string> held;
    DenseSet<StringAttr> requested;
    for (auto name : names)
      requested.insert(StringAttr::get(&getContext(), name));
    DenseSet<uint64_t> inputs, allOutputs, safeOutputs;
    for (auto &[value, range] : ranges) {
      if (auto input = dyn_cast<arc::RootInputOp>(range.op)) {
        inputs.insert(range.start);
        if (requested.contains(input.getNameAttr())) {
          if (!cast<arc::StateType>(range.type).getType().isInteger(1)) {
            fail("clock width");
            return;
          }
          held[range.start] = input.getName().str();
        }
      }
      if (auto output = dyn_cast<arc::RootOutputOp>(range.op)) {
        allOutputs.insert(range.start);
        if (llvm::is_contained(outputs, output.getNameAttr()))
          safeOutputs.insert(range.start);
      }
    }
    if (held.empty() || held.size() != requested.size()) {
      fail("held input contract");
      return;
    }
    std::map<uint64_t, uint64_t> histories;
    auto &body = model.getBodyBlock();
    for (auto write : body.getOps<arc::StateWriteOp>()) {
      auto read = write.getValue().getDefiningOp<arc::StateReadOp>();
      auto destination = ranges.find(write.getState());
      if (!read || destination == ranges.end())
        continue;
      auto source = ranges.find(read.getState());
      if (source == ranges.end() || !held.count(source->second.start))
        continue;
      auto &d = destination->second;
      if (d.type != source->second.type || inputs.contains(d.start) ||
          allOutputs.contains(d.start) ||
          (histories.count(d.start) &&
           histories[d.start] != source->second.start)) {
        fail("history write");
        return;
      }
      histories[d.start] = source->second.start;
    }
    if (histories.empty()) {
      fail("no clock history");
      return;
    }
    DenseMap<Value, bool> tainted;
    // i1 values known when none of the certified histories has a rising edge.
    DenseMap<Value, bool> known;
    auto isInactiveEdge = [&](Operation *operation) {
      auto conjunction = dyn_cast<comb::AndOp>(operation);
      if (!conjunction || conjunction.getNumOperands() != 2)
        return false;
      for (unsigned i = 0; i < 2; ++i) {
        auto fresh =
            conjunction.getOperand(i).getDefiningOp<arc::StateReadOp>();
        auto difference =
            conjunction.getOperand(1 - i).getDefiningOp<comb::XorOp>();
        if (!fresh || !difference || difference.getNumOperands() != 2)
          continue;
        auto input = ranges.find(fresh.getState());
        if (input == ranges.end() || !held.count(input->second.start))
          continue;
        for (unsigned j = 0; j < 2; ++j) {
          auto old = difference.getOperand(j).getDefiningOp<arc::StateReadOp>();
          auto other =
              difference.getOperand(1 - j).getDefiningOp<arc::StateReadOp>();
          if (!old || !other)
            continue;
          auto history = ranges.find(old.getState());
          auto repeated = ranges.find(other.getState());
          if (history != ranges.end() && repeated != ranges.end() &&
              histories.count(history->second.start) &&
              histories[history->second.start] == input->second.start &&
              repeated->second.start == input->second.start)
            return true;
        }
      }
      return false;
    };
    unsigned guarded = 0, outputWrites = 0;
    for (auto &op : body) {
      if (auto read = dyn_cast<arc::StateReadOp>(op)) {
        auto it = ranges.find(read.getState());
        if (it == ranges.end()) {
          fail("unknown state read");
          return;
        }
        tainted[read.getResult()] =
            held.count(it->second.start) || histories.count(it->second.start);
        continue;
      }
      if (auto condition = dyn_cast<scf::IfOp>(op)) {
        auto it = known.find(condition.getCondition());
        if (condition.getNumResults() || condition.elseBlock() ||
            it == known.end() || it->second) {
          llvm::errs() << "Unproven condition " << condition.getCondition()
                       << "\n";
          fail("possibly active region");
          return;
        }
        ++guarded;
        continue;
      }
      if (auto write = dyn_cast<arc::StateWriteOp>(op)) {
        auto it = ranges.find(write.getState());
        if (it == ranges.end()) {
          fail("unknown write");
          return;
        }
        auto h = histories.find(it->second.start);
        if (h != histories.end()) {
          auto read = write.getValue().getDefiningOp<arc::StateReadOp>();
          if (!read || ranges.find(read.getState()) == ranges.end() ||
              ranges[read.getState()].start != h->second) {
            fail("non-history update");
            return;
          }
        } else if (!safeOutputs.contains(it->second.start) ||
                   tainted[write.getValue()]) {
          fail("observable unguarded write");
          return;
        } else
          ++outputWrites;
        continue;
      }
      if (isa<arc::RootInputOp, arc::RootOutputOp, arc::AllocStateOp,
              arc::AllocMemoryOp, arc::StorageGetOp, arc::AsContextOp,
              arc::SetNextWakeupOp>(op))
        continue;
      bool dirty = llvm::any_of(op.getOperands(),
                                [&](Value v) { return tainted.lookup(v); });
      if (auto read = dyn_cast<arc::MemoryReadOp>(op)) {
        if (!ranges.count(read.getMemory()) || dirty) {
          fail("unknown/clock-dependent memory read");
          return;
        }
      } else if (!pure(&op)) {
        fail(op.getName().getStringRef());
        return;
      }
      for (Value v : op.getResults())
        tainted[v] = dirty;
      if (op.getNumResults() != 1 || !op.getResult(0).getType().isInteger(1))
        continue;
      if (isInactiveEdge(&op)) {
        known[op.getResult(0)] = false;
        continue;
      }
      APInt constant;
      if (matchPattern(op.getResult(0), m_ConstantInt(&constant))) {
        known[op.getResult(0)] = !constant.isZero();
        continue;
      }
      bool isAnd = isa<comb::AndOp, arith::AndIOp>(op);
      bool isOr = isa<comb::OrOp, arith::OrIOp>(op);
      bool isXor = isa<comb::XorOp, arith::XOrIOp>(op);
      if (!isAnd && !isOr && !isXor)
        continue;
      bool all = true, folded = isAnd;
      for (Value v : op.getOperands()) {
        auto it = known.find(v);
        if (it == known.end()) {
          all = false;
          continue;
        }
        if ((isAnd && !it->second) || (isOr && it->second)) {
          known[op.getResult(0)] = it->second;
          all = false;
          break;
        }
        if (isAnd)
          folded &= it->second;
        if (isOr)
          folded |= it->second;
        if (isXor)
          folded ^= it->second;
      }
      if (all)
        known[op.getResult(0)] = folded;
    }
    std::string name = (model.getName() + "_idle_clock").str();
    std::string stableName = (model.getName() + "_stable_combinational").str();
    std::string stableEvalName = (model.getName() + "_stable_eval").str();
    if (SymbolTable::lookupSymbolIn(getOperation(), name) ||
        SymbolTable::lookupSymbolIn(getOperation(), stableName) ||
        (stableEvaluator &&
         SymbolTable::lookupSymbolIn(getOperation(), stableEvalName))) {
      fail("symbol collision");
      return;
    }
    OpBuilder b(model);
    auto loc = model.getLoc();
    auto fn = func::FuncOp::create(
        b, loc, name,
        b.getFunctionType({body.getArgument(0).getType()}, {b.getI32Type()}));
    auto *entry = fn.addEntryBlock();
    b.setInsertionPointToStart(entry);
    Value zero = hw::ConstantOp::create(b, loc, APInt(1, 0));
    Value idle = hw::ConstantOp::create(b, loc, APInt(1, 1));
    std::map<uint64_t, Value> freshClocks;
    for (auto &[offset, ignored] : held) {
      auto state = arc::StorageGetOp::create(
          b, loc, arc::StateType::get(b.getI1Type()), entry->getArgument(0),
          b.getI32IntegerAttr(offset));
      freshClocks[offset] = arc::StateReadOp::create(b, loc, state);
    }
    for (auto &[offset, input] : histories) {
      auto state = arc::StorageGetOp::create(
          b, loc, arc::StateType::get(b.getI1Type()), entry->getArgument(0),
          b.getI32IntegerAttr(offset));
      Value old = arc::StateReadOp::create(b, loc, state);
      Value changed = comb::XorOp::create(b, loc, old, freshClocks[input]);
      Value active = comb::AndOp::create(b, loc, changed, freshClocks[input]);
      Value inactive =
          comb::ICmpOp::create(b, loc, comb::ICmpPredicate::eq, active, zero);
      idle = comb::AndOp::create(b, loc, idle, inactive);
    }
    // LLVM i1 loads ignore byte padding, while native convergence compares
    // physical bytes. Refuse dirty scalar-clock bytes: a full evaluation can
    // otherwise normalize or propagate padding differently at O0 and O3.
    // These i8 views cover exactly the allocated one-byte clock/history slots.
    auto canonicalClocks = [&](Value storage, Value allowed) {
      auto oneByte = hw::ConstantOp::create(b, loc, APInt(8, 1));
      auto check = [&](uint64_t offset) {
        auto slot = arc::StorageGetOp::create(
            b, loc, arc::StateType::get(b.getI8Type()), storage,
            b.getI32IntegerAttr(offset));
        auto physical = arc::StateReadOp::create(b, loc, slot);
        Value canonical = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ule,
                                               physical, oneByte);
        allowed = comb::AndOp::create(b, loc, allowed, canonical);
      };
      for (auto &[offset, ignored] : held)
        check(offset);
      for (auto &[offset, ignored] : histories)
        check(offset);
      return allowed;
    };
    idle = canonicalClocks(entry->getArgument(0), idle);
    auto condition = scf::IfOp::create(b, loc, idle, false);
    b.setInsertionPoint(condition.thenYield());
    for (auto &[offset, input] : histories) {
      auto state = arc::StorageGetOp::create(
          b, loc, arc::StateType::get(b.getI1Type()), entry->getArgument(0),
          b.getI32IntegerAttr(offset));
      arc::StateWriteOp::create(b, loc, state, freshClocks[input]);
    }
    if (resetWakeup) {
      auto context = arc::AsContextOp::create(b, loc, entry->getArgument(0));
      auto never = hw::ConstantOp::create(b, loc, APInt::getAllOnes(64));
      arc::SetNextWakeupOp::create(b, loc, context, never);
    }
    b.setInsertionPointAfter(condition);
    Value result = arith::ExtUIOp::create(b, loc, b.getI32Type(), idle);
    func::ReturnOp::create(b, loc, result);
    model->setAttr("chipyard.idle_clock_fn", FlatSymbolRefAttr::get(fn));
    // With every history equal to its held clock, all certified regions are
    // inactive and all unconditional history stores are identities. Ordinary
    // inputs may change: only write-only outputs can change as a result. The
    // runtime must also enable its certified-output comparison before using
    // this stronger, read-only query to omit a convergence snapshot.
    b.setInsertionPoint(model);
    auto stableFn = func::FuncOp::create(
        b, loc, stableName,
        b.getFunctionType({body.getArgument(0).getType()}, {b.getI32Type()}));
    auto *stableEntry = stableFn.addEntryBlock();
    b.setInsertionPointToStart(stableEntry);
    Value stable = hw::ConstantOp::create(b, loc, APInt(1, 1));
    std::map<uint64_t, Value> stableClocks;
    for (auto &[offset, ignored] : held) {
      auto state = arc::StorageGetOp::create(
          b, loc, arc::StateType::get(b.getI1Type()),
          stableEntry->getArgument(0), b.getI32IntegerAttr(offset));
      stableClocks[offset] = arc::StateReadOp::create(b, loc, state);
    }
    for (auto &[offset, input] : histories) {
      auto state = arc::StorageGetOp::create(
          b, loc, arc::StateType::get(b.getI1Type()),
          stableEntry->getArgument(0), b.getI32IntegerAttr(offset));
      Value previous = arc::StateReadOp::create(b, loc, state);
      Value same = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::eq,
                                        previous, stableClocks[input]);
      stable = comb::AndOp::create(b, loc, stable, same);
    }
    if (resetWakeup) {
      auto context =
          arc::AsContextOp::create(b, loc, stableEntry->getArgument(0));
      auto wakeup = arc::GetNextWakeupOp::create(b, loc, context);
      auto never = hw::ConstantOp::create(b, loc, APInt::getAllOnes(64));
      Value same =
          comb::ICmpOp::create(b, loc, comb::ICmpPredicate::eq, wakeup, never);
      stable = comb::AndOp::create(b, loc, stable, same);
    }
    stable = canonicalClocks(stableEntry->getArgument(0), stable);
    Value stableResult = arith::ExtUIOp::create(b, loc, b.getI32Type(), stable);
    func::ReturnOp::create(b, loc, stableResult);
    model->setAttr("chipyard.stable_combinational_fn",
                   FlatSymbolRefAttr::get(stableFn));

    if (stableEvaluator) {
      // This second evaluator is valid only after the read-only stable query
      // succeeds. The proof above makes every top-level region inactive. All
      // unconditional stores remain, including physical history and wakeup
      // writes; the ordinary lowering pipeline can remove unused calculations.
      b.setInsertionPoint(model);
      auto stableEval = func::FuncOp::create(
          b, loc, stableEvalName,
          b.getFunctionType({body.getArgument(0).getType()}, {}));
      // Keep the physical-byte fallback independent of the query's i8 views.
      // Cross-function inlining may otherwise normalize padding in i1 stores.
      stableEval->setAttr("llvm.no_inline", b.getUnitAttr());
      stableEval->setAttr("llvm.passthrough", b.getStrArrayAttr({"noinline"}));
      auto *stableBody = stableEval.addEntryBlock();
      b.setInsertionPointToStart(stableBody);
      IRMapping mapping;
      mapping.map(body.getArgument(0), stableBody->getArgument(0));
      unsigned omitted = 0, copied = 0;
      for (auto &op : body) {
        if (isa<scf::IfOp>(op)) {
          ++omitted;
          continue;
        }
        if (isa<arc::RootInputOp, arc::RootOutputOp, arc::AllocStateOp,
                arc::AllocMemoryOp>(op)) {
          auto slot = arc::StorageGetOp::create(
              b, op.getLoc(), op.getResult(0).getType(),
              mapping.lookup(body.getArgument(0)),
              op.getAttrOfType<IntegerAttr>("offset"));
          mapping.map(op.getResult(0), slot.getResult());
        } else
          b.clone(op, mapping);
        ++copied;
      }
      func::ReturnOp::create(b, loc);
      model->setAttr("chipyard.stable_eval_fn",
                     FlatSymbolRefAttr::get(stableEval));
      llvm::errs() << "Stable evaluator: " << omitted
                   << " proven-inactive regions omitted, " << copied
                   << " original top-level operations copied\n";
    }

    SmallVector<Attribute> inputNames;
    for (auto &[offset, name] : held)
      inputNames.push_back(b.getStringAttr(name));
    model->setAttr("chipyard.idle_clock_inputs", b.getArrayAttr(inputNames));
    llvm::errs() << "Certified idle clock: " << held.size() << " inputs, "
                 << histories.size() << " histories, " << guarded
                 << " inactive regions, " << outputWrites
                 << " invariant output writes\n";
  }
  void runOnOperation() override {
    for (auto model : getOperation().getOps<arc::ModelOp>()) {
      model->removeAttr("chipyard.stable_eval_fn");
      model->removeAttr("chipyard.idle_clock_fn");
      model->removeAttr("chipyard.idle_clock_inputs");
      model->removeAttr("chipyard.stable_combinational_fn");
    }
    if (!enabled || heldInputs.empty())
      return;
    // An independently accessible storage alias invalidates the proof.
    bool escaped = false;
    getOperation().walk([&](arc::StorageGetOp get) {
      if (!get->getParentOfType<arc::ModelOp>())
        escaped = true;
    });
    if (escaped) {
      llvm::errs() << "Idle clock fallback: external storage alias\n";
      return;
    }
    SmallVector<arc::ModelOp> models(getOperation().getOps<arc::ModelOp>());
    for (auto model : models)
      certify(model);
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardIdleClock", "1",
          [] { PassRegistration<IdleClock>(); }};
}
