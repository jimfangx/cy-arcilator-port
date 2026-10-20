// Lower only LTL cones with proven single-cycle Boolean semantics. Retain the
// clocked assertion, its enable/edge/label, and every genuinely temporal cone.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LTL/LTLOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"

using namespace mlir;
using namespace circt;

namespace {
struct PointwiseProperties
    : PassWrapper<PointwiseProperties, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PointwiseProperties)
  StringRef getArgument() const override {
    return "chipyard-lower-pointwise-properties";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<comb::CombDialect, hw::HWDialect>();
  }
  DenseMap<Value, Value> converted;
  bool supported(Operation *op) {
    if (isa<ltl::BooleanConstantOp, ltl::NotOp, ltl::ImplicationOp>(op))
      return true;
    if (isa<ltl::AndOp, ltl::OrOp, ltl::IntersectOp, ltl::ConcatOp>(op))
      return op->getNumOperands() != 0;
    if (auto delay = dyn_cast<ltl::DelayOp>(op))
      return delay.getDelay() == 0 && delay.getLength() == 0;
    if (auto repeat = dyn_cast<ltl::RepeatOp>(op))
      return repeat.getBase() == 1 && repeat.getMore() == 0;
    // In particular, an absent upper bound is not a zero delay, and a
    // zero-length sequence is not an ordinary one-cycle Boolean true.
    return false;
  }
  Value resolve(Value root) {
    // Iterative postorder avoids a host recursion limit on deep property DAGs.
    // A pending null entry also prevents cyclic expressions from being folded.
    SmallVector<std::pair<Value, bool>> work{{root, false}};
    while (!work.empty()) {
      auto [value, expanded] = work.pop_back_val();
      if (!expanded) {
        if (converted.count(value))
          continue;
        if (value.getType().isInteger(1)) {
          converted[value] = value;
          continue;
        }
        converted[value] = {};
        auto *op = value.getDefiningOp();
        if (!op || !supported(op))
          continue;
        work.emplace_back(value, true);
        for (auto input : op->getOperands())
          if (!converted.count(input))
            work.emplace_back(input, false);
        continue;
      }
      auto *op = value.getDefiningOp();
      SmallVector<Value> inputs;
      bool complete = true;
      for (auto input : op->getOperands()) {
        auto boolean = converted.lookup(input);
        if (!boolean) {
          complete = false;
          break;
        }
        inputs.push_back(boolean);
      }
      if (!complete)
        continue;
      // Build next to the original producer so cached values dominate all
      // assertion users, including assertions in other nested regions.
      OpBuilder builder(op);
      auto loc = op->getLoc();
      auto invert = [&](Value input) -> Value {
        auto one = hw::ConstantOp::create(builder, loc, APInt(1, 1));
        return comb::XorOp::create(builder, loc, input, one);
      };
      Value result;
      if (auto constant = dyn_cast<ltl::BooleanConstantOp>(op))
        result =
            hw::ConstantOp::create(builder, loc, APInt(1, constant.getValue()));
      else if (isa<ltl::NotOp>(op))
        result = invert(inputs[0]);
      else if (isa<ltl::ImplicationOp>(op))
        result = comb::OrOp::create(builder, loc, invert(inputs[0]), inputs[1]);
      else if (isa<ltl::OrOp>(op))
        result = comb::OrOp::create(builder, loc, inputs, false);
      else if (isa<ltl::AndOp, ltl::IntersectOp, ltl::ConcatOp>(op))
        result = comb::AndOp::create(builder, loc, inputs, false);
      else
        result = inputs[0]; // Exact zero delay or exact single repetition.
      converted[value] = result;
    }
    return converted.lookup(root);
  }
  void runOnOperation() override {
    SmallVector<verif::ClockedAssertOp> assertions;
    getOperation().walk(
        [&](verif::ClockedAssertOp op) { assertions.push_back(op); });
    unsigned count = 0;
    for (auto assertion : assertions) {
      if (assertion.getProperty().getType().isInteger(1))
        continue;
      if (auto boolean = resolve(assertion.getProperty())) {
        assertion.getPropertyMutable().assign(boolean);
        ++count;
      }
    }
    if (count)
      llvm::errs() << "Lowered " << count << " pointwise LTL assertions\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardProperties", "1",
          [] { PassRegistration<PointwiseProperties>(); }};
}
