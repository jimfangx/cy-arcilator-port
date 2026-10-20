// Preserve a coroutine's conditional signal writes through native state. The
// host publishes writes after all sequential models have sampled pre-edge IO.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/LLHD/LLHDPasses.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqPasses.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Transforms/Passes.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/Passes.h"

using namespace mlir;
using namespace circt;

namespace {
// A struct update whose only input is a merge argument can execute on each
// incoming edge when its new field value already dominates those edges. This
// exposes complete overwrite chains to the ordinary HW canonicalizer, removing
// false dependencies on a local aggregate's old contents without inventing an
// initial value or changing a partially assigned field.
struct PushStructUpdates
    : PassWrapper<PushStructUpdates, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PushStructUpdates)
  StringRef getArgument() const override {
    return "chipyard-push-struct-updates";
  }
  void runOnOperation() override {
    SmallVector<hw::StructInjectOp> updates;
    getOperation().walk([&](hw::StructInjectOp update) {
      if (isa<llhd::CombinationalOp>(update->getParentOp()))
        updates.push_back(update);
    });
    DominanceInfo dominance;
    unsigned moved = 0;
    for (auto update : updates) {
      auto argument = dyn_cast<BlockArgument>(update.getInput());
      if (!argument || !argument.hasOneUse())
        continue;
      auto *block = argument.getOwner();
      if (block == &block->getParent()->front())
        continue;
      SmallVector<cf::BranchOp> incoming;
      bool supported = true;
      for (auto *pred : block->getPredecessors()) {
        auto branch = dyn_cast<cf::BranchOp>(pred->getTerminator());
        if (!branch || incoming.size() >= 4096) {
          supported = false;
          break;
        }
        incoming.push_back(branch);
      }
      if (!supported || incoming.empty())
        continue;
      SmallVector<Operation *> slice;
      DenseSet<Operation *> seen;
      std::function<bool(Value)> available = [&](Value value) {
        if (auto arg = dyn_cast<BlockArgument>(value))
          if (arg.getOwner() == block)
            return true;
        if (llvm::all_of(incoming, [&](auto branch) {
              return dominance.dominates(value, branch);
            }))
          return true;
        auto *op = value.getDefiningOp();
        if (!op || op->getBlock() != update->getBlock() ||
            op->getNumRegions() || !isPure(op) || slice.size() >= 1024)
          return false;
        if (seen.contains(op))
          return true;
        for (auto operand : op->getOperands())
          if (!available(operand))
            return false;
        seen.insert(op);
        slice.push_back(op);
        return true;
      };
      if (!available(update.getNewValue()) ||
          incoming.size() * (slice.size() + 1) > 4096)
        continue;
      for (auto branch : incoming) {
        OpBuilder builder(branch);
        IRMapping mapping;
        for (auto [arg, input] :
             llvm::zip(block->getArguments(), branch.getDestOperands()))
          mapping.map(arg, input);
        for (auto *op : slice)
          builder.clone(*op, mapping);
        auto clone = builder.clone(*update, mapping);
        branch->setOperand(argument.getArgNumber(), clone->getResult(0));
      }
      update.replaceAllUsesWith(argument);
      update.erase();
      ++moved;
    }
    if (moved)
      llvm::errs() << "Moved " << moved
                   << " struct updates across CFG merges\n";
  }
};

// Compilation-unit/package statics can initialize from other global statics.
// Evaluate only pure constant startup expressions; preserve all effects by
// diagnosing them instead of discarding an initializer's unused operations.
struct SnapshotGlobalSignalInitializers
    : PassWrapper<SnapshotGlobalSignalInitializers, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SnapshotGlobalSignalInitializers)
  StringRef getArgument() const override {
    return "chipyard-snapshot-global-signal-initializers";
  }
  void runOnOperation() override {
    DenseMap<Operation *, Attribute> values;
    DenseSet<Operation *> active;
    std::function<Attribute(llhd::GlobalSignalOp)> evaluate;
    evaluate = [&](llhd::GlobalSignalOp global) -> Attribute {
      if (auto found = values.find(global); found != values.end())
        return found->second;
      if (!active.insert(global).second)
        return {};
      auto finish = [&](Attribute attr) {
        active.erase(global);
        if (attr)
          values[global] = attr;
        return attr;
      };
      auto type = global.getType();
      if (global.getInitRegion().empty()) {
        auto width = hw::getBitWidth(type);
        if (width <= 0 || width > IntegerType::kMaxWidth)
          return finish({});
        // This is the same native two-state default as SignalMemory.
        APInt zero(width, 0);
        if (isa<IntegerType>(type))
          return finish(IntegerAttr::get(type, zero));
        ArrayAttr aggregate;
        if (succeeded(hw::apIntToAggregateAttr(type, zero, aggregate)))
          return finish(aggregate);
        return finish({});
      }
      auto *source = global.getInitBlock();
      if (llvm::any_of(source->without_terminator(), [](Operation &op) {
            return !isa<llhd::ProbeOp>(&op) && !isPure(&op);
          }))
        return finish({});
      auto yield = dyn_cast<llhd::YieldOp>(source->getTerminator());
      if (!yield || yield->getNumOperands() != 1)
        return finish({});
      Region temporary;
      auto *block = new Block;
      temporary.push_back(block);
      OpBuilder builder(&getContext());
      builder.setInsertionPointToStart(block);
      DenseMap<Value, Value> copied;
      DenseSet<Value> visiting;
      auto constant = [&](Attribute attr, Type type, Location loc) -> Value {
        auto *op = hw::materializeConstant(builder, attr, type, loc);
        return op ? op->getResult(0) : Value{};
      };
      std::function<Value(Value)> copy = [&](Value value) -> Value {
        if (auto found = copied.find(value); found != copied.end())
          return found->second;
        if (!visiting.insert(value).second)
          return {};
        auto done = [&](Value result) {
          visiting.erase(value);
          if (result)
            copied[value] = result;
          return result;
        };
        Attribute attr;
        if (matchPattern(value, m_Constant(&attr)))
          return done(constant(attr, value.getType(), value.getLoc()));
        auto *op = value.getDefiningOp();
        if (!op || op->getBlock() != source)
          return done({});
        if (auto probe = dyn_cast<llhd::ProbeOp>(op)) {
          auto get = probe.getSignal().getDefiningOp<llhd::GetGlobalSignalOp>();
          auto target =
              get ? SymbolTable::lookupNearestSymbolFrom<llhd::GlobalSignalOp>(
                        get, get.getGlobalNameAttr())
                  : llhd::GlobalSignalOp{};
          auto initial = target ? evaluate(target) : Attribute{};
          return done(initial
                          ? constant(initial, value.getType(), value.getLoc())
                          : Value{});
        }
        if (op->getNumRegions() || !isPure(op))
          return done({});
        IRMapping mapping;
        for (auto operand : op->getOperands()) {
          auto input = copy(operand);
          if (!input)
            return done({});
          mapping.map(operand, input);
        }
        auto *clone = builder.clone(*op, mapping);
        SmallVector<Value> folded;
        if (succeeded(builder.tryFold(clone, folded)) && !folded.empty()) {
          for (auto [original, replacement] :
               llvm::zip(op->getResults(), folded))
            copied[original] = replacement;
          clone->erase();
        } else {
          for (auto [original, replacement] :
               llvm::zip(op->getResults(), clone->getResults()))
            copied[original] = replacement;
        }
        return done(copied.lookup(value));
      };
      auto result = copy(yield->getOperand(0));
      Attribute attr;
      if (!result || !matchPattern(result, m_Constant(&attr)))
        return finish({});
      return finish(attr);
    };
    SmallVector<llhd::GlobalSignalOp> globals;
    getOperation().walk(
        [&](llhd::GlobalSignalOp global) { globals.push_back(global); });
    // Resolve every original dependency before replacing any initializer body.
    for (auto global : globals)
      if (!evaluate(global)) {
        global.emitError("native global signal initialization requires a pure "
                         "constant startup value; runtime, effectful or cyclic "
                         "dependencies are unsupported");
        signalPassFailure();
        return;
      }
    for (auto global : globals) {
      if (global.getInitRegion().empty())
        continue;
      Region region;
      auto *block = new Block;
      region.push_back(block);
      OpBuilder builder(&getContext());
      builder.setInsertionPointToStart(block);
      auto *constant = hw::materializeConstant(
          builder, values.lookup(global), global.getType(), global.getLoc());
      llhd::YieldOp::create(builder, global.getLoc(), constant->getResult(0));
      global.getInitRegion().takeBody(region);
    }
  }
};

// Signal initializers run before time-zero processes and continuous drives.
// Cloning their pure dependency graph replaces reads of another signal with
// that signal's initial value. Leaving a probe in this graph would turn an
// undriven static variable into a live combinational alias during sig2reg.
struct SnapshotSignalInitializers
    : PassWrapper<SnapshotSignalInitializers, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SnapshotSignalInitializers)
  StringRef getArgument() const override {
    return "chipyard-snapshot-signal-initializers";
  }
  void runOnOperation() override {
    auto module = getOperation();
    auto *body = module.getBodyBlock();
    OpBuilder builder(body, body->begin());
    DenseMap<Value, Value> initial;
    DenseSet<Value> active;
    std::function<Value(Value)> snapshot = [&](Value value) -> Value {
      if (auto found = initial.find(value); found != initial.end())
        return found->second;
      if (!active.insert(value).second)
        return {};
      auto finish = [&](Value result) {
        active.erase(value);
        if (result)
          initial[value] = result;
        return result;
      };
      Attribute constant;
      if (matchPattern(value, m_Constant(&constant)))
        return finish(value);
      auto *op = value.getDefiningOp();
      if (!op || op->getBlock() != body)
        return finish({});
      if (auto probe = dyn_cast<llhd::ProbeOp>(op)) {
        auto signal = probe.getSignal().getDefiningOp<llhd::SignalOp>();
        if (signal && signal.getInit())
          return finish(snapshot(signal.getInit()));
        auto type = value.getType();
        Attribute attr;
        if (!signal) {
          auto get = probe.getSignal().getDefiningOp<llhd::GetGlobalSignalOp>();
          auto global =
              get ? SymbolTable::lookupNearestSymbolFrom<llhd::GlobalSignalOp>(
                        get, get.getGlobalNameAttr())
                  : llhd::GlobalSignalOp{};
          if (!global)
            return finish({});
          if (!global.getInitRegion().empty()) {
            auto yield =
                cast<llhd::YieldOp>(global.getInitBlock()->getTerminator());
            if (!matchPattern(yield->getOperand(0), m_Constant(&attr)))
              return finish({});
          }
        }
        if (attr) {
          auto *constant =
              hw::materializeConstant(builder, attr, type, value.getLoc());
          return finish(constant ? constant->getResult(0) : Value{});
        }
        // Uninitialized native two-state storage uses SignalMemory's zero.
        auto width = hw::getBitWidth(type);
        if (width <= 0 || width > IntegerType::kMaxWidth)
          return finish({});
        Value zero =
            hw::ConstantOp::create(builder, value.getLoc(), APInt(width, 0));
        if (zero.getType() != type)
          zero =
              builder.createOrFold<hw::BitcastOp>(value.getLoc(), type, zero);
        return finish(zero);
      }
      if (op->getNumRegions() || !isPure(op))
        return finish({});
      IRMapping mapping;
      for (auto operand : op->getOperands()) {
        auto input = snapshot(operand);
        if (!input)
          return finish({});
        mapping.map(operand, input);
      }
      auto *clone = builder.clone(*op, mapping);
      SmallVector<Value> folded;
      if (succeeded(builder.tryFold(clone, folded)) && !folded.empty()) {
        for (auto [original, replacement] : llvm::zip(op->getResults(), folded))
          initial[original] = replacement;
        clone->erase();
      } else {
        for (auto [original, replacement] :
             llvm::zip(op->getResults(), clone->getResults()))
          initial[original] = replacement;
      }
      return finish(initial.lookup(value));
    };
    SmallVector<llhd::SignalOp> signals;
    for (auto signal : body->getOps<llhd::SignalOp>())
      signals.push_back(signal);
    for (auto signal : signals) {
      if (!signal.getInit())
        continue;
      auto value = snapshot(signal.getInit());
      if (!value) {
        signal.emitError(
            "native signal initialization requires a pure startup "
            "value; runtime or cyclic dependencies are unsupported");
        signalPassFailure();
        return;
      }
      signal.getInitMutable().assign(value);
    }
  }
};

// This pass is native-only. Registers without a preset start at zero in Arc's
// two-state allocation, so identical transfer functions and controls stay equal
// from initialization onward. Do not infer equivalence from names or data
// alone.
static bool equivalentValue(Value first, Value next, Block *block) {
  if (first == next)
    return true;
  auto a = first.getDefiningOp<seq::FirRegOp>();
  auto b = next.getDefiningOp<seq::FirRegOp>();
  return a && b && a->getBlock() == block && b->getBlock() == block &&
         !a->getAttr("inner_sym") && !b->getAttr("inner_sym") &&
         OperationEquivalence::isEquivalentTo(
             a, b, OperationEquivalence::exactValueMatch, nullptr,
             OperationEquivalence::IgnoreLocations);
}

static bool immediate(llhd::DriveOp drive) {
  llhd::TimeAttr time;
  return !drive.getEnable() &&
         matchPattern(drive.getTime(), m_Constant(&time)) && !time.getTime() &&
         time.getDelta() == 0 && time.getEpsilon() == 1;
}

static bool initialBits(llhd::SignalOp signal, APInt &initial) {
  auto init = signal.getInit();
  if (!init)
    return true;
  Attribute attr;
  if (!matchPattern(init, m_Constant(&attr)))
    return false;
  if (auto integer = dyn_cast<IntegerAttr>(attr)) {
    initial = integer.getValue();
    return true;
  }
  auto array = dyn_cast<ArrayAttr>(attr);
  return array &&
         succeeded(hw::aggregateAttrToAPInt(init.getType(), array, initial));
}

// A module-level enabled drive with pure data/control dependencies is a
// transparent latch. Coroutine-produced values retain their event-controlled
// write semantics and must continue through the process-state path below.
static bool combinationalSource(Value value, Block *block,
                                DenseSet<Value> &visited) {
  if (!visited.insert(value).second)
    return true;
  if (auto arg = dyn_cast<BlockArgument>(value))
    return arg.getOwner() == block;
  auto *op = value.getDefiningOp();
  if (!op || op->getBlock() != block)
    return false;
  if (isa<seq::FirRegOp, seq::CompRegOp, hw::InstanceOp, llhd::ProbeOp>(op))
    return true;
  return !op->getNumRegions() && isPure(op) &&
         llvm::all_of(op->getOperands(), [&](Value input) {
           return combinationalSource(input, block, visited);
         });
}

// Follow only declarative combinational drivers. An unconditional self-drive
// can retain a value just like an enabled latch; promoting it directly to SSA
// discards its signal initializer. Preserve that boundary before sig2reg.
static bool feedbackSource(Value value, llhd::SignalOp target,
                           DenseSet<Value> &visited) {
  if (!visited.insert(value).second || visited.size() > 100000)
    return false;
  auto *op = value.getDefiningOp();
  if (!op || op->getBlock() != target->getBlock())
    return false;
  if (auto probe = dyn_cast<llhd::ProbeOp>(op)) {
    if (probe.getSignal() == target.getResult())
      return true;
    auto signal = probe.getSignal().getDefiningOp<llhd::SignalOp>();
    if (!signal)
      return false;
    llhd::DriveOp drive;
    for (auto *user : signal->getUsers()) {
      if (isa<llhd::ProbeOp>(user))
        continue;
      auto next = dyn_cast<llhd::DriveOp>(user);
      if (!next || drive || next->getBlock() != target->getBlock() ||
          !immediate(next))
        return false;
      drive = next;
    }
    return drive && feedbackSource(drive.getValue(), target, visited);
  }
  if (isa<seq::FirRegOp, seq::CompRegOp, hw::InstanceOp>(op) ||
      op->getNumRegions() || !isPure(op))
    return false;
  return llvm::any_of(op->getOperands(), [&](Value input) {
    return feedbackSource(input, target, visited);
  });
}

struct SignalState : PassWrapper<SignalState, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SignalState)
  SignalState() = default;
  SignalState(const SignalState &other) : PassWrapper(other) {}
  Option<bool> feedbackOnly{*this, "feedback-only", llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-native-signal-state";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect>();
  }
  void runOnOperation() override {
    auto top = getOperation();
    // LLHD delay explicitly transports input value transitions. SV continuous
    // assignments carry their separate inertial cell from the frontend.
    SmallVector<llhd::DelayOp> delays;
    top.walk([&](llhd::DelayOp op) { delays.push_back(op); });
    unsigned delayed = 0;
    for (auto op : delays) {
      auto timing = op.getDelay();
      if (!timing.getTime() || timing.getDelta() || timing.getEpsilon() ||
          !isa<hw::HWModuleOp>(op->getParentOp()))
        continue;
      auto width = hw::getBitWidth(op.getType());
      if (width <= 0)
        continue;
      OpBuilder b(op);
      auto integer = b.getIntegerType(width);
      hw::HWModuleExternOp declaration;
      {
        OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToStart(top.getBody());
        std::string name = "__ArcTransportDelay_i" + std::to_string(width);
        while (top.lookupSymbol(name)) name += "_";
        SmallVector<hw::PortInfo> ports{
            {{b.getStringAttr("value"), integer, hw::ModulePort::Input}, 0},
            {{b.getStringAttr("q"), integer, hw::ModulePort::Output}, 0}};
        declaration = hw::HWModuleExternOp::create(
            b, op.getLoc(), b.getStringAttr(name), ports, "ArcDelay",
            b.getArrayAttr({hw::ParamDeclAttr::get("INIT", integer),
                            hw::ParamDeclAttr::get("DELAY_FS", b.getI64Type()),
                            hw::ParamDeclAttr::get("INERTIAL", b.getI1Type())}));
        declaration.setPrivate();
      }
      Value input = op.getInput();
      if (input.getType() != integer)
        input = hw::BitcastOp::create(b, op.getLoc(), integer, input);
      auto instance = hw::InstanceOp::create(
          b, op.getLoc(), declaration, "transport_delay_" + std::to_string(delayed++),
          SmallVector<Value>{input}, b.getArrayAttr({
              hw::ParamDeclAttr::get("INIT", b.getIntegerAttr(integer, APInt(width, 0))),
              hw::ParamDeclAttr::get("DELAY_FS", b.getI64IntegerAttr(timing.getTime())),
              hw::ParamDeclAttr::get("INERTIAL", b.getBoolAttr(false))}));
      Value result = instance.getResult(0);
      if (result.getType() != op.getType())
        result = hw::BitcastOp::create(b, op.getLoc(), op.getType(), result);
      op.replaceAllUsesWith(result);
      op.erase();
    }
    if (delayed)
      llvm::errs() << "Bound " << delayed << " native transport delays\n";
    SmallVector<sim::StringCmpOp> comparisons;
    top.walk([&](sim::StringCmpOp op) { comparisons.push_back(op); });
    for (auto op : comparisons) {
      StringAttr lhs, rhs;
      if (!matchPattern(op.getLhs(), m_Constant(&lhs)) ||
          !matchPattern(op.getRhs(), m_Constant(&rhs)))
        continue;
      // The SDK does not fold sim.string.cmp. Parameter comparisons are
      // compile-time byte comparisons; preserve dynamic string operations.
      int order = lhs.getValue().compare(rhs.getValue());
      bool value = false;
      switch (op.getPredicate()) {
      case sim::StringCmpPredicate::eq:
        value = order == 0;
        break;
      case sim::StringCmpPredicate::ne:
        value = order != 0;
        break;
      case sim::StringCmpPredicate::lt:
        value = order < 0;
        break;
      case sim::StringCmpPredicate::le:
        value = order <= 0;
        break;
      case sim::StringCmpPredicate::gt:
        value = order > 0;
        break;
      case sim::StringCmpPredicate::ge:
        value = order >= 0;
        break;
      }
      OpBuilder builder(op);
      auto constant =
          hw::ConstantOp::create(builder, op.getLoc(), APInt(1, value));
      op.replaceAllUsesWith(constant.getResult());
      op.erase();
    }
    DenseMap<std::pair<unsigned, unsigned>, hw::HWModuleExternOp> declarations;
    SmallVector<hw::HWModuleOp> modules(top.getOps<hw::HWModuleOp>());
    unsigned count = 0, latches = 0, undriven = 0, coalesced = 0;
    for (auto module : modules) {
      SmallVector<llhd::SignalOp> signals(module.getOps<llhd::SignalOp>());
      DenseSet<Operation *> feedback;
      if (feedbackOnly)
        for (auto signal : signals)
          for (auto *user : signal->getUsers())
            if (auto drive = dyn_cast<llhd::DriveOp>(user)) {
              DenseSet<Value> visited;
              if (drive->getBlock() == signal->getBlock() && immediate(drive) &&
                  feedbackSource(drive.getValue(), signal, visited))
                feedback.insert(signal);
            }
      for (auto signal : signals) {
        if (feedbackOnly && !feedback.contains(signal))
          continue;
        auto width = hw::getBitWidth(signal.getType().getNestedType());
        if (width <= 0)
          continue;
        SmallVector<llhd::ProbeOp> probes;
        SmallVector<llhd::DriveOp> drives;
        bool unsupported = false;
        for (auto *user : signal->getUsers()) {
          if (auto probe = dyn_cast<llhd::ProbeOp>(user))
            probes.push_back(probe);
          else if (auto writer = dyn_cast<llhd::DriveOp>(user)) {
            if (writer->getBlock() != signal->getBlock()) {
              unsupported = true;
              break;
            }
            drives.push_back(writer);
          } else {
            unsupported = true;
            break;
          }
        }
        if (unsupported)
          continue;
        if (drives.empty()) {
          // No aliases, escapes or writes exist. Do not use this rule to break
          // cycles or to replace state that has an unknown producer.
          APInt initial(width, 0);
          if (!initialBits(signal, initial))
            continue;
          OpBuilder builder(signal);
          Value value =
              hw::ConstantOp::create(builder, signal.getLoc(), initial);
          if (value.getType() != signal.getType().getNestedType())
            value =
                hw::BitcastOp::create(builder, signal.getLoc(),
                                      signal.getType().getNestedType(), value);
          for (auto probe : probes) {
            probe.replaceAllUsesWith(value);
            probe.erase();
          }
          signal.erase();
          ++undriven;
          continue;
        }
        auto drive = drives.front();
        if (drives.size() > 1) {
          bool equal = llvm::all_of(drives, [&](auto next) {
            return immediate(next) &&
                   next->getAttrDictionary() == drive->getAttrDictionary() &&
                   equivalentValue(drive.getValue(), next.getValue(),
                                   signal->getBlock());
          });
          if (equal) {
            for (auto extra : llvm::drop_begin(drives))
              extra.erase();
            ++coalesced;
          }
          // The ordinary signal promoter handles the remaining single drive.
          continue;
        }
        bool unconditionalFeedback = feedbackOnly && !drive.getEnable();
        if (!drive.getEnable() && !unconditionalFeedback)
          continue;
        auto process = drive.getValue().getDefiningOp<llhd::ProcessOp>();
        bool processState =
            process && drive.getEnable() &&
            drive.getEnable().getDefiningOp() == process.getOperation();
        if (!processState) {
          DenseSet<Value> visited;
          if (!combinationalSource(drive.getValue(), signal->getBlock(),
                                   visited) ||
              (drive.getEnable() &&
               !combinationalSource(drive.getEnable(), signal->getBlock(),
                                    visited)))
            continue;
        }
        llhd::TimeAttr time;
        if (!matchPattern(drive.getTime(), m_Constant(&time)) ||
            time.getTime() ||
            !((time.getDelta() == 0 && time.getEpsilon() == 1) ||
              (time.getDelta() == 1 && time.getEpsilon() == 0)))
          continue;
        // Blocking latch writes settle before sequential sampling. NBA writes
        // use staged state and publish after peers sample the old value.
        bool transparent = !processState && time.getEpsilon() == 1;
        // Uninitialized state follows the native model's two-state zero
        // initialization. Preserve an explicit initializer bit for bit.
        APInt initial(width, 0);
        if (!initialBits(signal, initial))
          continue;
        OpBuilder builder(signal);
        auto type = builder.getIntegerType(width);
        auto key = std::make_pair(unsigned(width), unsigned(transparent));
        auto declaration = declarations.lookup(key);
        if (!declaration) {
          OpBuilder outer(module);
          auto name =
              std::string(transparent ? "__ArcLatch_i" : "__ArcSignalState_i") +
              std::to_string(width);
          while (top.lookupSymbol(name))
            name += "_";
          SmallVector<hw::PortInfo> ports{
              {{outer.getStringAttr("value"), type, hw::ModulePort::Input}, 0},
              {{outer.getStringAttr("write"), outer.getI1Type(),
                hw::ModulePort::Input},
               1},
              {{outer.getStringAttr("q"), type, hw::ModulePort::Output}, 0}};
          declaration = hw::HWModuleExternOp::create(
              outer, signal.getLoc(), outer.getStringAttr(name), ports,
              transparent ? "ArcLatch" : "ArcSignalState",
              outer.getArrayAttr({hw::ParamDeclAttr::get("INIT", type)}));
          declaration.setPrivate();
          declarations[key] = declaration;
        }
        Value value = drive.getValue();
        Value enable = drive.getEnable();
        if (!enable)
          enable =
              hw::ConstantOp::create(builder, signal.getLoc(), APInt(1, 1));
        if (value.getType() != type)
          value = hw::BitcastOp::create(builder, signal.getLoc(), type, value);
        auto parameters = builder.getArrayAttr({hw::ParamDeclAttr::get(
            "INIT", builder.getIntegerAttr(type, initial))});
        auto instance = hw::InstanceOp::create(
            builder, signal.getLoc(), declaration,
            (transparent ? "latch_" : "signal_state_") +
                std::to_string(count++),
            SmallVector<Value>{value, enable}, parameters);
        for (auto probe : probes) {
          builder.setInsertionPoint(probe);
          Value result = instance.getResult(0);
          if (result.getType() != probe.getType())
            result = hw::BitcastOp::create(builder, probe.getLoc(),
                                           probe.getType(), result);
          probe.replaceAllUsesWith(result);
          probe.erase();
        }
        drive.erase();
        signal.erase();
        latches += !processState;
      }
    }
    if (count > latches)
      llvm::errs() << "Bound " << count - latches
                   << " conditional process signals\n";
    if (latches)
      llvm::errs() << "Bound " << latches << " native transparent latches\n";
    if (undriven)
      llvm::errs() << "Resolved " << undriven << " undriven native signals\n";
    if (coalesced)
      llvm::errs() << "Coalesced equivalent register drives on " << coalesced
                   << " native signals\n";
  }
};

// The SDK LLHD pipeline promotes unconditional signal cycles to SSA and drops
// their initializers. Keep its lowering order, inserting native feedback state
// immediately before that promotion. Clocked state still uses normal register
// and memory inference.
struct PrepareNativeLLHD
    : PassWrapper<PrepareNativeLLHD, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PrepareNativeLLHD)
  StringRef getArgument() const override {
    return "chipyard-prepare-native-llhd";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, seq::SeqDialect, comb::CombDialect,
                    llhd::LLHDDialect, arith::ArithDialect,
                    cf::ControlFlowDialect, scf::SCFDialect>();
  }
  void runOnOperation() override {
    OpPassManager pipeline(ModuleOp::getOperationName());
    pipeline.addNestedPass<hw::HWModuleOp>(llhd::createWrapProceduralOpsPass());
    pipeline.addPass(createSCFToControlFlowPass());
    pipeline.addPass(llhd::createInlineCallsPass());
    pipeline.addPass(createSymbolDCEPass());
    pipeline.addPass(std::make_unique<SnapshotGlobalSignalInitializers>());
    auto &module = pipeline.nest<hw::HWModuleOp>();
    module.addPass(std::make_unique<SnapshotSignalInitializers>());
    module.addPass(createSROA());
    if (failed(parsePassPipeline("chipyard-native-mem2reg", module))) {
      getOperation().emitError(
          "native LLHD preparation requires the NativeMem2Reg pass plugin");
      signalPassFailure();
      return;
    }
    module.addPass(llhd::createHoistSignalsPass());
    module.addPass(llhd::createDeseqPass());
    module.addPass(llhd::createLowerProcessesPass());
    module.addPass(createCSEPass());
    module.addPass(createCanonicalizerPass());
    module.addPass(llhd::createUnrollLoopsPass());
    module.addPass(createCSEPass());
    module.addPass(createCanonicalizerPass());
    module.addPass(llhd::createRemoveControlFlowPass());
    module.addPass(createCSEPass());
    module.addPass(createCanonicalizerPass());
    module.addPass(createMapArithToCombPass(true));
    module.addPass(llhd::createCombineDrivesPass());
    auto state = std::make_unique<SignalState>();
    state->feedbackOnly = true;
    pipeline.addPass(std::move(state));
    auto &promote = pipeline.nest<hw::HWModuleOp>();
    promote.addPass(llhd::createSig2Reg());
    promote.addPass(createCSEPass());
    promote.addPass(createCanonicalizerPass());
    promote.addPass(seq::createRegOfVecToMem());
    promote.addPass(createCSEPass());
    promote.addPass(createCanonicalizerPass());
    if (failed(runPipeline(pipeline, getOperation())))
      signalPassFailure();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardSignalState", "1", [] {
            PassRegistration<PushStructUpdates>();
            PassRegistration<SignalState>();
            PassRegistration<SnapshotSignalInitializers>();
            PassRegistration<SnapshotGlobalSignalInitializers>();
            PassRegistration<PrepareNativeLLHD>();
          }};
}
