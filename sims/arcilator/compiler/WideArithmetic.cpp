// Keep proven-pure wide arithmetic visible to Arc optimization.
#include "WideArithmetic.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include <optional>
using namespace mlir;
using namespace circt;
namespace {
using chipyard_arc_compiler::ownsWideShift;
using chipyard_arc_compiler::wideShiftMode;
struct RestoreWide : PassWrapper<RestoreWide, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RestoreWide)
  StringRef getArgument() const override {
    return "chipyard-restore-wide-arithmetic";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<comb::CombDialect>();
  }
  void runOnOperation() override {
    SymbolTableCollection symbols;
    SmallVector<std::pair<sim::DPICallOp, unsigned>> calls;
    getOperation().walk([&](sim::DPICallOp call) {
      if (auto m = wideShiftMode(call, symbols))
        calls.emplace_back(call, *m);
    });
    for (auto item : calls) {
      auto call = item.first;
      OpBuilder builder(call);
      Value left = call.getInputs()[0], right = call.getInputs()[1], result;
      if (item.second == 0)
        result = comb::ShlOp::create(builder, call.getLoc(), left, right);
      else if (item.second == 1)
        result = comb::ShrUOp::create(builder, call.getLoc(), left, right);
      else
        result = comb::ShrSOp::create(builder, call.getLoc(), left, right);
      call.getResult(0).replaceAllUsesWith(result);
      call.erase();
    }
    getOperation()->setAttr("chipyard.restored_wide_arithmetic",
        IntegerAttr::get(IntegerType::get(&getContext(), 64), calls.size()));
  }
};
struct LowerWide : PassWrapper<LowerWide, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerWide)
  StringRef getArgument() const override {
    return "chipyard-lower-wide-arithmetic";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<func::FuncDialect>();
  }
  void runOnOperation() override {
    SmallVector<std::pair<Operation *, unsigned>> shifts;
    getOperation().walk([&](Operation *op) {
      if (op->getNumResults() != 1 || op->getNumOperands() != 2)
        return;
      auto type = dyn_cast<IntegerType>(op->getResult(0).getType());
      if (!type || type.getWidth() <= 1024 ||
          op->getOperand(0).getType() != type ||
          op->getOperand(1).getType() != type)
        return;
      if (isa<comb::ShlOp>(op)) shifts.emplace_back(op, 0);
      else if (isa<comb::ShrUOp>(op)) shifts.emplace_back(op, 1);
      else if (isa<comb::ShrSOp>(op)) shifts.emplace_back(op, 2);
    });
    unsigned lowered = 0;
    for (auto item : shifts) {
      auto *op = item.first;
      auto type = cast<IntegerType>(op->getResult(0).getType());
      std::string name = "chipyard_arc_wide_shift_" +
          std::to_string(item.second) + "_" + std::to_string(type.getWidth());
      auto symbol = SymbolTable::lookupSymbolIn(getOperation(), name);
      auto callee = dyn_cast_or_null<func::FuncOp>(symbol);
      if (symbol && !ownsWideShift(callee, type, item.second))
        continue; // A familiar name alone never redirects ordinary foreign code.
      if (!callee) {
        OpBuilder declBuilder(&getContext());
        declBuilder.setInsertionPointToEnd(getOperation().getBody());
        callee = func::FuncOp::create(declBuilder, op->getLoc(), name,
            declBuilder.getFunctionType({type, type}, {type}));
        callee.setPrivate();
        callee->setAttr("chipyard.native_pure_wide_shift",
                        declBuilder.getUnitAttr());
        callee->setAttr("chipyard.no_output_readback",
                        declBuilder.getUnitAttr());
      }
      OpBuilder builder(op);
      auto call = func::CallOp::create(builder, op->getLoc(), callee,
                                       op->getOperands());
      op->getResult(0).replaceAllUsesWith(call.getResult(0));
      op->erase();
      ++lowered;
    }
    getOperation()->setAttr("chipyard.lowered_wide_arithmetic",
        IntegerAttr::get(IntegerType::get(&getContext(), 64), lowered));
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardWideArithmetic", "1", [] {
    PassRegistration<RestoreWide>();
    PassRegistration<LowerWide>();
  }};
}
