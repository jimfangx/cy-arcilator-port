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
#include <set>
#include <tuple>

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
      model->setAttr(planAttr, builder.getI64IntegerAttr(2));
    }
    if (failed(report(reportFile,
                      llvm::json::Object{{"schema", 2},
                       {"algorithm", "state-dependence-graph"},
                       {"status", "planned_explicit_state"},
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
  // Internal diagnostic switch for independent legacy reload-guard tests.
  // The public model/Make option always enables graph scheduling.
  Option<bool> graphSchedule{*this, "graph-schedule", llvm::cl::Hidden,
                             llvm::cl::init(true)};
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
  uint64_t graphEpochs = 0, graphNodes = 0, graphBarriers = 0;
  uint64_t graphSSA = 0, graphRAW = 0, graphWAR = 0, graphWAW = 0;
  uint64_t graphMoved = 0, graphCycles = 0, graphRejectedCost = 0;
  uint64_t graphInterfaceBefore = 0, graphInterfaceAfter = 0;
  uint64_t graphFrontierBefore = 0, graphFrontierAfter = 0;
  uint64_t graphEarlyConsumers = 0;
  uint64_t prefixCandidates = 0, admittedPrefixes = 0, selectedPrefixes = 0;
  uint64_t noPrefixWindows = 0, rejectedPrefixMaterialization = 0;
  uint64_t maximumSelectedWork = 0;
  DenseMap<Operation *, uint64_t> originalPositions;
  SmallPtrSet<Operation *, 32> rejectedGraphOperations;
  llvm::json::Array graphReports;
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
    // Only known disjoint scalar objects participate in graph memory edges.
    // Trace observers have already caused whole-model fallback above.
    if (auto write = dyn_cast<arc::StateWriteOp>(op))
      return scalarState(write.getState()) && bool(object(write.getState()));
    return pure.pure(op);
  }
  bool graphCheap(Operation *op) {
    return constant(op) || isa<arc::StorageGetOp>(op);
  }
  bool graphOperation(Operation *op) {
    return graphCheap(op) || candidate(op);
  }
  uint64_t transport(Value value) {
    if (auto *definition = value.getDefiningOp())
      if (constant(definition) || address(definition))
        return 0; // Cheap constant/address rematerialization, not a dependency cut.
    return words(value.getType());
  }
  struct GraphNode {
    Operation *op;
    SmallVector<unsigned> successors;
    unsigned indegree = 0;
  };
  enum Edge : unsigned { SSA = 1, RAW = 2, WAR = 4, WAW = 8 };
  struct PrefixChoice {
    unsigned count = 0, candidates = 0, admitted = 0;
    uint64_t work = 0, input = 0, output = 0, charge = 0;
  };
  // One forward scan per bounded window. Definitions have one owner; only
  // constants, allocated addresses and exactly version-proven reads may be
  // rematerialized. Inputs are monotonic, and a producer's output cost retires
  // as its last use enters the prefix. No cut rescans the same long suffix.
  PrefixChoice choosePrefix(ArrayRef<Operation *> window, bool exact) {
    PrefixChoice best;
    if (window.empty())
      return best;
    Operation *entry = window.front();
    SmallPtrSet<Operation *, 32> members;
    llvm::SetVector<Value> inputs;
    DenseMap<Value, SmallVector<Value>> memo;
    DenseMap<Value, uint64_t> liveUses;
    uint64_t input = 0, output = 0, work = 0, writes = 0;
    bool implicitStorage = false;
    std::function<SmallVector<Value>(Value)> roots = [&](Value value) {
      Operation *definition = value.getDefiningOp();
      if (definition && members.contains(definition))
        return SmallVector<Value>{};
      if (auto found = memo.find(value); found != memo.end())
        return found->second;
      SmallVector<Value> result;
      if (!exact) {
        if (transport(value))
          result.push_back(value);
      } else if (definition &&
                 (constant(definition) || rematerializeAddress(definition) ||
                  (isa<arc::StateReadOp>(definition) &&
                   reload(cast<arc::StateReadOp>(definition), entry)))) {
        llvm::SetVector<Value> unique;
        for (Value operand : definition->getOperands())
          for (Value root : roots(operand))
            unique.insert(root);
        result.append(unique.begin(), unique.end());
      } else
        result.push_back(value);
      memo[value] = result;
      return result;
    };
    for (unsigned i = 0; i < window.size(); ++i) {
      Operation *op = window[i];
      members.insert(op);
      work += pure.work(op);
      writes += isa<arc::StateWriteOp>(op);
      if (!exact && !implicitStorage &&
          isa<arc::StateReadOp, arc::StateWriteOp>(op)) {
        ++input;
        implicitStorage = true;
      }
      for (Value operand : op->getOperands()) {
        auto live = liveUses.find(operand);
        if (live != liveUses.end() && --live->second == 0)
          output -= words(operand.getType());
        if (!operand.getDefiningOp() ||
            !members.contains(operand.getDefiningOp()))
          for (Value root : roots(operand))
            if (inputs.insert(root))
              input += words(root.getType());
      }
      for (Value value : op->getResults()) {
        uint64_t uses = std::distance(value.use_begin(), value.use_end());
        liveUses[value] = uses;
        if (uses)
          output += words(value.getType());
      }
      unsigned count = i + 1;
      if (count < minOps)
        continue;
      ++best.candidates;
      if (input > maxWords || output > maxWords || (!output && !writes))
        continue;
      ++best.admitted;
      uint64_t charge = 2 + input + output; // Call/return plus transferred words.
      unsigned __int128 left = (unsigned __int128)charge * best.work;
      unsigned __int128 right = (unsigned __int128)best.charge * work;
      bool better = !best.count || left < right ||
                    (left == right &&
                     (output < best.output ||
                      (output == best.output && work > best.work)));
      if (better) {
        best.count = count;
        best.work = work;
        best.input = input;
        best.output = output;
        best.charge = charge;
      }
    }
    return best;
  }
  struct Cost {
    uint64_t interface = 0, peak = 0, area = 0, maximumWork = 0;
    unsigned feasible = 0, groups = 0;
  };
  DenseMap<Value, uint64_t>
  remainingUses(ArrayRef<GraphNode> nodes,
                const DenseMap<Operation *, unsigned> &indices) {
    DenseMap<Value, uint64_t> remaining;
    for (auto &node : nodes) {
      for (Value operand : node.op->getOperands())
        if (transport(operand))
          ++remaining[operand];
      for (Value value : node.op->getResults())
        if (transport(value) &&
            llvm::any_of(value.getUsers(), [&](Operation *user) {
              return !indices.count(user);
            }))
          ++remaining[value]; // One retained live-out witness, not an SSA use.
    }
    return remaining;
  }
  Cost cost(ArrayRef<GraphNode> nodes, ArrayRef<unsigned> order,
            const DenseMap<Operation *, unsigned> &indices) {
    Cost result;
    SmallVector<unsigned> group(nodes.size(), 0);
    SmallVector<Operation *> window;
    unsigned current = 0;
    auto assign = [&] {
      if (window.empty())
        return;
      PrefixChoice choice = choosePrefix(window, false);
      for (unsigned i = 0; i < window.size(); ++i) {
        if (choice.count && i == choice.count)
          ++current;
        group[indices.lookup(window[i])] = current;
      }
      ++current;
      window.clear();
    };
    for (unsigned index : order) {
      if (graphCheap(nodes[index].op))
        continue;
      window.push_back(nodes[index].op);
      if (window.size() == maxOps)
        assign();
    }
    assign();
    result.groups = std::max(1u, current);
    SmallVector<llvm::SetVector<Value>> inputs(result.groups), outputs(result.groups);
    SmallVector<uint64_t> work(result.groups, 0), counts(result.groups, 0);
    SmallVector<bool> storage(result.groups, false), writes(result.groups, false);
    for (unsigned i = 0; i < nodes.size(); ++i) {
      Operation *op = nodes[i].op;
      unsigned owner = group[i];
      if (graphCheap(op))
        continue;
      ++counts[owner];
      work[owner] += pure.work(op);
      storage[owner] = storage[owner] || isa<arc::StateReadOp, arc::StateWriteOp>(op);
      writes[owner] = writes[owner] || isa<arc::StateWriteOp>(op);
      for (Value operand : op->getOperands()) {
        if (!transport(operand))
          continue;
        auto producer = indices.find(operand.getDefiningOp());
        if (producer == indices.end() || group[producer->second] != owner) {
          inputs[owner].insert(operand);
          if (producer != indices.end())
            outputs[group[producer->second]].insert(operand);
        }
      }
      for (Value value : op->getResults())
        if (transport(value) &&
            llvm::any_of(value.getUsers(), [&](Operation *user) {
              return !indices.count(user);
            }))
          outputs[owner].insert(value);
    }
    for (unsigned i = 0; i < result.groups; ++i) {
      uint64_t in = storage[i] ? 1 : 0, out = 0;
      for (Value value : inputs[i])
        in += transport(value);
      for (Value value : outputs[i])
        out += transport(value);
      result.interface += in + out;
      result.maximumWork = std::max(result.maximumWork, work[i]);
      result.feasible += counts[i] >= minOps && in <= maxWords &&
                         out <= maxWords && (out || writes[i]);
    }
    auto remaining = remainingUses(nodes, indices);
    llvm::SmallDenseSet<Value> external;
    uint64_t live = 0;
    for (auto &node : nodes)
      for (Value operand : node.op->getOperands())
        if (transport(operand) && !indices.count(operand.getDefiningOp()) &&
            external.insert(operand).second)
          live += transport(operand);
    result.peak = live;
    for (unsigned i : order) {
      Operation *op = nodes[i].op;
      for (Value value : op->getResults())
        if (remaining.lookup(value))
          live += transport(value);
      result.peak = std::max(result.peak, live);
      for (Value operand : op->getOperands())
        if (transport(operand) && --remaining[operand] == 0)
          live -= transport(operand);
      if (!graphCheap(op))
        result.area += live;
    }
    return result;
  }
  void scheduleEpoch(SmallVector<Operation *> &ops) {
    if (ops.empty())
      return;
    ++graphEpochs;
    graphNodes += ops.size();
    SmallVector<GraphNode> nodes;
    DenseMap<Operation *, unsigned> indices;
    for (Operation *op : ops) {
      indices[op] = nodes.size();
      nodes.push_back({op, {}, 0});
    }
    DenseMap<uint64_t, unsigned> kinds;
    auto edge = [&](unsigned from, unsigned to, unsigned kind) {
      if (from == to)
        return;
      uint64_t key = (uint64_t(from) << 32) | to;
      unsigned &mask = kinds[key];
      if (mask & kind)
        return;
      if (!mask) {
        nodes[from].successors.push_back(to);
        ++nodes[to].indegree;
      }
      mask |= kind;
      switch (kind) {
      case SSA: ++graphSSA; break;
      case RAW: ++graphRAW; break;
      case WAR: ++graphWAR; break;
      case WAW: ++graphWAW; break;
      }
    };
    DenseMap<unsigned, unsigned> lastWrite;
    DenseMap<unsigned, SmallVector<unsigned>> pendingReads;
    DenseMap<unsigned, SmallVector<unsigned>> overwrittenReads;
    for (unsigned i = 0; i < nodes.size(); ++i) {
      Operation *op = nodes[i].op;
      for (Value operand : op->getOperands()) {
        auto source = indices.find(operand.getDefiningOp());
        if (source != indices.end())
          edge(source->second, i, SSA);
      }
      if (auto read = dyn_cast<arc::StateReadOp>(op)) {
        unsigned identity = *object(read.getState());
        auto write = lastWrite.find(identity);
        if (write != lastWrite.end())
          edge(write->second, i, RAW); // Mutation witness: actual RAW dependency.
        pendingReads[identity].push_back(i);
      } else if (auto write = dyn_cast<arc::StateWriteOp>(op)) {
        unsigned identity = *object(write.getState());
        auto previous = lastWrite.find(identity);
        if (previous != lastWrite.end())
          edge(previous->second, i, WAW); // Actual ordered writes of one object.
        for (unsigned read : pendingReads[identity]) {
          edge(read, i, WAR); // Mutation witness: preserve the read's old version.
          overwrittenReads[i].push_back(read);
        }
        pendingReads[identity].clear();
        lastWrite[identity] = i;
      }
    }
    auto remaining = remainingUses(nodes, indices);
    std::set<unsigned> ready;
    for (unsigned i = 0; i < nodes.size(); ++i)
      if (!nodes[i].indegree)
        ready.insert(i);
    SmallVector<unsigned> schedule;
    auto score = [&](unsigned index) {
      Operation *op = nodes[index].op;
      uint64_t oldWords = 0;
      for (unsigned read : overwrittenReads[index]) {
        Value value = nodes[read].op->getResult(0);
        uint64_t consumed = llvm::count(op->getOperands(), value);
        if (remaining.lookup(value) > consumed)
          oldWords += transport(value);
      }
      int64_t delta = 0;
      for (Value value : op->getResults())
        if (remaining.lookup(value))
          delta += transport(value);
      llvm::SmallDenseSet<Value> operands;
      for (Value operand : op->getOperands())
        if (transport(operand) && operands.insert(operand).second &&
            remaining.lookup(operand) == uint64_t(llvm::count(op->getOperands(), operand)))
          delta -= transport(operand);
      bool connected = !schedule.empty() && llvm::any_of(
          op->getOperands(), [&](Value value) {
            return value.getDefiningOp() == nodes[schedule.back()].op;
          });
      return std::make_tuple(oldWords, delta, !connected, index);
    };
    while (!ready.empty()) {
      // Bounded candidate lookahead keeps compile work linear in epoch size.
      // Add immediate consumers so a long independent chain is not separated
      // simply because its next node has a distant original source ordinal.
      SmallVector<unsigned> choices;
      for (unsigned index : ready) {
        choices.push_back(index);
        if (choices.size() == 32)
          break;
      }
      if (!schedule.empty()) {
        unsigned extra = 0;
        for (unsigned index : nodes[schedule.back()].successors)
          if (ready.count(index)) {
            choices.push_back(index);
            if (++extra == 32)
              break;
          }
      }
      unsigned selected = choices.front();
      for (unsigned index : choices)
        if (score(index) < score(selected))
          selected = index;
      ready.erase(selected);
      schedule.push_back(selected);
      for (Value operand : nodes[selected].op->getOperands())
        if (transport(operand))
          --remaining[operand];
      for (unsigned successor : nodes[selected].successors)
        if (--nodes[successor].indegree == 0)
          ready.insert(successor);
    }
    if (schedule.size() != nodes.size()) {
      ++graphCycles;
      rejectedGraphOperations.insert(ops.begin(), ops.end());
      graphReports.push_back(llvm::json::Object{
          {"nodes", nodes.size()}, {"decision", "unsplit_cycle_fallback"}});
      ops.clear();
      return;
    }
    SmallVector<unsigned> original;
    for (unsigned i = 0; i < nodes.size(); ++i)
      original.push_back(i);
    Cost before = cost(nodes, original, indices);
    Cost after = cost(nodes, schedule, indices);
    uint64_t moved = 0, consumers = 0;
    SmallVector<unsigned> newPosition(nodes.size());
    for (unsigned i = 0; i < schedule.size(); ++i) {
      newPosition[schedule[i]] = i;
      moved += schedule[i] != i && !graphCheap(nodes[schedule[i]].op);
    }
    bool improved = after.interface < before.interface ||
                    after.peak < before.peak || after.area < before.area ||
                    after.feasible > before.feasible;
    // Reordering without any admissible region is not a partitioning benefit.
    // In particular a rejected narrow interface must retain the original IR.
    bool accept = moved && improved && after.feasible != 0 &&
                  after.feasible >= before.feasible &&
                  after.interface <= before.interface && after.peak <= before.peak &&
                  after.maximumWork <= before.maximumWork;
    graphInterfaceBefore += before.interface;
    graphInterfaceAfter += accept ? after.interface : before.interface;
    graphFrontierBefore += before.peak;
    graphFrontierAfter += accept ? after.peak : before.peak;
    if (accept) {
      for (auto [write, reads] : overwrittenReads)
        for (unsigned read : reads)
          for (unsigned user : nodes[read].successors)
            if (user > write && newPosition[user] < newPosition[write] &&
                pure.pure(nodes[user].op))
              ++consumers;
      // No executable operation crosses this epoch's boundary. Each node is
      // moved once, and shared expensive producers keep one original owner.
      Block *block = ops.front()->getBlock();
      Operation *afterEpoch = ops.back()->getNextNode();
      for (unsigned index : schedule)
        if (afterEpoch)
          nodes[index].op->moveBefore(afterEpoch);
        else
          nodes[index].op->moveBefore(block, block->end());
      graphMoved += moved;
      graphEarlyConsumers += consumers;
    } else if (moved)
      ++graphRejectedCost;
    graphReports.push_back(llvm::json::Object{
        {"nodes", nodes.size()}, {"decision", accept ? "scheduled" : "original_order"},
        {"reordered_operations", accept ? moved : 0},
        {"interface_words_before", before.interface},
        {"interface_words_after", accept ? after.interface : before.interface},
        {"frontier_words_before", before.peak},
        {"frontier_words_after", accept ? after.peak : before.peak},
        {"frontier_area_before", before.area},
        {"frontier_area_after", accept ? after.area : before.area},
        {"maximum_partition_work_before", before.maximumWork},
        {"maximum_partition_work_after", accept ? after.maximumWork : before.maximumWork},
        {"feasible_regions_before", before.feasible},
        {"feasible_regions_after", accept ? after.feasible : before.feasible},
        {"old_consumers_scheduled_before_overwrite", consumers}});
    ops.clear();
  }
  void scheduleBlock(Block &body) {
    for (auto conditional : body.getOps<scf::IfOp>())
      for (auto &region : conditional->getRegions())
        for (auto &nested : region)
          scheduleBlock(nested);
    // Capture epochs before moving their operations, preserving barriers.
    SmallVector<SmallVector<Operation *>> epochs;
    SmallVector<Operation *> current;
    for (auto &op : body) {
      if (!allocation(&op) && graphOperation(&op))
        current.push_back(&op);
      else {
        if (!current.empty())
          epochs.push_back(std::move(current));
        current.clear();
        ++graphBarriers;
      }
    }
    if (!current.empty())
      epochs.push_back(std::move(current));
    for (auto &epoch : epochs)
      scheduleEpoch(epoch);
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
    llvm::json::Array originalOrdinals;
    for (Operation *op : ops)
      originalOrdinals.push_back(originalPositions.lookup(op));
    regions.back().getAsObject()->insert(
        {"original_operation_ordinals", std::move(originalOrdinals)});
    ++helperCount;
    outlinedOps += ops.size();
    freshReads += clonedReads;
    ops.clear();
  }
  void costedWindow(SmallVector<Operation *> &ops) {
    if (ops.empty())
      return;
    PrefixChoice choice = choosePrefix(ops, true);
    prefixCandidates += choice.candidates;
    admittedPrefixes += choice.admitted;
    if (!choice.count) {
      ++noPrefixWindows;
      ops.clear();
      return;
    }
    unsigned windowSize = ops.size();
    SmallVector<Operation *> prefix(ops.begin(), ops.begin() + choice.count);
    SmallVector<Operation *> tail(ops.begin() + choice.count, ops.end());
    uint64_t before = helperCount;
    outline(prefix);
    if (helperCount == before) {
      ++rejectedPrefixMaterialization;
      ops.clear();
      return;
    }
    ++selectedPrefixes;
    maximumSelectedWork = std::max(maximumSelectedWork, choice.work);
    auto &region = *regions.back().getAsObject();
    region["selection"] = "costed-prefix";
    region["candidate_window_operations"] = windowSize;
    region["selected_prefix_operations"] = choice.count;
    region["selected_work"] = choice.work;
    region["selected_input_words"] = choice.input;
    region["selected_output_words"] = choice.output;
    region["call_and_transfer_charge"] = choice.charge;
    // The unselected suffix is considered once as a bounded tail. Never
    // repeatedly rescan a large suffix when a low-frontier prefix is short.
    if (!tail.empty()) {
      uint64_t tailWork = 0;
      unsigned tailSize = tail.size();
      for (Operation *op : tail)
        tailWork += pure.work(op);
      before = helperCount;
      outline(tail);
      if (helperCount != before) {
        maximumSelectedWork = std::max(maximumSelectedWork, tailWork);
        auto &suffix = *regions.back().getAsObject();
        suffix["selection"] = "costed-tail";
        suffix["candidate_window_operations"] = windowSize;
        suffix["selected_prefix_operations"] = tailSize;
        suffix["selected_work"] = tailWork;
      }
    }
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
    auto flush = [&] {
      if (graphSchedule)
        costedWindow(ops);
      else
        outline(ops);
    };
    for (auto &op : llvm::make_early_inc_range(body)) {
      if (rejectedGraphOperations.contains(&op)) {
        flush();
        continue;
      }
      if (constant(&op) || address(&op))
        continue;
      if (!candidate(&op)) {
        flush();
        continue;
      }
      if (ops.size() >= maxOps)
        flush();
      ops.push_back(&op);
    }
    flush();
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
      if (!plan || *plan != 2) {
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
      for (auto [op, position] : positions)
        originalPositions[op] = position;
      if (graphSchedule)
        scheduleBlock(model.getBodyBlock());
      // Scheduling changes the memory-event positions. Reconstruct both the
      // typed write timeline and operation dominance before any reload proof;
      // original numerical positions are never used to justify a new load.
      positions.clear();
      barriers.clear();
      for (auto &object : objects)
        object.writes.clear();
      clock = 0;
      timeline(model.getBodyBlock(), clock);
      dominance = std::make_unique<DominanceInfo>(module);
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
                      llvm::json::Object{{"schema", 2},
                       {"algorithm", "state-dependence-graph"},
                       {"graph_scheduling_enabled", bool(graphSchedule)},
                       {"partition_selection", graphSchedule
                           ? "costed-low-frontier" : "legacy-diagnostic"},
                       {"partition_prefix_candidates", prefixCandidates},
                       {"partition_admissible_candidates", admittedPrefixes},
                       {"partition_selected_prefixes", selectedPrefixes},
                       {"partition_no_admissible_windows", noPrefixWindows},
                       {"partition_materialization_rejections", rejectedPrefixMaterialization},
                       {"partition_maximum_selected_work", maximumSelectedWork},
                       {"status", "materialized_state_regions"},
                       {"helpers", helperCount}, {"outlined_operations", outlinedOps},
                       {"rematerialized_reads", freshReads},
                       {"rejected_interface", rejectedWide},
                       {"rejected_missing_plan", rejectedMissingPlan},
                       {"rejected_trace_observers", rejectedTrace},
                       {"rejected_unknown_or_overlapping_storage", rejectedObject},
                       {"graph_epochs", graphEpochs}, {"graph_nodes", graphNodes},
                       {"graph_barrier_segments", graphBarriers},
                       {"graph_edges_ssa", graphSSA}, {"graph_edges_raw", graphRAW},
                       {"graph_edges_war", graphWAR}, {"graph_edges_waw", graphWAW},
                       {"graph_reordered_operations", graphMoved},
                       {"graph_fallback_cycles", graphCycles},
                       {"graph_rejected_cost", graphRejectedCost},
                       {"graph_interface_words_before", graphInterfaceBefore},
                       {"graph_interface_words_after", graphInterfaceAfter},
                       {"graph_frontier_words_before", graphFrontierBefore},
                       {"graph_frontier_words_after", graphFrontierAfter},
                       {"graph_old_consumers_before_overwrite", graphEarlyConsumers},
                       {"graph_regions", std::move(graphReports)},
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
