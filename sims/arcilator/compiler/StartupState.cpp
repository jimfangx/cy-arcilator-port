// Lower proved native startup captures with the matching SDK state pass.
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/StringSet.h"
#include <string>
using namespace mlir;
using namespace circt;
namespace {
struct NativeStartupState
    : PassWrapper<NativeStartupState, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeStartupState)
  NativeStartupState() = default;
  NativeStartupState(const NativeStartupState &other) : PassWrapper(other) {}
  Option<std::string> scopeInputs{*this, "scope-inputs", llvm::cl::init("")};
  Option<std::string> initializedInputs{*this, "initialized-inputs",
                                        llvm::cl::init("")};
  StringRef getArgument() const override {
    return "chipyard-lower-native-startup-state";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, seq::SeqDialect>();
    OpPassManager pipeline(ModuleOp::getOperationName());
    if (succeeded(parsePassPipeline("arc-lower-state", pipeline)))
      pipeline.getDependentDialects(registry);
  }
  void runOnOperation() override {
    llvm::StringSet<> allowed;
    SmallVector<StringRef> names;
    StringRef(scopeInputs).split(names, ':', -1, false);
    StringRef(initializedInputs).split(names, ':', -1, false);
    for (auto name : names)
      allowed.insert(name);
    SmallVector<UnrealizedConversionCastOp> captures;
    for (auto module : getOperation().getOps<hw::HWModuleOp>())
      for (auto cast : module.getOps<UnrealizedConversionCastOp>()) {
        if (!cast->hasAttr("chipyard.native_startup_scope") &&
            !cast->hasAttr("chipyard.native_startup_input"))
          continue;
        bool valid = cast.getNumOperands() == 1 && cast.getNumResults() == 1;
        Value source = valid ? cast.getOperand(0) : Value{};
        auto type =
            valid ? dyn_cast<seq::ImmutableType>(cast.getResult(0).getType())
                  : seq::ImmutableType{};
        valid &= type && type.getInnerType().isInteger(64) && source &&
                 source.getType().isInteger(64);
        auto arg = dyn_cast_or_null<BlockArgument>(source);
        valid &= arg && arg.getOwner() == module.getBodyBlock() &&
                 allowed.contains(module.getArgName(arg.getArgNumber()));
        valid &= llvm::all_of(cast->getUsers(), [](Operation *user) {
          return isa<seq::InitialOp>(user);
        });
        if (!valid) {
          cast.emitError("native startup capture requires a verified "
                         "initialized binding input");
          signalPassFailure();
          return;
        }
        captures.push_back(cast);
      }
    // Erase the native immutable boundary inside this lowering only. The SDK
    // reads module inputs in its Initial phase and maps them to the region's
    // underlying block arguments. Its final Arc IR is verified normally.
    for (auto cast : captures) {
      cast.getResult(0).replaceAllUsesWith(cast.getOperand(0));
      cast.erase();
    }
    OpPassManager pipeline(ModuleOp::getOperationName());
    if (failed(parsePassPipeline("arc-lower-state", pipeline)) ||
        failed(runPipeline(pipeline, getOperation())))
      signalPassFailure();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardNativeStartupState", "1",
          [] { PassRegistration<NativeStartupState>(); }};
}
