// Transport opaque SV chandles as native pointer-width integer storage.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
using namespace mlir;
using namespace circt;
namespace {
struct NativeHandles : PassWrapper<NativeHandles, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeHandles)
  StringRef getArgument() const override { return "chipyard-native-handles"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, hw::HWDialect, comb::CombDialect,
                    llhd::LLHDDialect, sim::SimDialect, func::FuncDialect,
                    LLVM::LLVMDialect>();
  }
  Type storage(Type type) {
    if (isa<LLVM::LLVMPointerType>(type))
      return IntegerType::get(&getContext(), 64);
    if (auto ref = dyn_cast<llhd::RefType>(type))
      return llhd::RefType::get(storage(ref.getNestedType()));
    if (auto array = dyn_cast<hw::ArrayType>(type))
      return hw::ArrayType::get(&getContext(), storage(array.getElementType()),
                                array.getSizeAttr());
    if (auto ports = dyn_cast<hw::ModuleType>(type)) {
      SmallVector<hw::ModulePort> converted(ports.getPorts());
      for (auto &port : converted)
        port.type = storage(port.type);
      return hw::ModuleType::get(&getContext(), converted);
    }
    return type;
  }
  // Type conversion exposes native constants inside aggregate constructors.
  // Materialize the original constant tree before state/memory admission;
  // runtime string creation and other effectful initializers remain untouched.
  void foldNativeArrays(ArrayRef<hw::ArrayCreateOp> constructors) {
    DenseMap<Value, Attribute> values;
    auto value = [&](auto &&self, Value v) -> Attribute {
      if (auto it = values.find(v); it != values.end())
        return it->second;
      Attribute attr;
      if (!matchPattern(v, m_Constant(&attr))) {
        auto array = v.getDefiningOp<hw::ArrayCreateOp>();
        if (!array)
          return {};
        SmallVector<Attribute> fields;
        for (auto element : array.getInputs()) {
          auto field = self(self, element);
          if (!field)
            return {};
          fields.push_back(field);
        }
        attr = ArrayAttr::get(&getContext(), fields);
      }
      values[v] = attr;
      return attr;
    };
    SmallVector<std::pair<hw::ArrayCreateOp, ArrayAttr>> constants;
    for (auto op : constructors)
      if (auto attr = dyn_cast_or_null<ArrayAttr>(value(value, op.getResult())))
        constants.push_back({op, attr});
    for (auto [op, attr] : constants) {
      OpBuilder b(op);
      auto folded =
          hw::AggregateConstantOp::create(b, op.getLoc(), op.getType(), attr);
      op.replaceAllUsesWith(folded.getResult());
      op.erase();
    }
  }
  bool handles(DictionaryAttr info) {
    if (!info)
      return false;
    auto kind = info.getAs<StringAttr>("kind");
    return (kind && kind.getValue() == "chandle") ||
           handles(info.getAs<DictionaryAttr>("element")) ||
           handles(info.getAs<DictionaryAttr>("native_open_actual"));
  }
  bool handles(ArrayAttr abi) {
    return abi && llvm::any_of(abi, [&](Attribute attr) {
             return handles(dyn_cast<DictionaryAttr>(attr));
           });
  }
  DictionaryAttr mark(DictionaryAttr info) {
    NamedAttrList updated(info);
    if (auto kind = info.getAs<StringAttr>("kind");
        kind && kind.getValue() == "chandle")
      updated.set("native_chandle_handle", BoolAttr::get(&getContext(), true));
    for (auto name : {"element", "native_open_actual"})
      if (auto child = info.getAs<DictionaryAttr>(name))
        updated.set(name, mark(child));
    return updated.getDictionary(&getContext());
  }
  ArrayAttr mark(ArrayAttr abi) {
    if (!abi)
      return abi;
    SmallVector<Attribute> updated;
    for (auto attr : abi)
      updated.push_back(mark(cast<DictionaryAttr>(attr)));
    return ArrayAttr::get(&getContext(), updated);
  }
  void runOnOperation() override {
    auto top = getOperation();
    bool present = false;
    top.walk([&](Operation *op) {
      present |= handles(op->getAttrOfType<ArrayAttr>("chipyard.dpi_abi"));
      if (auto info = op->getAttrOfType<DictionaryAttr>("chipyard.dpi_export"))
        present |= handles(info.getAs<ArrayAttr>("abi"));
    });
    if (!present)
      return;
    SmallVector<hw::ArrayCreateOp> constructors;
    bool invalid = false;
    SmallVector<Operation *> zeroes;
    SmallVector<LLVM::ICmpOp> comparisons;
    top.walk([&](Operation *op) {
      bool pointer = llvm::any_of(op->getOperandTypes(),
                                  [&](Type t) { return storage(t) != t; }) ||
                     llvm::any_of(op->getResultTypes(),
                                  [&](Type t) { return storage(t) != t; });
      if (!pointer)
        return;
      if (auto array = dyn_cast<hw::ArrayCreateOp>(op))
        constructors.push_back(array);
      if (isa<LLVM::ZeroOp>(op)) {
        zeroes.push_back(op);
        return;
      }
      if (auto cmp = dyn_cast<LLVM::ICmpOp>(op)) {
        if (cmp.getPredicate() == LLVM::ICmpPredicate::eq ||
            cmp.getPredicate() == LLVM::ICmpPredicate::ne) {
          comparisons.push_back(cmp);
          return;
        }
      }
      if (isa<sim::DPICallOp, func::CallOp, func::ReturnOp, llhd::SignalOp,
              llhd::ProbeOp, llhd::DriveOp, llhd::ReturnOp, llhd::HaltOp,
              llhd::WaitOp, llhd::ProcessOp, comb::MuxOp, arith::SelectOp,
              hw::ArrayCreateOp, hw::ArrayGetOp, hw::ArraySliceOp,
              hw::ArrayConcatOp, llhd::SigArrayGetOp, llhd::SigArraySliceOp,
              hw::BitcastOp, hw::InstanceOp, hw::OutputOp>(op) ||
          isa<BranchOpInterface>(op))
        return;
      op->emitError("unsupported native opaque handle operation");
      invalid = true;
    });
    if (invalid) {
      signalPassFailure();
      return;
    }
    top.walk([&](Operation *op) {
      for (auto &region : op->getRegions())
        for (auto &block : region)
          for (auto arg : block.getArguments())
            arg.setType(storage(arg.getType()));
      for (auto result : op->getResults())
        result.setType(storage(result.getType()));
      if (auto type = op->getAttrOfType<TypeAttr>("module_type"))
        op->setAttr("module_type", TypeAttr::get(storage(type.getValue())));
      if (isa<func::FuncOp, llhd::CoroutineOp>(op)) {
        auto fn = cast<FunctionOpInterface>(op);
        auto type = cast<FunctionType>(fn.getFunctionType());
        SmallVector<Type> inputs, outputs;
        for (auto t : type.getInputs())
          inputs.push_back(storage(t));
        for (auto t : type.getResults())
          outputs.push_back(storage(t));
        fn.setType(FunctionType::get(&getContext(), inputs, outputs));
      }
      if (auto function = dyn_cast<sim::DPIFuncOp>(op)) {
        SmallVector<sim::DPIArgument> formals;
        for (auto formal : function.getDpiFunctionType().getArguments()) {
          formal.type = storage(formal.type);
          formals.push_back(formal);
        }
        function.setDpiFunctionTypeAttr(
            TypeAttr::get(sim::DPIFunctionType::get(&getContext(), formals)));
        if (auto abi = function->getAttrOfType<ArrayAttr>("chipyard.dpi_abi"))
          function->setAttr("chipyard.dpi_abi", mark(abi));
      }
      if (auto info =
              op->getAttrOfType<DictionaryAttr>("chipyard.dpi_export")) {
        NamedAttrList updated(info);
        updated.set("abi", mark(info.getAs<ArrayAttr>("abi")));
        if (auto captures = info.getAs<ArrayAttr>("captures")) {
          auto fn = cast<FunctionOpInterface>(op);
          auto signature = cast<FunctionType>(fn.getFunctionType());
          SmallVector<Attribute> converted;
          for (auto attr : captures) {
            auto capture = cast<DictionaryAttr>(attr);
            auto index = capture.getAs<IntegerAttr>("argument").getInt();
            auto ref = cast<llhd::RefType>(signature.getInput(index));
            NamedAttrList fields(capture);
            fields.set("width",
                       IntegerAttr::get(IntegerType::get(&getContext(), 64),
                                        hw::getBitWidth(ref.getNestedType())));
            converted.push_back(fields.getDictionary(&getContext()));
          }
          updated.set("captures", ArrayAttr::get(&getContext(), converted));
        }
        op->setAttr("chipyard.dpi_export",
                    updated.getDictionary(&getContext()));
      }
    });
    for (auto *zero : zeroes) {
      OpBuilder b(zero);
      auto value = hw::ConstantOp::create(b, zero->getLoc(), APInt(64, 0));
      zero->getResult(0).replaceAllUsesWith(value);
      zero->erase();
    }
    for (auto cmp : comparisons) {
      OpBuilder b(cmp);
      auto predicate = cmp.getPredicate() == LLVM::ICmpPredicate::eq
                           ? arith::CmpIPredicate::eq
                           : arith::CmpIPredicate::ne;
      auto value = arith::CmpIOp::create(b, cmp.getLoc(), predicate,
                                         cmp.getLhs(), cmp.getRhs());
      cmp.getResult().replaceAllUsesWith(value);
      cmp.erase();
    }
    foldNativeArrays(constructors);
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardNativeHandles", LLVM_VERSION_STRING,
          []() { PassRegistration<NativeHandles>(); }};
}
