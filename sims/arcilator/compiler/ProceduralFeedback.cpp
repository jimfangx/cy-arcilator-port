// Expose procedural combinational SCCs to the native same-cycle settling loop.
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/RegionUtils.h"
#include "llvm/ADT/SetVector.h"

using namespace mlir;
using namespace circt;
namespace {
struct ProceduralFeedback
    : PassWrapper<ProceduralFeedback, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ProceduralFeedback)
  StringRef getArgument() const override {
    return "chipyard-bind-procedural-feedback";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect>();
  }
  bool procedural(Operation *op) {
    if (auto call = dyn_cast<sim::DPICallOp>(op))
      return !call.getClock();
    if (auto region = dyn_cast<llhd::CombinationalOp>(op)) {
      bool clocked = false;
      region.walk(
          [&](sim::DPICallOp call) { clocked |= bool(call.getClock()); });
      return !clocked;
    }
    return false;
  }
  struct Node {
    Operation *op;
    SmallVector<Value> inputs;
  };
  void runOnOperation() override {
    auto root = getOperation().lookupSymbol<hw::HWModuleOp>("ArcModel");
    if (!root)
      return;
    SmallVector<Node> nodes;
    DenseMap<Operation *, unsigned> ids;
    for (auto &op : root.getBodyBlock()->without_terminator()) {
      auto dialect = op.getName().getDialectNamespace();
      // Sequential processes and register operations are state boundaries.
      // Combinational regions capture their inputs implicitly through their
      // nested operations, and can produce several independently used results.
      bool activeProcedure = procedural(&op);
      bool pure = !op.getNumRegions() && isPure(&op) &&
                  (dialect == "comb" || dialect == "arith" ||
                   (dialect == "hw" && !isa<hw::InstanceOp>(op)));
      if (!op.getNumResults() || (!activeProcedure && !pure))
        continue;
      llvm::SetVector<Value> inputs;
      inputs.insert(op.getOperands().begin(), op.getOperands().end());
      for (auto &region : op.getRegions())
        getUsedValuesDefinedAbove(region, inputs);
      ids[&op] = nodes.size();
      nodes.push_back({&op, {inputs.begin(), inputs.end()}});
    }
    // Iterative Tarjan traversal avoids host stack limits on large designs.
    unsigned unseen = ~0U, next = 0;
    SmallVector<unsigned> index(nodes.size(), unseen), low(nodes.size()), stack;
    SmallVector<bool> active(nodes.size(), false);
    SmallVector<SmallVector<unsigned>> components;
    struct Frame {
      unsigned node, input;
    };
    SmallVector<Frame> work;
    auto enter = [&](unsigned node) {
      index[node] = low[node] = next++;
      active[node] = true;
      stack.push_back(node);
      work.push_back({node, 0});
    };
    for (unsigned start = 0; start < nodes.size(); ++start) {
      if (index[start] != unseen)
        continue;
      enter(start);
      while (!work.empty()) {
        auto &frame = work.back();
        auto n = frame.node;
        if (frame.input < nodes[n].inputs.size()) {
          auto value = nodes[n].inputs[frame.input++];
          auto found = ids.find(value.getDefiningOp());
          if (found == ids.end())
            continue;
          auto input = found->second;
          if (index[input] == unseen)
            enter(input);
          else if (active[input])
            low[n] = std::min(low[n], index[input]);
          continue;
        }
        if (index[n] == low[n]) {
          SmallVector<unsigned> component;
          unsigned item;
          do {
            item = stack.pop_back_val();
            active[item] = false;
            component.push_back(item);
          } while (item != n);
          bool self = llvm::any_of(nodes[n].inputs, [&](Value value) {
            return value.getDefiningOp() == nodes[n].op;
          });
          if ((component.size() > 1 || self) &&
              llvm::any_of(component, [&](unsigned member) {
                return procedural(nodes[member].op);
              }))
            components.push_back(std::move(component));
        }
        work.pop_back();
        if (!work.empty())
          low[work.back().node] = std::min(low[work.back().node], low[n]);
      }
    }
    SmallVector<Attribute> wires;
    if (auto previous =
            getOperation()->getAttrOfType<ArrayAttr>("chipyard.feedback_wires"))
      llvm::append_range(wires, previous);
    unsigned bound = 0;
    for (auto &component : components) {
      DenseSet<unsigned> members(component.begin(), component.end());
      DenseMap<unsigned, unsigned> status;
      llvm::SetVector<Value> edges;
      work.clear();
      for (unsigned start : component) {
        if (status.lookup(start))
          continue;
        status[start] = 1;
        work.push_back({start, 0});
        while (!work.empty()) {
          auto &frame = work.back();
          auto n = frame.node;
          if (frame.input == nodes[n].inputs.size()) {
            status[n] = 2;
            work.pop_back();
            continue;
          }
          auto value = nodes[n].inputs[frame.input++];
          auto found = ids.find(value.getDefiningOp());
          if (found == ids.end() || !members.contains(found->second))
            continue;
          auto input = found->second;
          if (status.lookup(input) == 1)
            edges.insert(value);
          else if (!status.lookup(input)) {
            status[input] = 1;
            work.push_back({input, 0});
          }
        }
      }
      // Keep unsupported types for the normal compiler diagnostic. Integer
      // ports avoid aggregate padding in the native binding's packed ABI.
      if (llvm::any_of(edges, [](Value value) {
            auto bits = hw::getBitWidth(value.getType());
            return bits <= 0 || bits > 65536;
          }))
        continue;
      for (Value value : edges) {
        OpBuilder b(root.getContext());
        auto loc = value.getLoc();
        auto type = b.getIntegerType(hw::getBitWidth(value.getType()));
        unsigned id = wires.size();
        auto [name, input] = root.appendInput("feedback_q_" + Twine(id), type);
        Value replacement = input;
        if (type != value.getType()) {
          b.setInsertionPointToStart(root.getBodyBlock());
          replacement = hw::BitcastOp::create(b, loc, value.getType(), input);
        }
        value.replaceAllUsesWith(replacement);
        b.setInsertionPoint(root.getBodyBlock()->getTerminator());
        Value next = value;
        if (type != value.getType())
          next = hw::BitcastOp::create(b, loc, type, value);
        root.appendOutput("feedback_next_" + Twine(id), next);
        wires.push_back(b.getDictionaryAttr(
            {b.getNamedAttr("model", root.getSymNameAttr()),
             b.getNamedAttr("input", name),
             b.getNamedAttr("output", root.getOutputNameAttr(
                                          root.getNumOutputPorts() - 1)),
             b.getNamedAttr("bits", b.getI64IntegerAttr(type.getWidth()))}));
        ++bound;
      }
    }
    if (bound) {
      getOperation()->setAttr("chipyard.feedback_wires",
                              ArrayAttr::get(&getContext(), wires));
      llvm::errs() << "Bound " << bound
                   << " procedural combinational feedback wires\n";
    }
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardProceduralFeedback", "1",
          [] { PassRegistration<ProceduralFeedback>(); }};
}
