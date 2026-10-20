// Preserve SV real-to-integer rounding through the SDK Arc input dialects.
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;
namespace {
struct LowerRealRounding : PassWrapper<LowerRealRounding, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerRealRounding)
  StringRef getArgument() const final { return "chipyard-lower-real-rounding"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<math::MathDialect, LLVM::LLVMDialect>();
  }
  void runOnOperation() override {
    unsigned count = 0;
    getOperation().walk([&](math::RoundOp op) {
      OpBuilder builder(op);
      auto replacement = LLVM::RoundOp::create(
          builder, op.getLoc(), op.getType(), op.getOperand());
      op.replaceAllUsesWith(replacement.getResult());
      op.erase();
      ++count;
    });
    if (count)
      llvm::errs() << "Lowered " << count
                   << " real rounding operations to LLVM intrinsics\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardRealRounding", "1", [] {
            PassRegistration<LowerRealRounding>();
          }};
}
