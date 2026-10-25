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
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
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
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include <array>
#include <optional>

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
  SnapshotGlobalSignalInitializers() = default;
  SnapshotGlobalSignalInitializers(
      const SnapshotGlobalSignalInitializers &other)
      : PassWrapper(other) {}
  Option<bool> deferRuntime{*this, "defer-runtime", llvm::cl::init(false)};
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
        if (deferRuntime)
          continue;
        global.emitError("native global signal initialization requires a pure "
                         "constant startup value; runtime, effectful or cyclic "
                         "dependencies are unsupported");
        signalPassFailure();
        return;
      }
    for (auto global : globals) {
      if (global.getInitRegion().empty() || !values.contains(global))
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

// A declaration's DPI initializer runs once, before time-zero processes. Keep
// the effectful dependency graph inside seq.initial so ordinary evaluation
// cannot reexecute it. Closed declarations may use native storage when enabled;
// input-dependent, unbound contextual, escaping, or cyclic cases retain the
// startup-value diagnostic below. Native scope and storage handles may be
// captured under their initialize-before-model contract.
static bool readOnlyDeclaration(llhd::SignalOp signal) {
  return !signal->hasAttr("chipyard.dpi_captures") &&
         llvm::all_of(signal->getUsers(),
                      [](Operation *user) { return isa<llhd::ProbeOp>(user); });
}

// These generated binding outputs are established once by initialize(), before
// the compiled model's initial function. No ordinary input or blackbox output
// acquires this contract by having a constant-looking value.
static bool nativeStartupScope(Value value) {
  auto instance = value.getDefiningOp<hw::InstanceOp>();
  auto module =
      instance ? SymbolTable::lookupNearestSymbolFrom<hw::HWModuleExternOp>(
                     instance, instance.getModuleNameAttr())
               : hw::HWModuleExternOp{};
  auto name = module ? module.getVerilogName() : std::optional<StringRef>{};
  if (!name || (*name != "ArcDpiScope" && *name != "ArcDpiScopeRoot") ||
      instance.getNumOperands() || instance.getNumResults() != 1 ||
      !value.getType().isInteger(64))
    return false;
  bool root = false, mode = *name == "ArcDpiScopeRoot", scope = mode;
  for (auto param : instance.getParameters()) {
    auto entry = cast<hw::ParamDeclAttr>(param);
    auto text = dyn_cast_or_null<StringAttr>(entry.getValue());
    if (!text)
      return false;
    if (entry.getName() == "ROOTKEY")
      root = !text.getValue().empty();
    else if (entry.getName() == "MODE")
      mode = text.getValue() == "local" || text.getValue() == "root" ||
             text.getValue() == "global";
    else if (entry.getName() == "SCOPE")
      scope = true;
    else
      return false;
  }
  return root && mode && scope;
}

static bool nativeStartupMemory(Value value) {
  auto instance = value.getDefiningOp<hw::InstanceOp>();
  auto module =
      instance ? SymbolTable::lookupNearestSymbolFrom<hw::HWModuleExternOp>(
                     instance, instance.getModuleNameAttr())
               : hw::HWModuleExternOp{};
  auto name = module ? module.getVerilogName() : std::optional<StringRef>{};
  if (!name || *name != "ArcSignalMemory" || instance.getNumOperands() ||
      instance.getNumResults() != 2 || !value.getType().isInteger(64) ||
      cast<OpResult>(value).getResultNumber() != 0)
    return false;
  bool bits = false, initial = false;
  for (auto attr : instance.getParameters()) {
    auto entry = cast<hw::ParamDeclAttr>(attr);
    if (entry.getName() == "BITS") {
      auto size = dyn_cast_or_null<IntegerAttr>(entry.getValue());
      bits = size && size.getValue().getLimitedValue() > 0 &&
             size.getValue().getLimitedValue() <= (1u << 24);
    } else if (entry.getName() == "INIT") {
      initial = isa_and_nonnull<IntegerAttr>(entry.getValue());
    } else if (entry.getName() != "KEY" && entry.getName() != "CAPTURES" &&
               entry.getName() != "PATH" && entry.getName() != "STARTUP") {
      return false;
    } else if (!isa_and_nonnull<StringAttr>(entry.getValue())) {
      return false;
    }
  }
  return bits && initial;
}

static bool nativeInitialRead(Operation *op) {
  auto call = dyn_cast<sim::DPICallOp>(op);
  if (!call || call.getClock() || call.getEnable() ||
      call.getInputs().size() != 5 || call.getNumResults() != 1 ||
      !call.getResult(0).getType().isInteger(64) ||
      !nativeStartupMemory(call.getInputs()[0]) ||
      !llvm::all_of(call.getInputs().getTypes(),
                    [](Type type) { return type.isInteger(64); }))
    return false;
  auto fn = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
      call, call.getCalleeAttr());
  return fn && fn.getVerilogName() == StringRef("chipyard_arc_signal_read");
}

// Retain a single ordered startup region as native storage makes further
// declaration dependencies available. The original effect order is checked
// before any expression graph is moved.
static void freezeDeclarationDPI(hw::HWModuleOp module,
                                 bool nativeStorage = false,
                                 bool partial = false) {
  auto *body = module.getBodyBlock();
  seq::InitialOp previous;
  for (auto initial : body->getOps<seq::InitialOp>())
    if (initial->hasAttr("chipyard.declaration_startup")) {
      if (previous)
        return;
      previous = initial;
    }
  struct Slice {
    DenseMap<Value, Value> aliases;
    DenseSet<Operation *> ops;
    DenseSet<Value> captures, available, active;
    bool hasDPI = false;
  } combined;
  auto dependencies = [&](Operation *op) {
    SmallVector<Value> values;
    for (auto [index, operand] : llvm::enumerate(op->getOperands()))
      // The runtime's read version is only a sensitivity token. The startup
      // read happens at this exact program point and uses the documented zero
      // token, as compiled SV export reads already do.
      if (!nativeStorage || !nativeInitialRead(op) || index != 1)
        values.push_back(operand);
    return values;
  };
  SmallVector<sim::DPICallOp> unused;
  for (auto call : body->getOps<sim::DPICallOp>())
    if (call->hasAttr("chipyard.declaration_initializer") &&
        call.getNumResults() == 1 && call->use_empty())
      unused.push_back(call);
  for (auto call : unused) {
    OpBuilder builder(call);
    llhd::SignalOp::create(builder, call.getLoc(),
                           llhd::RefType::get(call.getResult(0).getType()),
                           StringAttr{}, call.getResult(0));
  }
  DenseMap<Operation *, unsigned> position;
  for (auto [index, op] : llvm::enumerate(*body))
    position[&op] = index;
  SmallVector<llhd::SignalOp> pending;
  for (auto signal : body->getOps<llhd::SignalOp>())
    if (signal.getInit() && !matchPattern(signal.getInit(), m_Constant()) &&
        !signal.getInit().getDefiningOp<seq::FromImmutableOp>())
      pending.push_back(signal);
  llvm::stable_sort(pending, [&](auto a, auto b) {
    auto *first = a.getInit().getDefiningOp();
    auto *next = b.getInit().getDefiningOp();
    return position.lookup(first && first->getBlock() == body ? first : a) <
           position.lookup(next && next->getBlock() == body ? next : b);
  });
  SmallVector<llhd::SignalOp> signals;
  for (auto signal : pending) {
    if (!nativeStorage && !readOnlyDeclaration(signal))
      continue;
    Slice slice;
    std::function<bool(Value, unsigned)> visit = [&](Value value,
                                                     unsigned depth) {
      if (slice.aliases.contains(value))
        return true;
      if (depth >= 1024 || slice.ops.size() >= 4096 ||
          !slice.active.insert(value).second)
        return false;
      auto *op = value.getDefiningOp();
      if (!op || op->getBlock() != body || op->getNumRegions())
        return false;
      if (auto from = dyn_cast<seq::FromImmutableOp>(op)) {
        if (!previous || from.getInput().getDefiningOp() != previous)
          return false;
        auto index = cast<OpResult>(from.getInput()).getResultNumber();
        auto oldValue =
            previous.getBodyBlock()->getTerminator()->getOperand(index);
        slice.aliases[value] = oldValue;
        slice.available.insert(oldValue);
        slice.hasDPI = true;
      } else if (nativeStartupScope(value) ||
                 (nativeStorage && nativeStartupMemory(value))) {
        slice.captures.insert(value);
        slice.aliases[value] = value;
      } else if (auto probe = dyn_cast<llhd::ProbeOp>(op)) {
        auto source = probe.getSignal().getDefiningOp<llhd::SignalOp>();
        if (!source || !source.getInit() || !readOnlyDeclaration(source) ||
            !visit(source.getInit(), depth + 1))
          return false;
        slice.aliases[value] = slice.aliases.lookup(source.getInit());
      } else {
        if (auto call = dyn_cast<sim::DPICallOp>(op)) {
          auto fn = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
              call, call.getCalleeAttr());
          auto decl = call->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
          if (!decl && fn)
            decl = fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
          auto context = decl ? decl.getAs<BoolAttr>("context") : BoolAttr{};
          auto task = decl ? decl.getAs<BoolAttr>("task") : BoolAttr{};
          if (call.getClock() || call.getEnable() || !context || !task ||
              task.getValue())
            return false;
          if (context.getValue() &&
              (!fn || !fn->hasAttr("chipyard.native_context_scope") ||
               call.getInputs().empty() ||
               !nativeStartupScope(call.getInputs()[0])))
            return false;
          slice.hasDPI = true;
        } else if (!isPure(op)) {
          return false;
        }
        for (auto operand : dependencies(op))
          if (!visit(operand, depth + 1))
            return false;
        slice.ops.insert(op);
        for (auto result : op->getResults())
          slice.aliases[result] = result;
      }
      slice.active.erase(value);
      return true;
    };
    if (!visit(signal.getInit(), 0)) {
      if (partial)
        break;
      continue;
    }
    if (!slice.hasDPI)
      continue;
    signals.push_back(signal);
    combined.ops.insert(slice.ops.begin(), slice.ops.end());
    combined.captures.insert(slice.captures.begin(), slice.captures.end());
    combined.available.insert(slice.available.begin(), slice.available.end());
    for (auto [value, alias] : slice.aliases)
      combined.aliases[value] = alias;
  }
  if (signals.empty() || combined.ops.size() > 4096)
    return;
  DenseSet<Operation *> visited;
  SmallVector<Operation *> ordered, calls, originalCalls;
  SmallVector<SmallVector<Operation *>> plans;
  auto effect = [&](Operation *op) {
    return isa<sim::DPICallOp>(op) &&
           (!nativeStorage || !nativeInitialRead(op));
  };
  DenseMap<Operation *, bool> temporal;
  std::function<bool(Value)> samplesStorage = [&](Value value) {
    value = combined.aliases.lookup(value);
    if (combined.captures.contains(value) || combined.available.contains(value))
      return false;
    auto *op = value.getDefiningOp();
    if (auto found = temporal.find(op); found != temporal.end())
      return found->second;
    bool sampled = nativeStorage && nativeInitialRead(op);
    // A user call's result is fixed at that original call. A later declaration
    // can resample native storage, but cannot duplicate that user's callback.
    if (!sampled && !effect(op))
      sampled = llvm::any_of(dependencies(op), samplesStorage);
    temporal[op] = sampled;
    return sampled;
  };
  for (auto signal : signals) {
    SmallVector<Operation *> plan;
    DenseSet<Operation *> local;
    std::function<void(Value)> order = [&](Value value) {
      value = combined.aliases.lookup(value);
      if (combined.captures.contains(value) ||
          combined.available.contains(value))
        return;
      auto *op = value.getDefiningOp();
      if (visited.contains(op) || !local.insert(op).second)
        return;
      for (auto operand : dependencies(op))
        order(operand);
      plan.push_back(op);
      ordered.push_back(op);
      if (effect(op))
        calls.push_back(op);
      if (!samplesStorage(value))
        visited.insert(op);
    };
    order(signal.getInit());
    plans.push_back(std::move(plan));
  }
  for (auto &op : *body)
    if (combined.ops.contains(&op) && effect(&op))
      originalCalls.push_back(&op);
  if (calls != originalCalls)
    return;
  DenseSet<Operation *> tainted, destinations;
  for (auto signal : signals)
    destinations.insert(signal);
  for (auto *op : ordered) {
    bool depends =
        effect(op) || llvm::any_of(dependencies(op), [&](Value value) {
          return tainted.contains(
              combined.aliases.lookup(value).getDefiningOp());
        });
    if (!depends)
      continue;
    tainted.insert(op);
    for (auto *user : op->getUsers())
      if (!combined.ops.contains(user) && !destinations.contains(user))
        return;
  }
  OpBuilder outer(previous ? previous.getOperation() : &body->front());
  SmallVector<Type> types;
  SmallVector<Value> handles;
  if (previous) {
    for (auto result : previous.getResults())
      types.push_back(
          cast<seq::ImmutableType>(result.getType()).getInnerType());
    llvm::append_range(handles, previous.getInputs());
  }
  unsigned oldResults = types.size();
  for (auto signal : signals)
    types.push_back(signal.getInit().getType());
  auto initial = seq::InitialOp::create(outer, module.getLoc(), types);
  initial->setAttr("chipyard.declaration_startup", outer.getUnitAttr());
  if (previous)
    initial.getBody().takeBody(previous.getBody());
  auto *block = initial.getBodyBlock();
  IRMapping mapping;
  for (auto value : combined.available)
    mapping.map(value, value);
  DenseSet<Value> captured;
  for (auto [handle, argument] : llvm::zip(handles, block->getArguments())) {
    auto cast = handle.getDefiningOp<UnrealizedConversionCastOp>();
    if (cast && cast.getNumOperands() == 1) {
      captured.insert(cast.getOperand(0));
      mapping.map(cast.getOperand(0), argument);
    }
  }
  for (auto &op : *body)
    for (auto value : op.getResults())
      if (combined.captures.contains(value) && captured.insert(value).second) {
        auto cast = UnrealizedConversionCastOp::create(
            outer, value.getLoc(),
            TypeRange{seq::ImmutableType::get(value.getType())},
            ValueRange{value});
        cast->setAttr("chipyard.native_startup_input", outer.getUnitAttr());
        handles.push_back(cast.getResult(0));
        mapping.map(value, block->addArgument(value.getType(), value.getLoc()));
      }
  initial.getInputsMutable().assign(handles);
  OpBuilder builder(block, block->end());
  Operation *yield = previous ? block->getTerminator() : nullptr;
  if (yield)
    builder.setInsertionPoint(yield);
  SmallVector<Value> values;
  if (yield)
    llvm::append_range(values, yield->getOperands());
  for (auto [signal, plan] : llvm::zip(signals, plans)) {
    for (auto *op : plan) {
      for (auto operand : dependencies(op))
        mapping.map(operand, mapping.lookup(combined.aliases.lookup(operand)));
      if (nativeStorage && nativeInitialRead(op))
        mapping.map(
            op->getOperand(1),
            hw::ConstantOp::create(builder, op->getLoc(), APInt(64, 0)));
      builder.clone(*op, mapping);
    }
    values.push_back(mapping.lookup(combined.aliases.lookup(signal.getInit())));
  }
  if (yield)
    yield->setOperands(values);
  else
    seq::YieldOp::create(builder, module.getLoc(), values);
  for (auto [index, signal] : llvm::enumerate(signals))
    signal.getInitMutable().assign(seq::FromImmutableOp::create(
        outer, signal.getLoc(), signal.getInit().getType(),
        initial.getResult(oldResults + index)));
  if (previous) {
    for (auto [before, after] :
         llvm::zip(previous.getResults(), initial.getResults()))
      before.replaceAllUsesWith(after);
    previous.erase();
  }
  DenseSet<Operation *> erased;
  for (auto *op : llvm::reverse(ordered))
    if (!erased.contains(op) && op->use_empty()) {
      erased.insert(op);
      op->erase();
    }
}

// Signal initializers run before time-zero processes and continuous drives.
// Cloning their pure dependency graph replaces reads of another signal with
// that signal's initial value. Leaving a probe in this graph would turn an
// undriven static variable into a live combinational alias during sig2reg.
struct SnapshotSignalInitializers
    : PassWrapper<SnapshotSignalInitializers, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SnapshotSignalInitializers)
  SnapshotSignalInitializers() = default;
  SnapshotSignalInitializers(const SnapshotSignalInitializers &other)
      : PassWrapper(other) {}
  Option<bool> nativeStorage{*this, "native-storage", llvm::cl::init(false)};
  Option<bool> deferRuntime{*this, "defer-runtime", llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-snapshot-signal-initializers";
  }
  void runOnOperation() override {
    auto module = getOperation();
    freezeDeclarationDPI(module, nativeStorage, deferRuntime);
    auto *body = module.getBodyBlock();
    OpBuilder builder(body, body->begin());
    DenseMap<Value, Value> initial;
    DenseSet<Value> dynamicInitial;
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
      if (isa<seq::FromImmutableOp>(op)) {
        dynamicInitial.insert(value);
        return finish(value);
      }
      if (auto probe = dyn_cast<llhd::ProbeOp>(op)) {
        auto signal = probe.getSignal().getDefiningOp<llhd::SignalOp>();
        if (signal && deferRuntime && !readOnlyDeclaration(signal) &&
            (signal->hasAttr("chipyard.dpi_captures") ||
             (signal.getInit() &&
              !matchPattern(signal.getInit(), m_Constant()))))
          return finish({});
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
      bool dynamic = llvm::any_of(op->getOperands(), [&](Value operand) {
        return dynamicInitial.contains(mapping.lookup(operand));
      });
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
      if (dynamic)
        for (auto result : op->getResults()) {
          auto replacement = initial.lookup(result);
          // A folded constant no longer depends on immutable startup storage.
          if (!matchPattern(replacement, m_Constant()))
            dynamicInitial.insert(replacement);
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
      if (!value || (dynamicInitial.contains(value) &&
                     !readOnlyDeclaration(signal) && !nativeStorage)) {
        if (deferRuntime)
          continue;
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

// Native signal-memory providers are initialized before the model's initial
// function. Materialize immutable declaration values into that existing storage
// before lowering clocked or asynchronous procedural writes.
struct PrepareDeclarationInitializers
    : PassWrapper<PrepareDeclarationInitializers, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PrepareDeclarationInitializers)
  StringRef getArgument() const override {
    return "chipyard-prepare-declaration-initializers";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, seq::SeqDialect, comb::CombDialect,
                    llhd::LLHDDialect, sim::SimDialect, func::FuncDialect,
                    LLVM::LLVMDialect, cf::ControlFlowDialect, ub::UBDialect,
                    arith::ArithDialect>();
  }
  void runOnOperation() override {
    auto measure = [&]() {
      std::array<unsigned, 3> counts{};
      getOperation().walk([&](llhd::SignalOp signal) {
        ++counts[0];
        if (signal.getInit() && !matchPattern(signal.getInit(), m_Constant()) &&
            !signal.getInit().getDefiningOp<seq::FromImmutableOp>())
          ++counts[1];
      });
      getOperation().walk([&](sim::DPICallOp call) {
        if (isa<hw::HWModuleOp>(call->getParentOp()) &&
            call->hasAttr("chipyard.declaration_initializer"))
          ++counts[2];
      });
      return counts;
    };
    auto before = measure();
    unsigned limit = 4 * (before[0] + before[2] + 1);
    for (unsigned iteration = 0; iteration < limit; ++iteration) {
      OpPassManager normalize(ModuleOp::getOperationName());
      auto partial = std::make_unique<SnapshotSignalInitializers>();
      partial->nativeStorage = true;
      partial->deferRuntime = true;
      normalize.addNestedPass<hw::HWModuleOp>(std::move(partial));
      if (failed(runPipeline(normalize, getOperation()))) {
        signalPassFailure();
        return;
      }
      bool needed = false;
      getOperation().walk([&](llhd::SignalOp signal) {
        needed |= signal.getInit() &&
                  signal.getInit().getDefiningOp<seq::FromImmutableOp>() &&
                  (!readOnlyDeclaration(signal) ||
                   signal->hasAttr("chipyard.dpi_captures"));
      });
      if (needed) {
        OpPassManager storage(ModuleOp::getOperationName());
        if (failed(parsePassPipeline(
                "chipyard-native-signal-memory{initializers-only=true}",
                storage)) ||
            failed(runPipeline(storage, getOperation()))) {
          signalPassFailure();
          return;
        }
      }
      auto after = measure();
      if (after == before)
        return;
      before = after;
    }
    getOperation().emitError(
        "native declaration initialization dependency graph did not stabilize");
    signalPassFailure();
  }
};

// Preserve the single-block immutable startup frame while using the SDK's
// conversion patterns and operation set for ordinary procedural SCF. Startup
// SCF is lowered later by the normal Arc/LLVM pipeline after state allocation.
struct LowerProceduralSCF
    : PassWrapper<LowerProceduralSCF, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerProceduralSCF)
  void getDependentDialects(DialectRegistry &registry) const override {
    registry
        .insert<arith::ArithDialect, cf::ControlFlowDialect, scf::SCFDialect>();
  }
  void runOnOperation() override {
    RewritePatternSet patterns(&getContext());
    populateSCFToControlFlowConversionPatterns(patterns);
    ConversionTarget target(getContext());
    target.addDynamicallyLegalOp<scf::ForallOp, scf::ForOp, scf::IfOp,
                                 scf::IndexSwitchOp, scf::ParallelOp,
                                 scf::WhileOp, scf::ExecuteRegionOp>(
        [](Operation *op) {
          return bool(op->getParentOfType<seq::InitialOp>());
        });
    target.markUnknownOpDynamicallyLegal([](Operation *) { return true; });
    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns))))
      signalPassFailure();
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
    PrepareDeclarationInitializers().getDependentDialects(registry);
  }
  void runOnOperation() override {
    OpPassManager pipeline(ModuleOp::getOperationName());
    pipeline.addNestedPass<hw::HWModuleOp>(llhd::createWrapProceduralOpsPass());
    pipeline.addPass(std::make_unique<LowerProceduralSCF>());
    pipeline.addPass(llhd::createInlineCallsPass());
    pipeline.addPass(createSymbolDCEPass());
    pipeline.addPass(std::make_unique<SnapshotGlobalSignalInitializers>());
    pipeline.addPass(std::make_unique<PrepareDeclarationInitializers>());
    auto &snapshot = pipeline.nest<hw::HWModuleOp>();
    auto initializers = std::make_unique<SnapshotSignalInitializers>();
    initializers->nativeStorage = true;
    snapshot.addPass(std::move(initializers));
    auto &module = pipeline.nest<hw::HWModuleOp>();
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
            PassRegistration<PrepareDeclarationInitializers>();
            PassRegistration<PrepareNativeLLHD>();
          }};
}
