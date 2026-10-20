// Lower $test$plusargs using the original drivers' native VPI argument ABI.
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
using namespace mlir;
using namespace circt;
namespace {
struct NativePlusArgs : PassWrapper<NativePlusArgs, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativePlusArgs)
  StringRef getArgument() const override { return "chipyard-native-plusargs"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, cf::ControlFlowDialect,
                    func::FuncDialect, LLVM::LLVMDialect>();
  }
  func::FuncOp declaration(OpBuilder &b, StringRef name, FunctionType type) {
    auto *existing = SymbolTable::lookupSymbolIn(getOperation(), name);
    if (existing) {
      auto fn = dyn_cast<func::FuncOp>(existing);
      if (!fn || fn.getFunctionType() != type) {
        existing->emitError("conflicting native plusargs C ABI declaration");
        signalPassFailure();
        return {};
      }
      return fn;
    }
    auto fn = func::FuncOp::create(b, getOperation().getLoc(), name, type);
    fn.setPrivate();
    return fn;
  }
  void runOnOperation() override {
    SmallVector<sim::PlusArgsTestOp> tests;
    getOperation().walk([&](sim::PlusArgsTestOp op) { tests.push_back(op); });
    if (tests.empty())
      return;
    auto top = getOperation();
    OpBuilder b(top.getBody(), top.getBody()->begin());
    auto loc = top.getLoc();
    auto ptr = LLVM::LLVMPointerType::get(&getContext());
    auto i32 = b.getI32Type(), i64 = b.getI64Type();
    auto query =
        declaration(b, "vpi_get_vlog_info", b.getFunctionType({ptr}, {i32}));
    auto compare =
        declaration(b, "strncmp", b.getFunctionType({ptr, ptr, i64}, {i32}));
    if (!query || !compare)
      return;
    auto infoType =
        LLVM::LLVMStructType::getLiteral(&getContext(), {i32, ptr, ptr, ptr});
    circt::Namespace names;
    names.add(top);
    llvm::StringMap<func::FuncOp> helpers;
    for (auto test : tests) {
      auto pattern = test.getFormatString();
      auto &helper = helpers[pattern];
      if (!helper) {
        b.setInsertionPointToStart(top.getBody());
        std::string bytes = "+" + pattern.str();
        bytes.push_back(0);
        auto global = LLVM::GlobalOp::create(
            b, loc, LLVM::LLVMArrayType::get(b.getI8Type(), bytes.size()), true,
            LLVM::Linkage::Private, names.newName("__chipyard_plusarg_prefix"),
            b.getStringAttr(bytes));
        helper = func::FuncOp::create(b, loc,
                                      names.newName("__chipyard_test_plusargs"),
                                      b.getFunctionType({}, {b.getI1Type()}));
        helper.setPrivate();
        auto *entry = helper.addEntryBlock();
        auto *header = b.createBlock(&helper.getBody());
        auto index = header->addArgument(i32, loc);
        auto *body = b.createBlock(&helper.getBody());
        auto *next = b.createBlock(&helper.getBody());
        auto *yes = b.createBlock(&helper.getBody());
        auto *no = b.createBlock(&helper.getBody());
        b.setInsertionPointToStart(entry);
        auto one = arith::ConstantIntOp::create(b, loc, 1, 32);
        auto zero = arith::ConstantIntOp::create(b, loc, 0, 32);
        auto info = LLVM::AllocaOp::create(b, loc, ptr, infoType, one, 0);
        auto ok =
            func::CallOp::create(b, loc, query, ValueRange{info}).getResult(0);
        auto available =
            arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne, ok, zero);
        cf::CondBranchOp::create(b, loc, available, header, ValueRange{one}, no,
                                 ValueRange{});
        b.setInsertionPointToStart(header);
        auto argcAddress = LLVM::GEPOp::create(b, loc, ptr, infoType, info,
                                               ArrayRef<LLVM::GEPArg>{0, 0});
        auto argc = LLVM::LoadOp::create(b, loc, i32, argcAddress);
        auto inRange = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::slt,
                                             index, argc);
        cf::CondBranchOp::create(b, loc, inRange, body, no);
        b.setInsertionPointToStart(body);
        auto argvAddress = LLVM::GEPOp::create(b, loc, ptr, infoType, info,
                                               ArrayRef<LLVM::GEPArg>{0, 1});
        auto argv = LLVM::LoadOp::create(b, loc, ptr, argvAddress);
        auto argAddress = LLVM::GEPOp::create(b, loc, ptr, ptr, argv,
                                              ArrayRef<LLVM::GEPArg>{index});
        auto arg = LLVM::LoadOp::create(b, loc, ptr, argAddress);
        auto prefix = LLVM::AddressOfOp::create(b, loc, global);
        auto length =
            arith::ConstantIntOp::create(b, loc, bytes.size() - 1, 64);
        auto order = func::CallOp::create(b, loc, compare,
                                          ValueRange{arg, prefix, length})
                         .getResult(0);
        auto matches = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq,
                                             order, zero);
        cf::CondBranchOp::create(b, loc, matches, yes, next);
        b.setInsertionPointToStart(next);
        auto increment = arith::AddIOp::create(b, loc, index, one);
        cf::BranchOp::create(b, loc, header, ValueRange{increment});
        b.setInsertionPointToStart(yes);
        auto found = arith::ConstantIntOp::create(b, loc, 1, 1);
        func::ReturnOp::create(b, loc, ValueRange{found});
        b.setInsertionPointToStart(no);
        auto missing = arith::ConstantIntOp::create(b, loc, 0, 1);
        func::ReturnOp::create(b, loc, ValueRange{missing});
      }
      b.setInsertionPoint(test);
      auto result = func::CallOp::create(b, test.getLoc(), helper, ValueRange{})
                        .getResult(0);
      test.replaceAllUsesWith(result);
      test.erase();
    }
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardNativePlusArgs", "1",
          [] { PassRegistration<NativePlusArgs>(); }};
}
