// Retain explicit state provenance, then outline low-interface state regions.
// The planner runs while allocations still name state objects.  The late
// materializer runs after allocation, while StateRead/StateWrite remain typed.
// It never guesses that either native current/staged buffer is immutable.
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>

using namespace mlir;
using namespace circt;
namespace {
constexpr StringLiteral planAttr = "chipyard.state_partition.plan";
constexpr StringLiteral objectAttr = "chipyard.state_partition.object";
constexpr StringLiteral readAttr = "chipyard.state_partition.read";
constexpr StringLiteral versionAttr = "chipyard.state_partition.version";
constexpr StringLiteral epochAttr = "chipyard.state_partition.epoch";

static bool allocation(Operation *op) {
  return isa<arc::AllocStateOp, arc::AllocMemoryOp, arc::AllocStorageOp,
             arc::RootInputOp, arc::RootOutputOp>(op);
}
static bool address(Operation *op) {
  return isa<arc::StorageGetOp>(op) || allocation(op);
}
static bool scalarState(Value value) {
  auto type = dyn_cast<arc::StateType>(value.getType());
  return type && isa<IntegerType>(type.getType());
}
static bool scalarValue(Type type) {
  return isa<IntegerType>(type) || hw::isHWValueType(type);
}
static std::optional<uint64_t> plannedID(IntegerAttr attribute) {
  if (!attribute || attribute.getValue().getBitWidth() != 64 ||
      attribute.getValue().isNegative() || attribute.getValue().isZero())
    return std::nullopt;
  return attribute.getValue().getZExtValue();
}
static bool constant(Operation *op) {
  return op && !op->getNumOperands() && !op->getNumRegions() &&
         op->hasTrait<OpTrait::ConstantLike>() &&
         llvm::all_of(op->getResultTypes(), scalarValue);
}

// Deliberately whitelist integer hardware operations.  In particular native
// foreign declarations, formatted values, floating-environment operations,
// freezes, pointer-valued arrays and arbitrary effect annotations do not make
// an operation eligible. Defined callees are checked transitively.
struct PureOperations {
  DenseMap<Operation *, unsigned> calleeState;
  DenseMap<Operation *, uint64_t> calleeWork;
  bool primitive(Operation *op) {
    if (op->getNumRegions() || op->hasTrait<OpTrait::IsTerminator>())
      return false;
    StringRef dialect = op->getName().getDialectNamespace();
    if (dialect != "comb" && dialect != "arith" &&
        !isa<hw::ConstantOp, hw::BitcastOp>(op))
      return false;
    if (!isMemoryEffectFree(op))
      return false;
    return llvm::all_of(op->getOperandTypes(), scalarValue) &&
           llvm::all_of(op->getResultTypes(), scalarValue) &&
           llvm::none_of(op->getOperandTypes(), [](Type t) {
             return isa<FloatType>(t);
           }) && llvm::none_of(op->getResultTypes(), [](Type t) {
             return isa<FloatType>(t);
           });
  }
  Operation *callee(Operation *op) {
    FlatSymbolRefAttr symbol;
    if (isa<arc::CallOp>(op))
      symbol = op->getAttrOfType<FlatSymbolRefAttr>("arc");
    else if (isa<func::CallOp>(op))
      symbol = op->getAttrOfType<FlatSymbolRefAttr>("callee");
    if (!symbol)
      return nullptr;
    return SymbolTable::lookupNearestSymbolFrom(op, symbol);
  }
  bool pure(Operation *op) {
    if (primitive(op))
      return true;
    auto *target = callee(op);
    if (!target || target->getNumRegions() != 1 ||
        !llvm::hasSingleElement(target->getRegion(0)))
      return false;
    if (!llvm::all_of(op->getOperandTypes(), scalarValue) ||
        !llvm::all_of(op->getResultTypes(), scalarValue))
      return false;
    auto found = calleeState.find(target);
    if (found != calleeState.end())
      return found->second == 2;
    calleeState[target] = 1; // A recursive or cyclic call graph is a barrier.
    for (auto &nested : target->getRegion(0).front()) {
      if (isa<arc::OutputOp, func::ReturnOp>(nested))
        continue;
      if (!pure(&nested)) {
        calleeState[target] = 3;
        return false;
      }
    }
    calleeState[target] = 2;
    return true;
  }
  uint64_t work(Operation *op) {
    auto *target = callee(op);
    if (!target || !pure(op))
      return 1;
    auto found = calleeWork.find(target);
    if (found != calleeWork.end())
      return found->second;
    uint64_t total = 1;
    for (auto &nested : target->getRegion(0).front())
      total += work(&nested);
    calleeWork[target] = total;
    return total;
  }
};

static LogicalResult report(StringRef file, llvm::json::Object data,
                            Operation *op) {
  if (file.empty())
    return success();
  std::error_code error;
  llvm::raw_fd_ostream stream(file, error);
  if (error)
    return op->emitError("cannot write state partition report: ")
           << error.message();
  stream << llvm::json::Value(std::move(data)) << '\n';
  stream.close();
  if (stream.has_error())
    return op->emitError("cannot finish state partition report");
  return success();
}

struct StatePartitionPlan
    : PassWrapper<StatePartitionPlan, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(StatePartitionPlan)
  StatePartitionPlan() = default;
  StatePartitionPlan(const StatePartitionPlan &other) : PassWrapper(other) {}
  Option<std::string> reportFile{*this, "report-file", llvm::cl::init("")};
  StringRef getArgument() const override {
    return "chipyard-state-partition-plan";
  }
  void runOnOperation() override {
    uint64_t objects = 0, reads = 0, writes = 0, barriers = 0;
    Builder builder(&getContext());
    PureOperations pure;
    for (auto model : getOperation().getOps<arc::ModelOp>()) {
      uint64_t nextObject = 0, nextVersion = 0, epoch = 0;
      DenseMap<Value, uint64_t> identities, versions;
      model.walk([&](Operation *op) {
        if (allocation(op) && op->getNumResults() == 1) {
          identities[op->getResult(0)] = ++nextObject;
          op->setAttr(objectAttr, builder.getI64IntegerAttr(nextObject));
          ++objects;
        }
      });
      // These numbers describe the original syntactic provenance, not a
      // feasible-path join calculation. In particular nested branches can
      // conservatively advance versions. The materializer never uses them
      // as an absence-of-write proof: it reconstructs physical objects and
      // examines all actual typed writes/effect barriers after allocation.
      model.walk<WalkOrder::PreOrder>([&](Operation *op) {
        if (auto read = dyn_cast<arc::StateReadOp>(op)) {
          auto found = identities.find(read.getState());
          if (found != identities.end() && scalarState(read.getState())) {
            read->setAttr(readAttr, builder.getI64IntegerAttr(found->second));
            ++reads;
          }
        } else if (auto write = dyn_cast<arc::StateWriteOp>(op)) {
          versions[write.getState()] = ++nextVersion;
          ++writes;
        } else if (op != model && !allocation(op) && !address(op) &&
                   !pure.pure(op) && !isa<scf::IfOp>(op) &&
                   !op->hasTrait<OpTrait::IsTerminator>()) {
          ++epoch;
          ++barriers;
        }
      });
      model->setAttr(planAttr, builder.getI64IntegerAttr(1));
    }
    if (failed(report(reportFile,
                      llvm::json::Object{{"schema", 1}, {"status", "planned_explicit_state"},
                       {"objects", objects}, {"scalar_reads", reads},
                       {"writes", writes}, {"effect_epochs", barriers}},
                      getOperation())))
      signalPassFailure();
  }
};

struct Location {
  Value base;
  uint64_t offset = 0, bytes = 0;
};
struct StatePartitionMaterialize
    : PassWrapper<StatePartitionMaterialize, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(StatePartitionMaterialize)
  StatePartitionMaterialize() = default;
  StatePartitionMaterialize(const StatePartitionMaterialize &other)
      : PassWrapper(other) {}
  Option<unsigned> minOps{*this, "min-ops", llvm::cl::init(64)};
  Option<unsigned> maxOps{*this, "max-ops", llvm::cl::init(4096)};
  Option<unsigned> maxWords{*this, "max-input-words", llvm::cl::init(8)};
  Option<std::string> reportFile{*this, "report-file", llvm::cl::init("")};
  StringRef getArgument() const override {
    return "chipyard-state-partition-materialize";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<func::FuncDialect, LLVM::LLVMDialect>();
  }
  struct Object {
    Location location;
    uint64_t identity;
    SmallVector<uint64_t> writes;
  };
  SmallVector<Object> objects;
  DenseMap<Value, std::map<uint64_t, unsigned>> byBase;
  DenseMap<Operation *, uint64_t> positions;
  SmallVector<uint64_t> barriers;
  PureOperations pure;
  std::unique_ptr<DominanceInfo> dominance;
  ModuleOp module;
  std::string modelName;
  uint64_t serial = 0, helperCount = 0, outlinedOps = 0, freshReads = 0;
  uint64_t rejectedWide = 0, rejectedMissingPlan = 0, rejectedObject = 0;
  uint64_t rejectedTrace = 0;
  llvm::json::Array regions;

  // Allocations remain in their original model and order.  Only constant
  // address calculations are cloned; allocations are never duplicated.
  std::optional<Location> locate(Value value) {
    Location result;
    Type type = value.getType();
    if (auto state = dyn_cast<arc::StateType>(type)) {
      if (!isa<IntegerType>(state.getType()))
        return std::nullopt;
      result.bytes = state.getByteWidth();
    }
    SmallPtrSet<Operation *, 8> seen;
    while (auto *op = value.getDefiningOp()) {
      if (!seen.insert(op).second || !address(op) ||
          op->getNumOperands() != 1)
        return std::nullopt;
      auto offset = op->getAttrOfType<IntegerAttr>("offset");
      if (!offset || offset.getValue().isNegative() ||
          offset.getValue().getActiveBits() > 63)
        return std::nullopt;
      auto add = offset.getValue().getZExtValue();
      if (add > std::numeric_limits<uint64_t>::max() - result.offset)
        return std::nullopt;
      result.offset += add;
      value = op->getOperand(0);
    }
    if (!isa<arc::StorageType>(value.getType()))
      return std::nullopt;
    result.base = value;
    return result;
  }
  std::optional<unsigned> object(Value handle) {
    auto loc = locate(handle);
    if (!loc || !loc->bytes)
      return std::nullopt;
    auto base = byBase.find(loc->base);
    if (base == byBase.end())
      return std::nullopt;
    auto it = base->second.find(loc->offset);
    if (it == base->second.end() ||
        objects[it->second].location.bytes != loc->bytes)
      return std::nullopt;
    return it->second;
  }
  bool certifiedRead(arc::StateReadOp read) {
    if (!scalarState(read.getState()))
      return false;
    auto id = plannedID(read->getAttrOfType<IntegerAttr>(readAttr));
    auto index = object(read.getState());
    return id && index &&
           *id == objects[*index].identity;
  }
  bool noEventBetween(ArrayRef<uint64_t> events, uint64_t from, uint64_t to) {
    auto it = llvm::upper_bound(events, from);
    return it == events.end() || *it >= to;
  }
  bool reload(arc::StateReadOp read, Operation *at) {
    if (!certifiedRead(read) || !positions.count(read) || !positions.count(at) ||
        !dominance->properlyDominates(read.getOperation(), at))
      return false;
    uint64_t from = positions.lookup(read), to = positions.lookup(at);
    auto index = object(read.getState());
    return from < to && noEventBetween(barriers, from, to) &&
           noEventBetween(objects[*index].writes, from, to);
  }
  // Unknown regions are barriers on both sides. Only scf.if regions are
  // entered for extraction, so loop-carried/coroutine state is never inferred.
  void timeline(Block &block, uint64_t &clock) {
    for (auto &op : block) {
      uint64_t pos = ++clock;
      positions[&op] = pos;
      if (auto write = dyn_cast<arc::StateWriteOp>(op)) {
        if (auto index = object(write.getState()))
          objects[*index].writes.push_back(pos);
        else
          barriers.push_back(pos);
      } else if (auto read = dyn_cast<arc::StateReadOp>(op)) {
        if (!certifiedRead(read))
          barriers.push_back(pos);
      } else if (allocation(&op) || address(&op) || pure.pure(&op) ||
                 op.hasTrait<OpTrait::IsTerminator>()) {
        // No memory version change.
      } else if (!isa<scf::IfOp>(op)) {
        barriers.push_back(pos);
      }
      for (auto &region : op.getRegions())
        for (auto &nested : region)
          timeline(nested, clock);
      if (op.getNumRegions() && !isa<scf::IfOp>(op))
        barriers.push_back(++clock);
    }
  }
  uint64_t words(Type type) {
    if (isa<arc::StorageType, arc::StateType, arc::MemoryType>(type))
      return 1;
    int64_t bits = hw::getBitWidth(type);
    if (bits < 0)
      return std::numeric_limits<uint32_t>::max();
    return (uint64_t(bits) + 63) / 64;
  }
  bool candidate(Operation *op) {
    if (auto read = dyn_cast<arc::StateReadOp>(op))
      return certifiedRead(read);
    // Keep state consumers and their update together. The whole contiguous
    // slice executes at its original position, in original operation order;
    // writes are not sunk/hoisted or exposed through a different alias. Trace
    // observers have already caused whole-model fallback above.
    if (auto write = dyn_cast<arc::StateWriteOp>(op))
      return scalarState(write.getState()) && bool(object(write.getState()));
    return pure.pure(op);
  }
  bool rematerializeAddress(Operation *op) {
    return op && address(op) && op->getNumOperands() == 1 &&
           op->getAttrOfType<IntegerAttr>("offset") &&
           op->getNumResults() == 1 && locate(op->getResult(0));
  }
  void outline(SmallVector<Operation *> &ops) {
    if (ops.size() < minOps) {
      ops.clear();
      return;
    }
    SmallPtrSet<Operation *, 32> members;
    members.insert(ops.begin(), ops.end());
    llvm::SetVector<Value> inputs, outputs;
    llvm::SetVector<Operation *> remats;
    Operation *insertion = ops.front();
    uint64_t clonedReads = 0;
    std::function<void(Value)> gather = [&](Value value) {
      auto *def = value.getDefiningOp();
      if (def && members.contains(def))
        return;
      bool canClone = def &&
                      (constant(def) || rematerializeAddress(def) ||
                       (isa<arc::StateReadOp>(def) &&
                        reload(cast<arc::StateReadOp>(def), insertion)));
      if (canClone) {
        if (remats.contains(def))
          return;
        for (Value operand : def->getOperands())
          gather(operand);
        remats.insert(def);
        clonedReads += isa<arc::StateReadOp>(def);
      } else
        inputs.insert(value);
    };
    for (auto *op : ops) {
      for (Value value : op->getOperands())
        gather(value);
      for (Value result : op->getResults())
        if (llvm::any_of(result.getUsers(), [&](Operation *user) {
              return !members.contains(user);
            }))
          outputs.insert(result);
    }
    uint64_t inputWords = 0, outputWords = 0;
    uint64_t stateReads = 0, stateWrites = 0, transitiveWork = 0;
    for (Operation *op : ops) {
      stateReads += isa<arc::StateReadOp>(op);
      stateWrites += isa<arc::StateWriteOp>(op);
      transitiveWork += pure.work(op);
    }
    for (Value value : inputs)
      inputWords += words(value.getType());
    for (Value value : outputs)
      outputWords += words(value.getType());
    if (inputWords > maxWords || outputWords > maxWords ||
        (outputs.empty() && !stateWrites)) {
      ++rejectedWide;
      ops.clear();
      return;
    }
    // Cloned state references must ultimately be a dominating original model
    // storage value; no frame owner, temporary address or opaque value escapes.
    for (Value value : inputs)
      if ((!scalarValue(value.getType()) &&
           !isa<arc::StorageType>(value.getType())) ||
          !dominance->dominates(value, insertion)) {
        ++rejectedObject;
        ops.clear();
        return;
      }
    SmallVector<Type> inTypes, outTypes;
    for (Value value : inputs)
      inTypes.push_back(value.getType());
    for (Value value : outputs)
      outTypes.push_back(value.getType());
    OpBuilder builder(module.getContext());
    builder.setInsertionPointToEnd(module.getBody());
    std::string name = modelName + "_state_region_" + std::to_string(serial++);
    while (module.lookupSymbol(name))
      name += "_";
    auto helper = func::FuncOp::create(
        builder, insertion->getLoc(), name,
        builder.getFunctionType(inTypes, outTypes));
    helper.setPrivate();
    helper->setAttr("llvm.no_inline", builder.getUnitAttr());
    helper->setAttr("chipyard.state_partition.helper", builder.getUnitAttr());
    helper->setAttr("llvm.linkage", LLVM::LinkageAttr::get(
        module.getContext(), LLVM::Linkage::Internal));
    Block *entry = helper.addEntryBlock();
    IRMapping mapping;
    for (auto [source, destination] : llvm::zip(inputs, entry->getArguments()))
      mapping.map(source, destination);
    builder.setInsertionPointToStart(entry);
    for (Operation *op : remats) {
      if (allocation(op)) {
        // Rebase an already allocated object, never allocate a second copy.
        auto view = arc::StorageGetOp::create(
            builder, op->getLoc(), op->getResult(0).getType(),
            mapping.lookupOrDefault(op->getOperand(0)),
            op->getAttrOfType<IntegerAttr>("offset"));
        mapping.map(op->getResult(0), view.getResult());
      } else
        builder.clone(*op, mapping);
    }
    OpBuilder caller(insertion);
    auto call = func::CallOp::create(caller, insertion->getLoc(), helper,
                                     inputs.getArrayRef());
    for (Operation *op : ops) {
      op->moveBefore(entry, entry->end());
      for (OpOperand &operand : op->getOpOperands())
        operand.set(mapping.lookupOrDefault(operand.get()));
    }
    builder.setInsertionPointToEnd(entry);
    func::ReturnOp::create(builder, helper.getLoc(), outputs.getArrayRef());
    for (auto [original, replacement] : llvm::zip(outputs, call.getResults())) {
      Value value = original;
      value.replaceUsesWithIf(replacement, [&](OpOperand &use) {
        return use.getOwner()->getParentOfType<func::FuncOp>() != helper;
      });
    }
    regions.push_back(llvm::json::Object{
        {"helper", name}, {"operations", ops.size()},
        {"operations_including_pure_callees", transitiveWork},
        {"input_words", inputWords}, {"output_words", outputWords},
        {"typed_state_reads", stateReads}, {"typed_state_writes", stateWrites},
        {"rematerialized_reads", clonedReads},
        {"rematerialized_addresses_and_constants", remats.size() - clonedReads}});
    ++helperCount;
    outlinedOps += ops.size();
    freshReads += clonedReads;
    ops.clear();
  }
  void block(Block &body) {
    // Existing predicates remain at the caller. Initial/final, loops and
    // coroutine regions retain their exact parent and are not extracted.
    SmallVector<scf::IfOp> conditionals;
    for (auto conditional : body.getOps<scf::IfOp>())
      conditionals.push_back(conditional);
    for (auto conditional : conditionals)
      for (auto &region : conditional->getRegions())
        for (auto &nested : region)
          block(nested);
    SmallVector<Operation *> ops;
    for (auto &op : llvm::make_early_inc_range(body)) {
      if (constant(&op) || address(&op))
        continue;
      if (!candidate(&op)) {
        outline(ops);
        continue;
      }
      if (ops.size() >= maxOps)
        outline(ops);
      ops.push_back(&op);
    }
    outline(ops);
  }
  void runOnOperation() override {
    if (!minOps || !maxWords || maxOps < minOps) {
      getOperation().emitError("invalid state partition operation/interface budgets");
      signalPassFailure();
      return;
    }
    module = getOperation();
    dominance = std::make_unique<DominanceInfo>(module);
    SmallVector<arc::ModelOp> models;
    for (auto model : module.getOps<arc::ModelOp>())
      models.push_back(model);
    for (auto model : models) {
      auto plan = plannedID(model->getAttrOfType<IntegerAttr>(planAttr));
      if (!plan || *plan != 1) {
        ++rejectedMissingPlan;
        continue;
      }
      // Trace insertion may turn a typed state write into an opaque runtime
      // callback later. The initial implementation has no contract allowing
      // reloads across such observers, so keep the entire model unchanged.
      bool traced = bool(model->getAttr("traceTaps"));
      model.walk([&](arc::StateWriteOp write) {
        traced |= bool(write->getAttr("traceTapModel")) ||
                  bool(write->getAttr("traceTapIndex"));
      });
      if (traced) {
        ++rejectedTrace;
        continue;
      }
      objects.clear();
      byBase.clear();
      positions.clear();
      barriers.clear();
      bool invalid = false;
      model.walk([&](Operation *op) {
        auto rawIdentity = op->getAttrOfType<IntegerAttr>(objectAttr);
        auto identity = plannedID(rawIdentity);
        if (rawIdentity && !identity) {
          invalid = true;
          return;
        }
        if (!identity || op->getNumResults() != 1 ||
            !scalarState(op->getResult(0)))
          return;
        auto loc = locate(op->getResult(0));
        if (!loc || !loc->bytes ||
            loc->offset > std::numeric_limits<uint64_t>::max() - loc->bytes) {
          invalid = true;
          return;
        }
        auto &ranges = byBase[loc->base];
        auto next = ranges.lower_bound(loc->offset);
        if ((next != ranges.end() &&
             next->first < loc->offset + loc->bytes) ||
            (next != ranges.begin() && [&] {
              auto previous = std::prev(next);
              auto &range = objects[previous->second].location;
              return range.offset + range.bytes > loc->offset;
            }())) {
          invalid = true;
          return;
        }
        ranges[loc->offset] = objects.size();
        objects.push_back({*loc, *identity, {}});
      });
      if (invalid || objects.empty()) {
        ++rejectedObject;
        continue;
      }
      uint64_t clock = 0;
      timeline(model.getBodyBlock(), clock);
      modelName = model.getName().str();
      block(model.getBodyBlock());
    }
    // Provenance is consumed by this pass. Do not make downstream CSE or LLVM
    // lowering depend on diagnostic IDs, including on a rejected model.
    module.walk([](Operation *op) {
      for (auto name : {planAttr, objectAttr, readAttr, versionAttr, epochAttr})
        op->removeAttr(name);
    });
    if (failed(report(reportFile,
                      llvm::json::Object{{"schema", 1}, {"status", "materialized_state_regions"},
                       {"helpers", helperCount}, {"outlined_operations", outlinedOps},
                       {"rematerialized_reads", freshReads},
                       {"rejected_interface", rejectedWide},
                       {"rejected_missing_plan", rejectedMissingPlan},
                       {"rejected_trace_observers", rejectedTrace},
                       {"rejected_unknown_or_overlapping_storage", rejectedObject},
                       {"regions", std::move(regions)}}, getOperation())))
      signalPassFailure();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardStatePartition", "1", [] {
            PassRegistration<StatePartitionPlan>();
            PassRegistration<StatePartitionMaterialize>();
          }};
}
