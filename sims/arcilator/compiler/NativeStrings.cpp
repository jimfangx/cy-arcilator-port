// Lower SV dynamic strings to runtime-owned immutable native string handles.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
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
struct NativeStrings : PassWrapper<NativeStrings, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeStrings)
  StringRef getArgument() const override { return "chipyard-native-strings"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, comb::CombDialect, llhd::LLHDDialect,
                    sim::SimDialect, func::FuncDialect, LLVM::LLVMDialect>();
  }
  circt::Namespace names;
  llvm::StringMap<sim::DPIFuncOp> helpers;
  Type storage(Type type) {
    if (isa<sim::DynamicStringType>(type))
      return IntegerType::get(&getContext(), 64);
    if (auto ref = dyn_cast<llhd::RefType>(type))
      return llhd::RefType::get(storage(ref.getNestedType()));
    if (auto array = dyn_cast<hw::ArrayType>(type))
      return hw::ArrayType::get(&getContext(), storage(array.getElementType()),
                                array.getSizeAttr());
    if (auto structure = dyn_cast<hw::StructType>(type)) {
      SmallVector<hw::StructType::FieldInfo> fields;
      for (auto field : structure.getElements())
        fields.push_back({field.name, storage(field.type)});
      return hw::StructType::get(&getContext(), fields);
    }
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
  void foldNativeAggregates(ArrayRef<Operation *> constructors) {
    DenseMap<Value, Attribute> values;
    auto value = [&](auto &&self, Value v) -> Attribute {
      if (auto it = values.find(v); it != values.end())
        return it->second;
      Attribute attr;
      if (!matchPattern(v, m_Constant(&attr))) {
        auto *aggregate = v.getDefiningOp();
        if (!isa_and_nonnull<hw::ArrayCreateOp, hw::StructCreateOp>(aggregate))
          return {};
        SmallVector<Attribute> fields;
        for (auto element : aggregate->getOperands()) {
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
    SmallVector<std::pair<Operation *, ArrayAttr>> constants;
    for (auto op : constructors)
      if (auto attr =
              dyn_cast_or_null<ArrayAttr>(value(value, op->getResult(0))))
        constants.push_back({op, attr});
    for (auto [op, attr] : constants) {
      OpBuilder b(op);
      auto folded = hw::AggregateConstantOp::create(
          b, op->getLoc(), op->getResult(0).getType(), attr);
      op->getResult(0).replaceAllUsesWith(folded.getResult());
      op->erase();
    }
  }
  DictionaryAttr mark(DictionaryAttr info) {
    NamedAttrList updated(info);
    if (auto kind = info.getAs<StringAttr>("kind");
        kind && kind.getValue() == "string")
      updated.set("native_string_handle", BoolAttr::get(&getContext(), true));
    for (auto name : {"element", "native_open_actual"})
      if (auto child = info.getAs<DictionaryAttr>(name))
        updated.set(name, mark(child));
    return updated.getDictionary(&getContext());
  }
  ArrayAttr mark(ArrayAttr abi) {
    SmallVector<Attribute> result;
    for (auto attr : abi)
      result.push_back(mark(cast<DictionaryAttr>(attr)));
    return ArrayAttr::get(&getContext(), result);
  }
  Value constant(OpBuilder &b, Location loc, uint64_t n) {
    return hw::ConstantOp::create(b, loc, APInt(64, n));
  }
  sim::DPIFuncOp helper(StringRef name, unsigned count,
                        func::FuncOp target = {}) {
    if (!target)
      if (auto it = helpers.find(name); it != helpers.end())
        return it->second;
    auto top = getOperation();
    OpBuilder b(top.getBody(), top.getBody()->begin());
    SmallVector<sim::DPIArgument> formals;
    SmallVector<Attribute> abi;
    for (unsigned i = 0; i <= count; ++i) {
      bool result = i == count;
      formals.push_back(
          {b.getStringAttr("arg" + Twine(i)), b.getI64Type(),
           result ? sim::DPIDirection::Return : sim::DPIDirection::Input});
      abi.push_back(b.getDictionaryAttr(
          {b.getNamedAttr("kind", b.getStringAttr("longint")),
           b.getNamedAttr("width", b.getI64IntegerAttr(64)),
           b.getNamedAttr("signed", b.getBoolAttr(false)),
           b.getNamedAttr("four_state", b.getBoolAttr(false)),
           b.getNamedAttr("direction",
                          b.getStringAttr(result ? "return" : "in"))}));
    }
    auto fn = sim::DPIFuncOp::create(
        b, top.getLoc(), names.newName("string_import"),
        b.getStringAttr("private"),
        sim::DPIFunctionType::get(&getContext(), formals), ArrayAttr{},
        b.getStringAttr(name));
    fn->setAttr("chipyard.dpi_abi", b.getArrayAttr(abi));
    fn->setAttr(
        "chipyard.dpi_decl",
        b.getDictionaryAttr({b.getNamedAttr("c_name", b.getStringAttr(name)),
                             b.getNamedAttr("context", b.getBoolAttr(false)),
                             b.getNamedAttr("task", b.getBoolAttr(false)),
                             b.getNamedAttr("pure", b.getBoolAttr(false))}));
    if (target)
      fn->setAttr("chipyard.native_target", FlatSymbolRefAttr::get(target));
    else
      helpers[name] = fn;
    return fn;
  }
  Value call(OpBuilder &b, Location loc, StringRef name, ValueRange args) {
    return sim::DPICallOp::create(
               b, loc, TypeRange{b.getI64Type()},
               FlatSymbolRefAttr::get(helper(name, args.size())), Value{},
               Value{}, args)
        .getResult(0);
  }
  Value literal(OpBuilder &b, Location loc, StringRef value) {
    if (value.empty())
      return constant(b, loc, 0);
    auto top = getOperation();
    OpBuilder outer(top.getBody(), top.getBody()->begin());
    auto global = LLVM::GlobalOp::create(
        outer, loc, LLVM::LLVMArrayType::get(b.getI8Type(), value.size()), true,
        LLVM::Linkage::Private, names.newName("string_bytes"),
        b.getStringAttr(value));
    auto getter =
        func::FuncOp::create(outer, loc, names.newName("string_literal"),
                             b.getFunctionType({}, {b.getI64Type()}));
    getter.setPrivate();
    OpBuilder body = OpBuilder::atBlockBegin(getter.addEntryBlock());
    Value address = LLVM::AddressOfOp::create(body, loc, global);
    Value bits =
        LLVM::PtrToIntOp::create(body, loc, body.getI64Type(), address);
    Value result = call(body, loc, "chipyard_arc_string_create",
                        {bits, constant(body, loc, value.size())});
    func::ReturnOp::create(body, loc, ValueRange{result});
    auto fn = helper(getter.getSymName(), 0, getter);
    return sim::DPICallOp::create(b, loc, TypeRange{b.getI64Type()},
                                  FlatSymbolRefAttr::get(fn), Value{}, Value{},
                                  ValueRange{})
        .getResult(0);
  }
  FailureOr<Value> readMemory(sim::SVReadMemOp op, OpBuilder &b) {
    auto array = dyn_cast<hw::ArrayType>(op.getMemory().getType());
    Type element = array;
    SmallVector<uint64_t> counts;
    std::string dimensions;
    for (auto [low, descending] :
         llvm::zip(op.getDimLows(), op.getDimDescending())) {
      auto dimension = dyn_cast_or_null<hw::ArrayType>(element);
      if (!dimension || !dimension.getNumElements())
        return op.emitError("native readmem requires fixed storage for every "
                            "declared unpacked dimension"),
               failure();
      auto count = dimension.getNumElements();
      if (low < INT32_MIN || low > INT32_MAX ||
          count - 1 > uint64_t(INT32_MAX - low))
        return op.emitError("native readmem dimension exceeds SV index range"),
               failure();
      counts.push_back(count);
      dimensions += std::to_string(count) + " " + std::to_string(low) + " " +
                    std::to_string(descending) + " ";
      element = dimension.getElementType();
    }
    if (!array || counts.empty())
      return op.emitError("native readmem requires fixed array storage"),
             failure();
    auto bits = hw::getBitWidth(element);
    auto total = hw::getBitWidth(array);
    if (bits <= 0 || total <= 0 || total > IntegerType::kMaxWidth)
      return op.emitError(
                 "unsupported native readmem packed element/storage width"),
             failure();
    SmallVector<sim::DPIArgument> formals;
    SmallVector<Attribute> abi;
    bool multidimensional = counts.size() > 1;
    auto cName = b.getStringAttr(multidimensional ? "chipyard_arc_readmem_multi"
                                                  : "chipyard_arc_readmem");
    for (unsigned i = 0; i < (multidimensional ? 14u : 13u); i++) {
      bool memory = i == 1;
      formals.push_back(
          {b.getStringAttr("arg" + Twine(i)),
           memory ? Type(array) : Type(b.getI64Type()),
           memory ? sim::DPIDirection::InOut : sim::DPIDirection::Input});
      abi.push_back(b.getDictionaryAttr(
          {b.getNamedAttr("kind",
                          b.getStringAttr(memory ? "packed_array" : "longint")),
           b.getNamedAttr("width", b.getI64IntegerAttr(memory ? total : 64)),
           b.getNamedAttr("signed", b.getBoolAttr(false)),
           b.getNamedAttr("four_state", b.getBoolAttr(false)),
           b.getNamedAttr("direction",
                          b.getStringAttr(memory ? "inout" : "in"))}));
    }
    auto top = getOperation();
    sim::DPIFuncOp fn;
    {
      OpBuilder outer(top.getBody(), top.getBody()->begin());
      fn = sim::DPIFuncOp::create(
          outer, op.getLoc(), names.newName("readmem_import"),
          b.getStringAttr("private"),
          sim::DPIFunctionType::get(&getContext(), formals), ArrayAttr{},
          cName);
      fn->setAttr("chipyard.dpi_abi", b.getArrayAttr(abi));
      fn->setAttr(
          "chipyard.dpi_decl",
          b.getDictionaryAttr({b.getNamedAttr("c_name", cName),
                               b.getNamedAttr("context", b.getBoolAttr(false)),
                               b.getNamedAttr("task", b.getBoolAttr(false)),
                               b.getNamedAttr("pure", b.getBoolAttr(false))}));
    }
    auto loc = op.getLoc();
    auto address = [&](Value value) -> Value {
      if (!value)
        return constant(b, loc, 0);
      return comb::ConcatOp::create(
          b, loc,
          ValueRange{hw::ConstantOp::create(b, loc, APInt(32, 0)), value});
    };
    auto enums = op.getEnumValues();
    std::string allowed;
    if (enums)
      for (auto value : *enums)
        allowed += std::to_string(value) + " ";
    uint64_t flags = unsigned(bool(op.getStartAddr())) |
                     unsigned(bool(op.getFinishAddr())) * 2 |
                     unsigned(bool(op.getSliceLeft())) * 4 |
                     unsigned(bool(enums)) * 8;
    SmallVector<Value> arguments{op->getOperand(0),
                                 op.getMemory(),
                                 constant(b, loc, bits),
                                 constant(b, loc, counts.front()),
                                 constant(b, loc, op.getDimLows()[0]),
                                 constant(b, loc, op.getDimDescending()[0]),
                                 constant(b, loc, op.getIsBinary()),
                                 constant(b, loc, flags),
                                 address(op.getStartAddr()),
                                 address(op.getFinishAddr()),
                                 address(op.getSliceLeft()),
                                 address(op.getSliceRight()),
                                 literal(b, loc, allowed)};
    if (multidimensional)
      arguments.push_back(literal(b, loc, dimensions));
    auto call = sim::DPICallOp::create(b, loc, TypeRange{array},
                                       FlatSymbolRefAttr::get(fn), Value{},
                                       Value{}, arguments);
    return call.getResult(0);
  }
  void runOnOperation() override {
    auto top = getOperation();
    names.add(top);
    SmallVector<Operation *> strings;
    top.walk([&](Operation *op) {
      if (isa<sim::StringConstantOp, sim::StringLengthOp, sim::StringConcatOp,
              sim::StringGetOp, sim::StringCmpOp, sim::StringCompareOp,
              sim::StringToLowerOp, sim::StringToUpperOp, sim::IntToStringOp,
              sim::StringToIntOp, sim::SVFOpenOp, sim::SVReadMemOp>(op))
        strings.push_back(op);
    });
    // The existing native formatter consumes literal strings and packed
    // integer strings directly. Preserve that path, including byte filtering
    // and field padding; shared producers get an independent formatter copy.
    DenseSet<Operation *> formatValues;
    top.walk([&](sim::FormatStringOp format) {
      auto *producer = format->getOperand(0).getDefiningOp();
      if (!isa_and_nonnull<sim::StringConstantOp, sim::IntToStringOp>(producer))
        return;
      OpBuilder at(format);
      auto *copy = at.clone(*producer);
      format->setOperand(0, copy->getResult(0));
      formatValues.insert(copy);
    });
    DenseSet<Operation *> supported(strings.begin(), strings.end());
    bool unsupported = false;
    top.walk([&](Operation *op) {
      bool hasString = llvm::any_of(op->getOperandTypes(),
                                    [&](Type t) { return storage(t) != t; }) ||
                       llvm::any_of(op->getResultTypes(),
                                    [&](Type t) { return storage(t) != t; });
      if (!hasString || supported.contains(op) || formatValues.contains(op) ||
          (isa<sim::FormatStringOp>(op) &&
           formatValues.contains(op->getOperand(0).getDefiningOp())) ||
          isa<sim::DPICallOp, func::CallOp, func::ReturnOp, llhd::SignalOp,
              llhd::ProbeOp, llhd::DriveOp, llhd::ReturnOp, llhd::WaitOp,
              llhd::HaltOp, llhd::ProcessOp, hw::ArrayCreateOp, hw::ArrayGetOp,
              hw::ArraySliceOp, hw::ArrayConcatOp, hw::StructCreateOp,
              hw::StructExtractOp, hw::StructInjectOp, llhd::SigStructExtractOp,
              llhd::GetGlobalSignalOp, llhd::YieldOp, llhd::SigArrayGetOp,
              llhd::SigArraySliceOp, hw::BitcastOp, hw::InstanceOp,
              hw::OutputOp, comb::MuxOp, arith::SelectOp>(op) ||
          isa<BranchOpInterface>(op))
        return;
      op->emitError("unsupported native dynamic string consumer");
      unsupported = true;
    });
    if (unsupported) {
      signalPassFailure();
      return;
    }
    SmallVector<Operation *> constructors;
    top.walk([&](Operation *op) {
      if (isa<hw::ArrayCreateOp, hw::StructCreateOp>(op) &&
          storage(op->getResult(0).getType()) != op->getResult(0).getType())
        constructors.push_back(op);
    });
    // Change storage and transport types together. Unsupported consumers remain
    // explicit errors; no original SV operation is discarded to admit a model.
    top.walk([&](Operation *op) {
      for (auto &region : op->getRegions())
        for (auto &block : region)
          for (auto arg : block.getArguments())
            arg.setType(storage(arg.getType()));
      if (!formatValues.contains(op))
        for (auto result : op->getResults())
          result.setType(storage(result.getType()));
      if (auto type = op->getAttrOfType<TypeAttr>("module_type"))
        op->setAttr("module_type", TypeAttr::get(storage(type.getValue())));
      if (auto global = dyn_cast<llhd::GlobalSignalOp>(op))
        global.setType(storage(global.getType()));
      if (isa<func::FuncOp, llhd::CoroutineOp>(op)) {
        auto fn = cast<FunctionOpInterface>(op);
        auto type = dyn_cast<FunctionType>(fn.getFunctionType());
        if (type) {
          SmallVector<Type> inputs, outputs;
          for (auto t : type.getInputs())
            inputs.push_back(storage(t));
          for (auto t : type.getResults())
            outputs.push_back(storage(t));
          fn.setType(FunctionType::get(&getContext(), inputs, outputs));
        }
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
    for (auto *op : strings) {
      if (op->use_empty() &&
          isa<sim::StringConstantOp, sim::IntToStringOp>(op)) {
        op->erase();
        continue;
      }
      OpBuilder b(op);
      auto loc = op->getLoc();
      Value result;
      if (auto memory = dyn_cast<sim::SVReadMemOp>(op)) {
        auto lowered = readMemory(memory, b);
        if (failed(lowered)) {
          signalPassFailure();
          return;
        }
        result = *lowered;
      } else if (auto str = dyn_cast<sim::StringConstantOp>(op))
        result = literal(b, loc, str.getLiteral());
      else if (auto len = dyn_cast<sim::StringLengthOp>(op)) {
        result = call(b, loc, "chipyard_arc_string_length", op->getOperand(0));
        // Moore's SV .len() result is int; sim.string.length is i64. The SDK
        // leaves that boundary as a conversion cast instead of truncating it.
        SmallVector<UnrealizedConversionCastOp> casts;
        for (auto *user : op->getResult(0).getUsers())
          if (auto conversion = dyn_cast<UnrealizedConversionCastOp>(user))
            if (conversion.getNumOperands() == 1 &&
                conversion.getNumResults() == 1 &&
                conversion->getAttrs().empty() &&
                isa<IntegerType>(conversion.getResult(0).getType()))
              casts.push_back(conversion);
        for (auto conversion : casts) {
          OpBuilder at(conversion);
          unsigned bits =
              cast<IntegerType>(conversion.getResult(0).getType()).getWidth();
          Value converted = result;
          if (bits < 64)
            converted = comb::ExtractOp::create(at, loc, result, 0, bits);
          else if (bits > 64)
            converted = comb::ConcatOp::create(
                at, loc,
                ValueRange{hw::ConstantOp::create(at, loc, APInt(bits - 64, 0)),
                           result});
          conversion.getResult(0).replaceAllUsesWith(converted);
          conversion.erase();
        }
      } else if (auto open = dyn_cast<sim::SVFOpenOp>(op)) {
        auto mode = open.getMode();
        result = call(
            b, loc, "chipyard_arc_string_fopen",
            {op->getOperand(0), constant(b, loc, mode ? int64_t(*mode) : -1)});
        result = comb::ExtractOp::create(b, loc, result, 0, 32);
      } else if (auto cat = dyn_cast<sim::StringConcatOp>(op)) {
        result = constant(b, loc, 0);
        for (auto input : op->getOperands())
          result = call(b, loc, "chipyard_arc_string_concat", {result, input});
      } else if (auto get = dyn_cast<sim::StringGetOp>(op)) {
        auto index = comb::ConcatOp::create(
            b, loc,
            ValueRange{hw::ConstantOp::create(b, loc, APInt(32, 0)),
                       get.getIndex()});
        result =
            call(b, loc, "chipyard_arc_string_get", {op->getOperand(0), index});
        result = comb::ExtractOp::create(b, loc, result, 0, 8);
      } else if (auto cmp = dyn_cast<sim::StringCmpOp>(op)) {
        result = call(b, loc, "chipyard_arc_string_compare",
                      {op->getOperand(0), op->getOperand(1)});
        comb::ICmpPredicate predicate;
        switch (cmp.getPredicate()) {
        case sim::StringCmpPredicate::eq:
          predicate = comb::ICmpPredicate::eq;
          break;
        case sim::StringCmpPredicate::ne:
          predicate = comb::ICmpPredicate::ne;
          break;
        case sim::StringCmpPredicate::lt:
          predicate = comb::ICmpPredicate::slt;
          break;
        case sim::StringCmpPredicate::le:
          predicate = comb::ICmpPredicate::sle;
          break;
        case sim::StringCmpPredicate::gt:
          predicate = comb::ICmpPredicate::sgt;
          break;
        case sim::StringCmpPredicate::ge:
          predicate = comb::ICmpPredicate::sge;
          break;
        }
        result = comb::ICmpOp::create(b, loc, predicate, result,
                                      constant(b, loc, 0));
      } else if (auto cmp = dyn_cast<sim::StringCompareOp>(op)) {
        result = call(b, loc, "chipyard_arc_string_compare",
                      {op->getOperand(0), op->getOperand(1)});
        result = comb::ExtractOp::create(b, loc, result, 0, 32);
      } else if (isa<sim::StringToLowerOp, sim::StringToUpperOp>(op))
        result = call(b, loc, "chipyard_arc_string_case",
                      {op->getOperand(0),
                       constant(b, loc, isa<sim::StringToUpperOp>(op))});
      else if (auto cast = dyn_cast<sim::IntToStringOp>(op)) {
        auto input = cast.getInput();
        auto width = mlir::cast<IntegerType>(input.getType()).getWidth();
        result = constant(b, loc, 0);
        for (unsigned byte = (width + 7) / 8; byte-- > 0;) {
          unsigned bits = std::min(8u, width - byte * 8);
          Value part = comb::ExtractOp::create(b, loc, input, byte * 8, bits);
          part = comb::ConcatOp::create(
              b, loc,
              ValueRange{hw::ConstantOp::create(b, loc, APInt(64 - bits, 0)),
                         part});
          result =
              call(b, loc, "chipyard_arc_string_append_byte", {result, part});
        }
      } else if (auto cast = dyn_cast<sim::StringToIntOp>(op)) {
        unsigned width =
            mlir::cast<IntegerType>(cast.getResult().getType()).getWidth();
        SmallVector<Value> parts;
        for (unsigned offset = 0; offset < width; offset += 64) {
          Value part = call(b, loc, "chipyard_arc_string_pack",
                            {op->getOperand(0), constant(b, loc, offset / 8)});
          if (width - offset < 64)
            part = comb::ExtractOp::create(b, loc, part, 0, width - offset);
          parts.push_back(part);
        }
        std::reverse(parts.begin(), parts.end());
        result = parts.size() == 1
                     ? parts[0]
                     : comb::ConcatOp::create(b, loc, parts).getResult();
      }
      op->getResult(0).replaceAllUsesWith(result);
      op->erase();
    }
    foldNativeAggregates(constructors);
  }
};
} // namespace
extern "C" ::mlir::PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardNativeStrings", LLVM_VERSION_STRING,
          []() { PassRegistration<NativeStrings>(); }};
}
