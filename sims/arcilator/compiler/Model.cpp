// Late array normalization, allocated-model outlining, and SDK layout export.
#include "WideArithmetic.h"
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Arc/ModelInfo.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Seq/SeqTypes.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/RegionUtils.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/ToolOutputFile.h"
#include <limits>
#include <memory>
using namespace mlir;
using namespace circt;
namespace {
// Make the last partial word physically canonical before observing it. An
// arbitrary-width LLVM value can retain byte padding in host storage; a
// separately compiled, legal-width mask prevents that padding from becoming a
// coroutine event. Keep this helper opaque to interprocedural optimizations.
static Value maskDPIWord(OpBuilder &builder, Location loc, Value value,
                         unsigned width) {
  assert(value.getType().isInteger(64) && width > 0 && width < 64);
  auto module =
      builder.getInsertionBlock()->getParentOp()->getParentOfType<ModuleOp>();
  if (!module)
    module = dyn_cast<ModuleOp>(builder.getInsertionBlock()->getParentOp());
  auto reference =
      module->getAttrOfType<FlatSymbolRefAttr>("chipyard.dpi_word_mask");
  func::FuncOp helper;
  if (reference)
    helper = module.lookupSymbol<func::FuncOp>(reference.getValue());
  if (!helper) {
    OpBuilder::InsertionGuard guard(builder);
    builder.setInsertionPointToEnd(module.getBody());
    circt::Namespace names;
    names.add(module);
    auto type = builder.getI64Type();
    helper = func::FuncOp::create(
        builder, loc, names.newName("chipyard_dpi_mask_word"),
        builder.getFunctionType({type, type}, {type}));
    helper->setAttr("llvm.no_inline", builder.getUnitAttr());
    helper->setAttr("llvm.passthrough",
                    builder.getStrArrayAttr({"noinline", "optnone"}));
    helper->setAttr(
        "llvm.linkage",
        LLVM::LinkageAttr::get(builder.getContext(), LLVM::Linkage::WeakODR));
    auto *entry = helper.addEntryBlock();
    builder.setInsertionPointToEnd(entry);
    auto masked = arith::AndIOp::create(builder, loc, entry->getArgument(0),
                                        entry->getArgument(1));
    func::ReturnOp::create(builder, loc, ValueRange{masked});
    module->setAttr("chipyard.dpi_word_mask", FlatSymbolRefAttr::get(helper));
  }
  auto mask =
      hw::ConstantOp::create(builder, loc, APInt::getLowBitsSet(64, width));
  return func::CallOp::create(builder, loc, helper, ValueRange{value, mask})
      .getResult(0);
}
// Keep event activation on effectful combinational regions. The stock Arc
// conversion can leave nested DPI calls unlowered, and a bare unclocked call
// can otherwise execute on every model evaluation, including stable settles.
struct SensitizeDPI : PassWrapper<SensitizeDPI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SensitizeDPI)
  SensitizeDPI() = default;
  SensitizeDPI(const SensitizeDPI &other) : PassWrapper(other) {}
  Option<bool> freezeSampling{*this, "freeze-sampling", llvm::cl::init(false)};
  StringRef getArgument() const override { return "chipyard-sensitize-dpi"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry
        .insert<llhd::LLHDDialect, hw::HWDialect, comb::CombDialect,
                arith::ArithDialect, cf::ControlFlowDialect, scf::SCFDialect,
                seq::SeqDialect, func::FuncDialect, LLVM::LLVMDialect>();
  }
  // Only the generated, byte-ABI wide-shift helper is a pure calculation.
  // no_output_readback alone also marks diagnostic helpers and is insufficient.
  bool pureWideShift(sim::DPICallOp call, SymbolTableCollection &symbols) {
    return chipyard_arc_compiler::wideShiftMode(call, symbols).has_value();
  }
  bool observable(Type type) {
    return isa<FloatType, seq::ClockType>(type) || hw::getBitWidth(type) >= 0;
  }
  SmallVector<Value> observations(OpBuilder &builder, Location loc,
                                  Value value) {
    auto type = value.getType();
    if (isa<seq::ClockType>(type))
      return {seq::FromClockOp::create(builder, loc, value)};
    if (auto fp = dyn_cast<FloatType>(type))
      return {arith::BitcastOp::create(
          builder, loc, builder.getIntegerType(fp.getWidth()), value)};
    auto width = hw::getBitWidth(type);
    if (width == 0)
      return {};
    if (!isa<IntegerType>(type))
      value = hw::BitcastOp::create(builder, loc, builder.getIntegerType(width),
                                    value);
    if (width == 1 || width == 8 || width == 16 || width == 32 || width == 64)
      return {value};
    SmallVector<Value> words;
    for (int64_t offset = 0; offset < width; offset += 64) {
      Value word = value;
      if (offset) {
        auto amount = arith::ConstantOp::create(
            builder, loc, builder.getIntegerAttr(value.getType(), offset));
        word = arith::ShRUIOp::create(builder, loc, word, amount);
      }
      if (width > 64)
        word =
            arith::TruncIOp::create(builder, loc, builder.getI64Type(), word);
      else
        word = arith::ExtUIOp::create(builder, loc, builder.getI64Type(), word);
      if (width - offset < 64)
        word = maskDPIWord(builder, loc, word, width - offset);
      words.push_back(word);
    }
    return words;
  }
  Value zero(OpBuilder &builder, Location loc, Type type) {
    if (isa<seq::ClockType>(type)) {
      auto bit = hw::ConstantOp::create(builder, loc, APInt(1, 0));
      return seq::ToClockOp::create(builder, loc, bit);
    }
    if (auto fp = dyn_cast<FloatType>(type))
      return arith::ConstantOp::create(builder, loc, type,
                                       builder.getFloatAttr(fp, 0.0));
    auto bits = hw::getBitWidth(type);
    Value value = hw::ConstantOp::create(builder, loc, APInt(bits, 0));
    if (value.getType() != type)
      value = hw::BitcastOp::create(builder, loc, type, value);
    return value;
  }
  bool normalizeEnable(sim::DPICallOp call) {
    if (call.getClock() || !call.getEnable() ||
        !llvm::all_of(call.getResultTypes(), [&](Type type) {
          return isa<FloatType>(type) || hw::getBitWidth(type) >= 0;
        }))
      return false;
    OpBuilder builder(call);
    auto conditional = scf::IfOp::create(
        builder, call.getLoc(), call.getResultTypes(), call.getEnable(), true);
    auto body = [&](Block *block, bool enabled) {
      builder.setInsertionPointToEnd(block);
      if (!block->empty() && isa<scf::YieldOp>(block->back())) {
        block->back().erase();
        builder.setInsertionPointToEnd(block);
      }
      SmallVector<Value> results;
      if (enabled) {
        auto clone = cast<sim::DPICallOp>(builder.clone(*call));
        clone.getEnableMutable().clear();
        results.append(clone.getResults().begin(), clone.getResults().end());
      } else {
        // Disabled unclocked results are X in SV. Arc's two-state value is
        // zero, with no callback and no invented retained-output dependency.
        for (auto type : call.getResultTypes())
          results.push_back(zero(builder, call.getLoc(), type));
      }
      scf::YieldOp::create(builder, call.getLoc(), results);
    };
    body(conditional.thenBlock(), true);
    body(conditional.elseBlock(), false);
    call.replaceAllUsesWith(conditional.getResults());
    call.erase();
    return true;
  }
  void runOnOperation() override {
    SmallVector<sim::DPICallOp> bare, enabled;
    SymbolTableCollection symbols;
    getOperation().walk([&](sim::DPICallOp call) {
      if (call.getClock() || pureWideShift(call, symbols))
        return;
      if (isa<hw::HWModuleOp>(call->getParentOp()))
        bare.push_back(call);
      else if (call.getEnable())
        enabled.push_back(call);
    });
    // A bare call becomes a one-call combinational region before sharing the
    // same region-level sensitivity analysis as imported procedural RTL.
    for (auto call : bare) {
      if (!llvm::all_of(call->getOperandTypes(),
                        [&](Type type) { return observable(type); }))
        continue;
      OpBuilder builder(call);
      auto region = llhd::CombinationalOp::create(builder, call.getLoc(),
                                                  call.getResultTypes());
      auto *block = new Block;
      region.getBody().push_back(block);
      call->moveBefore(block, block->end());
      call.replaceAllUsesWith(region.getResults());
      builder.setInsertionPointToEnd(block);
      llhd::YieldOp::create(builder, call.getLoc(), call.getResults());
      if (call.getEnable())
        enabled.push_back(call);
    }
    SmallVector<llhd::CombinationalOp> regions;
    DenseSet<Operation *> eligible;
    getOperation().walk([&](llhd::CombinationalOp op) {
      bool hasDPI = false, hasClock = false, hasPrint = false;
      op.walk([&](sim::DPICallOp call) {
        hasDPI = true;
        hasClock |= bool(call.getClock());
      });
      op.walk([&](sim::PrintFormattedProcOp) { hasPrint = true; });
      op.walk([&](func::CallOp call) {
        hasDPI |= call->hasAttr("chipyard.native_format");
        hasPrint |= call->hasAttr("chipyard.native_print");
      });
      llvm::SetVector<Value> inputs;
      getUsedValuesDefinedAbove(op.getBody(), inputs);
      if ((hasDPI || hasPrint) && !hasClock &&
          llvm::all_of(op.getResultTypes(),
                       [&](Type type) { return observable(type); }) &&
          llvm::all_of(inputs, [&](Value value) {
            auto *producer = value.getDefiningOp();
            return observable(value.getType()) ||
                   (producer && producer->hasTrait<OpTrait::ConstantLike>());
          })) {
        regions.push_back(op);
        eligible.insert(op);
      }
    });
    for (auto call : enabled) {
      auto region = call->getParentOfType<llhd::CombinationalOp>();
      if (!region || eligible.contains(region))
        normalizeEnable(call);
    }
    unsigned count = 0;
    DenseMap<Operation *, Value> phaseSignals;
    if (!regions.empty()) {
      NamedAttrList names(getOperation()->getAttrOfType<DictionaryAttr>(
          "chipyard.sampling_inputs"));
      DenseMap<Operation *, StringAttr> portNames;
      for (auto module : getOperation().getOps<hw::HWModuleOp>()) {
        StringAttr name =
            dyn_cast_or_null<StringAttr>(names.get(module.getSymName()));
        Value input;
        if (name) {
          for (unsigned i = 0; i < module.getNumInputPorts(); ++i)
            if (module.getArgName(i) == name)
              input = module.getBodyBlock()->getArgument(i);
        }
        if (!input) {
          auto added = module.appendInput("arc_sampling",
                                          IntegerType::get(&getContext(), 8));
          name = added.first;
          input = added.second;
          names.set(module.getSymName(), name);
        }
        phaseSignals[module] = input;
        portNames[module] = name;
      }
      bool invalid = false;
      getOperation().walk([&](hw::InstanceOp instance) {
        auto target = SymbolTable::lookupNearestSymbolFrom<hw::HWModuleOp>(
            instance, instance.getModuleNameAttr());
        if (!target || !phaseSignals.contains(target))
          return;
        auto parent = instance->getParentOfType<hw::HWModuleOp>();
        if (!parent || !phaseSignals.contains(parent)) {
          instance.emitError("cannot propagate native sampling control outside "
                             "a hardware module");
          invalid = true;
          return;
        }
        auto name = portNames[target];
        if (instance.getNumOperands() == target.getNumInputPorts() &&
            instance.getArgNames()[instance.getArgNames().size() - 1] == name)
          return;
        instance.getInputsMutable().append(phaseSignals[parent]);
        SmallVector<Attribute> arguments(instance.getArgNames().begin(),
                                         instance.getArgNames().end());
        arguments.push_back(name);
        instance.setArgNamesAttr(ArrayAttr::get(&getContext(), arguments));
      });
      if (invalid) {
        signalPassFailure();
        return;
      }
      getOperation()->setAttr("chipyard.sampling_inputs",
                              names.getDictionary(&getContext()));
    }
    func::FuncOp samplingMarker;
    if (freezeSampling && !regions.empty()) {
      OpBuilder top(getOperation().getBodyRegion());
      top.setInsertionPointToStart(getOperation().getBody());
      std::string name = "__chipyard_arc_sampling_identity";
      while (getOperation().lookupSymbol(name))
        name += "_";
      auto byte = top.getIntegerType(8);
      samplingMarker =
          func::FuncOp::create(top, regions.front().getLoc(), name,
                               top.getFunctionType({byte}, {byte}));
      samplingMarker.setPrivate();
      samplingMarker->setAttr("chipyard.native_sampling_identity",
                              top.getUnitAttr());
      SmallVector<Attribute> markers;
      if (auto previous = getOperation()->getAttrOfType<ArrayAttr>(
              "chipyard.native_sampling_markers"))
        llvm::append_range(markers, previous);
      markers.push_back(
          FlatSymbolRefAttr::get(samplingMarker.getSymNameAttr()));
      getOperation()->setAttr("chipyard.native_sampling_markers",
                              top.getArrayAttr(markers));
    }
    for (auto region : regions) {
      llvm::SetVector<Value> inputs;
      getUsedValuesDefinedAbove(region.getBody(), inputs);
      SmallVector<Operation *> localConstants;
      for (auto value : inputs) {
        auto *producer = value.getDefiningOp();
        if (!observable(value.getType()) && producer &&
            producer->hasTrait<OpTrait::ConstantLike>())
          localConstants.push_back(producer);
      }
      OpBuilder constants(&region.getBody().front(),
                          region.getBody().front().begin());
      for (auto *producer : localConstants) {
        auto *copy = constants.clone(*producer);
        for (auto [original, local] :
             llvm::zip(producer->getResults(), copy->getResults()))
          original.replaceUsesWithIf(local, [&](OpOperand &use) {
            return region.getBody().isAncestor(
                use.getOwner()->getParentRegion());
          });
      }
      inputs.clear();
      getUsedValuesDefinedAbove(region.getBody(), inputs);
      if (!llvm::all_of(
              inputs, [&](Value value) { return observable(value.getType()); }))
        continue;
      OpBuilder builder(region);
      auto loc = region.getLoc();
      Value sampling = phaseSignals[region->getParentOp()];
      // Keep the observed byte separate from the branch predicate. CFG
      // canonicalization replaces a dominating condition with true in its
      // taken successor, which would turn a wait on that condition into a
      // constant observation and permanently suspend the coroutine.
      auto phaseZero = hw::ConstantOp::create(builder, loc, APInt(8, 0));
      Value isSampling = comb::ICmpOp::create(
          builder, loc, comb::ICmpPredicate::ne, sampling, phaseZero);
      SmallVector<Value> observed;
      SmallVector<std::pair<Value, Value>> floatInputs;
      for (auto value : inputs) {
        if (auto *op = value.getDefiningOp();
            op && op->hasTrait<OpTrait::ConstantLike>())
          continue;
        auto words = observations(builder, loc, value);
        observed.append(words);
        if (isa<FloatType>(value.getType()))
          floatInputs.push_back({value, words.front()});
      }
      // Coroutine frames use HW bit widths, which do not include floating
      // fields. Carry IEEE bits across suspensions and decode only inside the
      // activation or at the original region's external uses.
      SmallVector<Type> resultTypes;
      for (auto type : region.getResultTypes())
        resultTypes.push_back(
            isa<FloatType>(type)
                ? builder.getIntegerType(cast<FloatType>(type).getWidth())
                : type);
      auto process = llhd::ProcessOp::create(builder, loc, resultTypes);
      process->setDiscardableAttrs(region->getDiscardableAttrDictionary());
      process.getBody().takeBody(region.getBody());
      auto *entry = &process.getBody().front();
      for (auto [value, bits] : floatInputs) {
        builder.setInsertionPointToStart(entry);
        Value restored =
            arith::BitcastOp::create(builder, loc, value.getType(), bits);
        value.replaceUsesWithIf(restored, [&](OpOperand &use) {
          return process.getBody().isAncestor(
              use.getOwner()->getParentRegion());
        });
      }
      auto *start = new Block;
      process.getBody().push_front(start);
      auto *check = new Block;
      auto *frozen = new Block;
      process.getBody().getBlocks().insert(Region::iterator(entry), check);
      process.getBody().getBlocks().insert(Region::iterator(entry), frozen);
      for (auto type : process.getResultTypes())
        check->addArgument(type, loc);
      builder.setInsertionPointToEnd(start);
      SmallVector<Value> initial;
      for (auto type : process.getResultTypes())
        initial.push_back(zero(builder, loc, type));
      cf::BranchOp::create(builder, loc, check, initial);
      builder.setInsertionPointToEnd(check);
      if (samplingMarker) {
        auto marker = func::CallOp::create(builder, loc, samplingMarker,
                                           ValueRange{sampling});
        auto zero = hw::ConstantOp::create(builder, loc, APInt(8, 0));
        isSampling = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::ne,
                                          marker.getResult(0), zero);
      }
      cf::CondBranchOp::create(builder, loc, isSampling, frozen, entry);
      builder.setInsertionPointToEnd(frozen);
      llhd::WaitOp::create(builder, loc, check->getArguments(), Value{},
                           ValueRange{sampling}, check->getArguments(), check);
      for (auto &block : process.getBody()) {
        auto yield = dyn_cast<llhd::YieldOp>(block.getTerminator());
        if (!yield)
          continue;
        builder.setInsertionPoint(yield);
        SmallVector<Value> yielded;
        for (auto value : yield.getOperands()) {
          if (auto fp = dyn_cast<FloatType>(value.getType()))
            yielded.push_back(arith::BitcastOp::create(
                builder, yield.getLoc(), builder.getIntegerType(fp.getWidth()),
                value));
          else
            yielded.push_back(value);
        }
        if (observed.empty())
          llhd::HaltOp::create(builder, yield.getLoc(), yielded);
        else
          llhd::WaitOp::create(builder, yield.getLoc(), yielded, Value{},
                               observed, yielded, check);
        yield.erase();
      }
      builder.setInsertionPointAfter(process);
      for (auto [original, result] :
           llvm::zip(region.getResults(), process.getResults())) {
        Value replacement = result;
        if (isa<FloatType>(original.getType()))
          replacement = arith::BitcastOp::create(builder, loc,
                                                 original.getType(), result);
        original.replaceAllUsesWith(replacement);
      }
      region.erase();
      ++count;
    }
    if (count)
      llvm::errs() << "Sensitized " << count << " combinational DPI regions\n";
  }
};

// Preserve the logical DPI signature at call sites, but interpose an LLVM-side
// bridge with the actual SystemVerilog C representation. Stack buffers live in
// an ordinary function, so clocked graph calls need no graph-level allocations.
struct NativeDPIABI : PassWrapper<NativeDPIABI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeDPIABI)
  NativeDPIABI() = default;
  NativeDPIABI(const NativeDPIABI &other) : PassWrapper(other) {}
  Option<bool> validateOnly{*this, "validate-only", llvm::cl::init(false)};
  StringRef getArgument() const override { return "chipyard-native-dpi-abi"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, func::FuncDialect, hw::HWDialect,
                    LLVM::LLVMDialect, sim::SimDialect>();
  }
  enum Repr { Atom, Bit, Logic, Bits, Logics, Real, String, Handle, Array };
  func::FuncOp stringData, stringCopy, openCreate, openDelete;
  struct Argument {
    sim::DPIArgument original;
    Type logical, scalar;
    Repr repr;
    unsigned width;
    bool isSigned;
    bool contextScope;
    unsigned count = 0;
    DictionaryAttr openActual;
    std::shared_ptr<Argument> element;
    bool vector() const {
      return repr == Bits || repr == Logics || repr == Array;
    }
    uint64_t cBytes() const {
      if (repr == Array)
        return uint64_t(count) * element->cBytes();
      if (vector())
        return uint64_t((width + 31) / 32) * (repr == Logics ? 8 : 4);
      return (repr == Bit || repr == Logic) ? 1 : width / 8;
    }
    bool byref() const {
      return original.dir == sim::DPIDirection::Output ||
             original.dir == sim::DPIDirection::InOut;
    }
  };
  bool usesString(const Argument &arg) {
    return arg.repr == String || (arg.element && usesString(*arg.element));
  }
  DictionaryAttr originalABI(DictionaryAttr info) {
    NamedAttrList fields(info);
    fields.erase("native_string_handle");
    fields.erase("native_chandle_handle");
    for (auto name : {"element", "native_open_actual"})
      if (auto child = info.getAs<DictionaryAttr>(name))
        fields.set(name, originalABI(child));
    return fields.getDictionary(&getContext());
  }
  std::string cElement(const Argument &arg) {
    if (arg.repr == Array)
      return cElement(*arg.element);
    if (arg.repr == Bits)
      return "unsigned32";
    if (arg.repr == Logics)
      return "logic_word";
    if (arg.repr == Bit || arg.repr == Logic)
      return "unsigned8";
    if (arg.repr == String)
      return "const_char_pointer";
    if (arg.repr == Handle)
      return "void_pointer";
    if (arg.repr == Real)
      return "float" + std::to_string(arg.width);
    return std::string(arg.isSigned ? "signed" : "unsigned") +
           std::to_string(arg.width);
  }
  ArrayAttr cPrototype(OpBuilder &b, ArrayRef<Argument> args) {
    SmallVector<Attribute> types;
    for (auto &arg : args) {
      if (arg.contextScope)
        continue;
      if (arg.openActual) {
        types.push_back(b.getStringAttr("open_array_handle"));
        continue;
      }
      auto type = cElement(arg);
      if (arg.vector() || arg.byref())
        type += arg.original.dir == sim::DPIDirection::Input ? "_const_pointer"
                                                             : "_pointer";
      if (arg.original.dir == sim::DPIDirection::Return)
        type = "return_" + type;
      types.push_back(b.getStringAttr(type));
    }
    return b.getArrayAttr(types);
  }
  bool compatibleOpen(DictionaryAttr formal, DictionaryAttr actual,
                      bool &hasOpen) {
    if (!formal || !actual)
      return false;
    auto fk = formal.getAs<StringAttr>("kind"),
         ak = actual.getAs<StringAttr>("kind");
    if (!fk || !ak)
      return false;
    if (fk.getValue() == "open_array" || fk.getValue() == "unpacked_array") {
      if (ak.getValue() != "unpacked_array")
        return false;
      if (fk.getValue() == "open_array") {
        auto packed = formal.getAs<BoolAttr>("packed");
        if (!packed || packed.getValue())
          return false;
        hasOpen = true;
      } else {
        auto fl = formal.getAs<IntegerAttr>("left"),
             fr = formal.getAs<IntegerAttr>("right"),
             al = actual.getAs<IntegerAttr>("left"),
             ar = actual.getAs<IntegerAttr>("right");
        if (!fl || !fr || !al || !ar || !llvm::isInt<32>(fl.getInt()) ||
            !llvm::isInt<32>(fr.getInt()) || !llvm::isInt<32>(al.getInt()) ||
            !llvm::isInt<32>(ar.getInt()) ||
            std::abs(fl.getInt() - fr.getInt()) !=
                std::abs(al.getInt() - ar.getInt()))
          return false;
      }
      return compatibleOpen(formal.getAs<DictionaryAttr>("element"),
                            actual.getAs<DictionaryAttr>("element"), hasOpen);
    }
    NamedAttrList f(formal), a(actual);
    for (auto name : {"direction", "native_open_actual"}) {
      f.erase(name);
      a.erase(name);
    }
    return f.getDictionary(&getContext()) == a.getDictionary(&getContext());
  }
  LogicalResult describeArgument(sim::DPIFuncOp function,
                                 sim::DPIArgument formal, Attribute attribute,
                                 SmallVectorImpl<Argument> &arguments,
                                 bool contextScope) {
    auto *ctx = &getContext();
    auto info = dyn_cast<DictionaryAttr>(attribute);
    auto kindAttr = info ? info.getAs<StringAttr>("kind") : StringAttr{};
    auto kind = kindAttr ? kindAttr.getValue() : StringRef{};
    if (auto actual = info ? info.getAs<DictionaryAttr>("native_open_actual")
                           : DictionaryAttr{}) {
      bool hasOpen = false;
      if (!compatibleOpen(info, actual, hasOpen) || !hasOpen || contextScope)
        return function.emitError(
            "invalid native open-array actual provenance");
      SmallVector<Argument> described;
      if (failed(describeArgument(function, formal, actual, described, false)))
        return failure();
      if (described[0].repr != Array)
        return function.emitError(
            "native open-array actual requires fixed unpacked shape");
      described[0].openActual = actual;
      arguments.push_back(std::move(described[0]));
      return success();
    }
    if (kind == "unpacked_array") {
      auto array = dyn_cast<hw::ArrayType>(formal.type);
      auto left = info.getAs<IntegerAttr>("left"),
           right = info.getAs<IntegerAttr>("right");
      auto childInfo = info.getAs<DictionaryAttr>("element");
      if (!array || !left || !right || !childInfo ||
          !llvm::isInt<32>(left.getInt()) || !llvm::isInt<32>(right.getInt()) ||
          array.getNumElements() !=
              uint64_t(std::abs(left.getInt() - right.getInt()) + 1) ||
          array.getNumElements() > std::numeric_limits<unsigned>::max() ||
          formal.dir == sim::DPIDirection::Return ||
          formal.dir == sim::DPIDirection::Ref)
        return function.emitError(
            "invalid native unpacked DPI array shape or direction");
      SmallVector<Argument> child;
      sim::DPIArgument childFormal{formal.name, array.getElementType(),
                                   sim::DPIDirection::Input};
      if (failed(
              describeArgument(function, childFormal, childInfo, child, false)))
        return failure();
      uint64_t width = uint64_t(child[0].width) * array.getNumElements();
      if (!width || width > std::numeric_limits<unsigned>::max() ||
          hw::getBitWidth(formal.type) != int64_t(width))
        return function.emitError(
            "unpacked DPI array metadata does not match logical type");
      Argument arg{formal,
                   IntegerType::get(ctx, width),
                   LLVM::LLVMPointerType::get(ctx),
                   Array,
                   unsigned(width),
                   false,
                   contextScope};
      arg.count = array.getNumElements();
      arg.element = std::make_shared<Argument>(child[0]);
      arguments.push_back(std::move(arg));
      return success();
    }
    auto widthAttr = info ? info.getAs<IntegerAttr>("width") : IntegerAttr{};
    auto stringHandle =
        info ? info.getAs<BoolAttr>("native_string_handle") : BoolAttr{};
    bool nativeString =
        kind == "string" && stringHandle && stringHandle.getValue();
    auto pointerHandle =
        info ? info.getAs<BoolAttr>("native_chandle_handle") : BoolAttr{};
    bool nativeHandle =
        kind == "chandle" && pointerHandle && pointerHandle.getValue();
    unsigned width = nativeString || nativeHandle ? 64
                     : widthAttr                  ? widthAttr.getInt()
                     : (kind == "real" || kind == "realtime") ? 64
                     : kind == "shortreal"                    ? 32
                                                              : 0;
    if (!width || formal.dir == sim::DPIDirection::Ref)
      return function.emitError(
                 "unsupported native DPI formal representation: ")
             << kind;
    Repr repr;
    if (nativeString)
      repr = String;
    else if (nativeHandle)
      repr = Handle;
    else if (kind == "bit")
      repr = Bit;
    else if (kind == "logic")
      repr = Logic;
    else if (kind == "byte" || kind == "shortint" || kind == "int" ||
             kind == "longint")
      repr = Atom;
    else if (kind == "packed_array" || kind == "packed_struct" ||
             kind == "packed_union" || kind == "integer") {
      auto state = info.getAs<BoolAttr>("four_state");
      if (!state)
        return function.emitError("missing native DPI state domain");
      repr = state.getValue() ? Logics : Bits;
    } else if (kind == "real" || kind == "realtime" || kind == "shortreal")
      repr = Real;
    else
      return function.emitError(
                 "unsupported native DPI formal representation: ")
             << kind;
    unsigned atomWidth = kind == "byte"       ? 8
                         : kind == "shortint" ? 16
                         : kind == "int"      ? 32
                                              : 64;
    if ((repr == Atom && width != atomWidth) ||
        ((repr == Bit || repr == Logic) && width != 1) ||
        (repr == Real ? (!isa<FloatType>(formal.type) ||
                         cast<FloatType>(formal.type).getWidth() != width)
                      : hw::getBitWidth(formal.type) != width))
      return function.emitError(
          "SystemVerilog DPI metadata does not match logical formal type");
    if ((repr == Bits || repr == Logics) &&
        formal.dir == sim::DPIDirection::Return)
      return function.emitError("packed DPI returns require a native binding");
    Type logical = IntegerType::get(ctx, width);
    Type scalar =
        repr == String || repr == Handle ? Type(LLVM::LLVMPointerType::get(ctx))
        : repr == Real
            ? (width == 32 ? Type(Float32Type::get(ctx))
                           : Type(Float64Type::get(ctx)))
            : IntegerType::get(ctx, repr == Bit || repr == Logic ? 8 : width);
    auto sign = info.getAs<BoolAttr>("signed");
    arguments.push_back({formal, logical, scalar, repr, width,
                         sign && sign.getValue(), contextScope});
    return success();
  }
  LogicalResult describe(sim::DPIFuncOp function,
                         SmallVectorImpl<Argument> &arguments) {
    auto abi = function->getAttrOfType<ArrayAttr>("chipyard.dpi_abi");
    auto decl = function->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
    if (!abi || !decl ||
        abi.size() != function.getDpiFunctionType().getArguments().size())
      return function.emitError(
          "native DPI requires original SystemVerilog ABI metadata");
    bool scoped = function->hasAttr("chipyard.native_context_scope");
    if (scoped) {
      auto original =
          function->getAttrOfType<ArrayAttr>("chipyard.original_dpi_abi");
      bool valid = original && abi.size() == original.size() + 1;
      auto extra =
          abi.empty() ? DictionaryAttr{} : dyn_cast<DictionaryAttr>(abi[0]);
      auto flag =
          extra ? extra.getAs<BoolAttr>("native_context_scope") : BoolAttr{};
      auto extraKind = extra ? extra.getAs<StringAttr>("kind") : StringAttr{};
      auto context = decl.getAs<BoolAttr>("context");
      valid &= flag && flag.getValue() && extraKind &&
               extraKind.getValue() == "longint" && context &&
               context.getValue();
      auto formals = function.getDpiFunctionType().getArguments();
      valid &= !formals.empty() && formals[0].dir == sim::DPIDirection::Input &&
               formals[0].type.isInteger(64);
      if (valid)
        for (unsigned i = 0; i < original.size(); ++i) {
          valid &= originalABI(cast<DictionaryAttr>(abi[i + 1])) == original[i];
        }
      if (!valid)
        return function.emitError(
            "invalid native context scope transport provenance");
    }
    if (auto flag = decl.getAs<BoolAttr>("context"); flag && flag.getValue()) {
      // Compiled export kernels execute under the selected per-instance scope.
      // Every use must be in such a frame; ordinary HW imports still need a
      // physical caller-scope handle carried through flattening.
      auto uses = SymbolTable::getSymbolUses(function, getOperation());
      bool bound = uses && !uses->empty();
      if (uses)
        for (auto use : *uses) {
          auto call = dyn_cast<sim::DPICallOp>(use.getUser());
          auto owner =
              call ? call->getParentOfType<func::FuncOp>() : func::FuncOp{};
          bound &= owner && owner->hasAttr("chipyard.lowered_dpi_export");
        }
      if (!bound && !scoped)
        return function.emitError(
            "context DPI requires native declaration-scope binding");
    }
    if (auto flag = decl.getAs<BoolAttr>("task"); flag && flag.getValue()) {
      auto original =
          function->getAttrOfType<ArrayAttr>("chipyard.original_task_abi");
      auto formals = function.getDpiFunctionType().getArguments();
      unsigned offset = scoped ? 1 : 0;
      bool valid =
          original && function->hasAttr("chipyard.native_task_status") &&
          abi.size() == original.size() + offset + 1 && !formals.empty();
      auto status = abi.empty() ? DictionaryAttr{}
                                : dyn_cast<DictionaryAttr>(abi[abi.size() - 1]);
      auto transport =
          status ? status.getAs<BoolAttr>("native_task_status") : BoolAttr{};
      auto kind = status ? status.getAs<StringAttr>("kind") : StringAttr{};
      auto width = status ? status.getAs<IntegerAttr>("width") : IntegerAttr{};
      valid &= transport && transport.getValue() && kind &&
               kind.getValue() == "int" && width && width.getInt() == 32 &&
               !formals.empty() &&
               formals.back().dir == sim::DPIDirection::Return &&
               formals.back().type.isInteger(32);
      if (valid)
        for (unsigned i = 0; i < original.size(); ++i)
          valid &=
              originalABI(cast<DictionaryAttr>(abi[i + offset])) == original[i];
      if (!valid)
        return function.emitError(
            "DPI task disable/suspension ABI requires a native binding");
    }
    auto *ctx = &getContext();
    for (auto [formal, attribute] :
         llvm::zip(function.getDpiFunctionType().getArguments(), abi))
      if (failed(describeArgument(function, formal, attribute, arguments,
                                  scoped && arguments.empty())))
        return failure();
    return success();
  }
  Value constant(OpBuilder &b, Location loc, unsigned width, uint64_t value) {
    return arith::ConstantOp::create(
        b, loc, b.getIntegerType(width),
        b.getIntegerAttr(b.getIntegerType(width), APInt(width, value)));
  }
  Value castBits(OpBuilder &b, Location loc, Type target, Value value) {
    if (target == value.getType())
      return value;
    if (isa<FloatType>(target) || isa<FloatType>(value.getType()))
      return arith::BitcastOp::create(b, loc, target, value);
    return hw::BitcastOp::create(b, loc, target, value);
  }
  Value resize(OpBuilder &b, Location loc, Value value, unsigned width) {
    unsigned old = cast<IntegerType>(value.getType()).getWidth();
    if (old == width)
      return value;
    if (old < width)
      return arith::ExtUIOp::create(b, loc, b.getIntegerType(width), value);
    return arith::TruncIOp::create(b, loc, b.getIntegerType(width), value);
  }
  Value wordPtr(OpBuilder &b, Location loc, Value base, unsigned word) {
    return LLVM::GEPOp::create(
        b, loc, LLVM::LLVMPointerType::get(&getContext()), b.getI32Type(), base,
        ArrayRef<LLVM::GEPArg>{int32_t(word)});
  }
  Value bytePtr(OpBuilder &b, Location loc, Value base, uint64_t byte) {
    return LLVM::GEPOp::create(
        b, loc, LLVM::LLVMPointerType::get(&getContext()), b.getI8Type(), base,
        ValueRange{constant(b, loc, 64, byte)});
  }
  Value allocate(OpBuilder &b, Location loc, Type element, unsigned count = 1) {
    return LLVM::AllocaOp::create(b, loc,
                                  LLVM::LLVMPointerType::get(&getContext()),
                                  element, constant(b, loc, 64, count));
  }
  void pack(OpBuilder &b, Location loc, const Argument &arg, Value value,
            Value buffer) {
    if (arg.repr == Array) {
      auto &child = *arg.element;
      for (unsigned i = 0; i < arg.count; i++) {
        unsigned offset = (arg.count - 1 - i) * child.width;
        Value part = value;
        if (offset)
          part = arith::ShRUIOp::create(b, loc, part,
                                        constant(b, loc, arg.width, offset));
        part = resize(b, loc, part, child.width);
        pack(b, loc, child, part,
             bytePtr(b, loc, buffer, uint64_t(i) * child.cBytes()));
      }
      return;
    }
    if (!arg.vector()) {
      LLVM::StoreOp::create(b, loc, encodeScalar(b, loc, arg, value), buffer);
      return;
    }
    for (unsigned word = 0; word < (arg.width + 31) / 32; ++word) {
      Value part = value;
      if (word)
        part = arith::ShRUIOp::create(b, loc, part,
                                      constant(b, loc, arg.width, word * 32));
      part = resize(b, loc, part, 32);
      unsigned stride = arg.repr == Logics ? 2 : 1;
      LLVM::StoreOp::create(b, loc, part,
                            wordPtr(b, loc, buffer, stride * word));
      if (arg.repr == Logics)
        LLVM::StoreOp::create(b, loc, constant(b, loc, 32, 0),
                              wordPtr(b, loc, buffer, stride * word + 1));
    }
  }
  Value unpack(OpBuilder &b, Location loc, const Argument &arg, Value buffer) {
    if (arg.repr == Array) {
      auto &child = *arg.element;
      Value value = constant(b, loc, arg.width, 0);
      for (unsigned i = 0; i < arg.count; i++) {
        Value part =
            unpack(b, loc, child,
                   bytePtr(b, loc, buffer, uint64_t(i) * child.cBytes()));
        part = resize(b, loc, part, arg.width);
        unsigned offset = (arg.count - 1 - i) * child.width;
        if (offset)
          part = arith::ShLIOp::create(b, loc, part,
                                       constant(b, loc, arg.width, offset));
        value = arith::OrIOp::create(b, loc, value, part);
      }
      return value;
    }
    if (!arg.vector())
      return decodeScalar(b, loc, arg,
                          LLVM::LoadOp::create(b, loc, arg.scalar, buffer));
    Value value = constant(b, loc, arg.width, 0);
    for (unsigned word = 0; word < (arg.width + 31) / 32; ++word) {
      unsigned stride = arg.repr == Logics ? 2 : 1;
      Value part = LLVM::LoadOp::create(b, loc, b.getI32Type(),
                                        wordPtr(b, loc, buffer, stride * word));
      if (arg.repr == Logics) {
        Value unknown = LLVM::LoadOp::create(
            b, loc, b.getI32Type(), wordPtr(b, loc, buffer, stride * word + 1));
        Value known = arith::XOrIOp::create(b, loc, unknown,
                                            constant(b, loc, 32, UINT32_MAX));
        part = arith::AndIOp::create(b, loc, part, known);
      }
      part = resize(b, loc, part, arg.width);
      if (word)
        part = arith::ShLIOp::create(b, loc, part,
                                     constant(b, loc, arg.width, word * 32));
      value = arith::OrIOp::create(b, loc, value, part);
    }
    return value;
  }
  Value encodeScalar(OpBuilder &b, Location loc, const Argument &arg,
                     Value value) {
    if (arg.repr == Handle)
      return LLVM::IntToPtrOp::create(b, loc, arg.scalar, value);
    if (arg.repr == String)
      return func::CallOp::create(b, loc, stringData, ValueRange{value})
          .getResult(0);
    if (arg.repr == Real)
      return arith::BitcastOp::create(b, loc, arg.scalar, value);
    return resize(b, loc, value, cast<IntegerType>(arg.scalar).getWidth());
  }
  Value decodeScalar(OpBuilder &b, Location loc, const Argument &arg,
                     Value value) {
    if (arg.repr == Handle)
      return LLVM::PtrToIntOp::create(b, loc, arg.logical, value);
    if (arg.repr == String)
      return func::CallOp::create(b, loc, stringCopy, ValueRange{value})
          .getResult(0);
    if (arg.repr == Real)
      return arith::BitcastOp::create(b, loc, arg.logical, value);
    if (arg.repr == Logic) {
      Value valid = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult,
                                          value, constant(b, loc, 8, 2));
      value = arith::AndIOp::create(b, loc, resize(b, loc, value, 1), valid);
    }
    return resize(b, loc, value, arg.width);
  }
  LogicalResult extensions(func::FuncOp external, ArrayRef<Argument> args) {
    auto set = [&](unsigned index, bool result,
                   const Argument &arg) -> LogicalResult {
      if (arg.vector() || arg.byref() || arg.repr == Real ||
          arg.repr == String || arg.repr == Handle ||
          cast<IntegerType>(arg.scalar).getWidth() >= 32)
        return success();
      auto flag = arg.repr == Atom && arg.isSigned
                      ? LLVM::LLVMDialect::getSExtAttrName()
                      : LLVM::LLVMDialect::getZExtAttrName();
      auto opposite = arg.repr == Atom && arg.isSigned
                          ? LLVM::LLVMDialect::getZExtAttrName()
                          : LLVM::LLVMDialect::getSExtAttrName();
      auto old = result ? external.getResultAttr(index, opposite)
                        : external.getArgAttr(index, opposite);
      if (old)
        return external.emitError(
            "conflicting narrow-integer DPI C ABI extension");
      if (result)
        external.setResultAttr(index, flag, UnitAttr::get(&getContext()));
      else
        external.setArgAttr(index, flag, UnitAttr::get(&getContext()));
      return success();
    };
    unsigned index = 0;
    for (auto &arg : args) {
      if (arg.contextScope)
        continue;
      if (arg.original.dir == sim::DPIDirection::Return) {
        if (failed(set(0, true, arg)))
          return failure();
      } else if (failed(set(index++, false, arg)))
        return failure();
    }
    return success();
  }
  void runOnOperation() override {
    ModuleOp module = getOperation();
    circt::Namespace names;
    names.add(module);
    for (auto fn : module.getOps<sim::DPIFuncOp>())
      names.add(fn.getVerilogName().value_or(fn.getSymName()));
    SmallVector<sim::DPIFuncOp> functions;
    if (validateOnly) {
      bool unresolved = false;
      module.walk([&](sim::DPICallOp call) {
        if (!SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
                call, call.getCalleeAttr())) {
          call.emitError("native DPI call requires its imported declaration");
          unresolved = true;
        }
      });
      if (unresolved) {
        signalPassFailure();
        return;
      }
    }
    for (auto fn : module.getOps<sim::DPIFuncOp>()) {
      if (!fn->hasAttr("chipyard.dpi_abi")) {
        if (validateOnly) {
          fn.emitError(
              "native DPI requires original SystemVerilog ABI metadata");
          signalPassFailure();
          return;
        }
        continue;
      }
      if (!fn->hasAttr("chipyard.native_dpi_bridge"))
        functions.push_back(fn);
    }
    for (auto fn : functions) {
      SmallVector<Argument> args;
      if (failed(describe(fn, args))) {
        signalPassFailure();
        return;
      }
      if (validateOnly)
        continue;
      auto loc = fn.getLoc();
      OpBuilder b(fn);
      if (llvm::any_of(args,
                       [&](const Argument &arg) { return usesString(arg); })) {
        auto ensure = [&](StringRef name, FunctionType type) -> func::FuncOp {
          auto function = module.lookupSymbol<func::FuncOp>(name);
          if (function && function.getFunctionType() != type) {
            fn.emitError("conflicting native DPI string helper: ") << name;
            return {};
          }
          if (!function) {
            OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(module.getBody());
            function = func::FuncOp::create(b, loc, name, type);
            function.setPrivate();
          }
          return function;
        };
        auto pointer = LLVM::LLVMPointerType::get(&getContext());
        stringData = ensure("chipyard_arc_string_c_str",
                            b.getFunctionType({b.getI64Type()}, {pointer}));
        stringCopy = ensure("chipyard_arc_string_from_c",
                            b.getFunctionType({pointer}, {b.getI64Type()}));
        if (!stringData || !stringCopy) {
          signalPassFailure();
          return;
        }
      }
      if (llvm::any_of(
              args, [](const Argument &arg) { return bool(arg.openActual); })) {
        auto ensure = [&](StringRef name, FunctionType type) -> func::FuncOp {
          auto function = module.lookupSymbol<func::FuncOp>(name);
          if (function && function.getFunctionType() != type) {
            fn.emitError("conflicting native open-array helper: ") << name;
            return {};
          }
          if (!function) {
            OpBuilder::InsertionGuard guard(b);
            b.setInsertionPointToStart(module.getBody());
            function = func::FuncOp::create(b, loc, name, type);
            function.setPrivate();
          }
          return function;
        };
        auto pointer = LLVM::LLVMPointerType::get(&getContext());
        auto i64 = b.getI64Type();
        openCreate =
            ensure("chipyard_arc_open_array_create",
                   b.getFunctionType(
                       {pointer, i64, i64, i64, pointer, i64, i64}, {pointer}));
        openDelete = ensure("chipyard_arc_open_array_delete",
                            b.getFunctionType({pointer}, {}));
        if (!openCreate || !openDelete) {
          signalPassFailure();
          return;
        }
      }
      auto cName =
          b.getStringAttr(fn.getVerilogName().value_or(fn.getSymName()));
      if (auto target =
              fn->getAttrOfType<FlatSymbolRefAttr>("chipyard.native_target"))
        cName = target.getAttr();
      if (auto collision = module.lookupSymbol<sim::DPIFuncOp>(cName)) {
        auto oldCName = b.getStringAttr(
            collision.getVerilogName().value_or(collision.getSymName()));
        auto renamed = b.getStringAttr(names.newName("chipyard_dpi_import"));
        if (failed(SymbolTable::replaceAllSymbolUses(collision, renamed,
                                                     module))) {
          signalPassFailure();
          return;
        }
        collision.setSymNameAttr(renamed);
        collision.setVerilogNameAttr(oldCName);
      }
      SmallVector<Type> cInputs, cResults, bridgeInputs, bridgeResults;
      auto ptr = LLVM::LLVMPointerType::get(&getContext());
      SmallVector<sim::DPIArgument> normalized;
      for (auto &arg : args) {
        normalized.push_back(
            {arg.original.name, arg.logical, arg.original.dir});
        if (arg.contextScope) {
          bridgeInputs.push_back(arg.logical);
          continue;
        }
        if (arg.original.dir == sim::DPIDirection::Return) {
          cResults.push_back(arg.scalar);
          bridgeResults.push_back(arg.logical);
        } else {
          cInputs.push_back(arg.byref() || arg.vector() ? Type(ptr)
                                                        : arg.scalar);
          bridgeInputs.push_back(arg.byref() ? Type(ptr) : arg.logical);
        }
      }
      auto cType = b.getFunctionType(cInputs, cResults);
      auto external = module.lookupSymbol<func::FuncOp>(cName);
      if (external && external.getFunctionType() != cType) {
        fn.emitError("conflicting native C ABI for DPI symbol ") << cName;
        signalPassFailure();
        return;
      }
      if (!external) {
        external = func::FuncOp::create(b, loc, cName.getValue(), cType);
        external.setPrivate();
      }
      if (failed(extensions(external, args))) {
        signalPassFailure();
        return;
      }
      auto prototype = cPrototype(b, args);
      if (auto previous =
              external->getAttrOfType<ArrayAttr>("chipyard.dpi_c_prototype");
          previous && previous != prototype) {
        fn.emitError(
            "conflicting native DPI C pointer element prototype for symbol ")
            << cName;
        signalPassFailure();
        return;
      }
      external->setAttr("chipyard.dpi_c_prototype", prototype);
      auto bridge =
          func::FuncOp::create(b, loc, names.newName("chipyard_dpi_bridge"),
                               b.getFunctionType(bridgeInputs, bridgeResults));
      bridge.setPrivate();
      auto *entry = bridge.addEntryBlock();
      b.setInsertionPointToStart(entry);
      SmallVector<Value> actual, openHandles;
      SmallVector<std::pair<Argument, std::pair<Value, Value>>> outputs;
      unsigned index = 0;
      Value declarationScope;
      for (auto &arg : args) {
        if (arg.original.dir == sim::DPIDirection::Return)
          continue;
        Value input = entry->getArgument(index++);
        if (arg.contextScope) {
          declarationScope = input;
          continue;
        }
        Value logical =
            arg.byref()
                ? Value(LLVM::LoadOp::create(b, loc, arg.logical, input))
                : input;
        if (arg.vector()) {
          auto buffer =
              arg.repr == Array
                  ? allocate(b, loc, b.getI64Type(), (arg.cBytes() + 7) / 8)
                  : allocate(b, loc, b.getI32Type(),
                             ((arg.width + 31) / 32) *
                                 (arg.repr == Logics ? 2 : 1));
          pack(b, loc, arg, logical, buffer);
          if (arg.openActual) {
            std::string shape;
            auto info = arg.openActual;
            auto *leaf = &arg;
            while (leaf->repr == Array) {
              if (!shape.empty())
                shape += ",";
              shape +=
                  std::to_string(info.getAs<IntegerAttr>("left").getInt()) +
                  ":" +
                  std::to_string(info.getAs<IntegerAttr>("right").getInt());
              info = info.getAs<DictionaryAttr>("element");
              leaf = leaf->element.get();
            }
            shape.push_back(0);
            LLVM::GlobalOp global;
            {
              OpBuilder::InsertionGuard guard(b);
              b.setInsertionPointToStart(module.getBody());
              global = LLVM::GlobalOp::create(
                  b, loc, LLVM::LLVMArrayType::get(b.getI8Type(), shape.size()),
                  true, LLVM::Linkage::Internal,
                  names.newName("__chipyard_open_shape"),
                  b.getStringAttr(shape), 0);
            }
            Value address = LLVM::AddressOfOp::create(b, loc, global);
            unsigned flags = leaf->vector() ? 1 : leaf->repr == Logic ? 4 : 0;
            if (leaf->repr == Logics)
              flags |= 2;
            int64_t left = leaf->width - 1, right = 0;
            auto l = info.getAs<IntegerAttr>("left"),
                 r = info.getAs<IntegerAttr>("right");
            if (l && r && llvm::isInt<32>(l.getInt()) &&
                llvm::isInt<32>(r.getInt()) &&
                uint64_t(std::abs(l.getInt() - r.getInt()) + 1) ==
                    leaf->width) {
              left = l.getInt();
              right = r.getInt();
            }
            Value handle =
                func::CallOp::create(
                    b, loc, openCreate,
                    ValueRange{buffer, constant(b, loc, 64, leaf->cBytes()),
                               constant(b, loc, 64, leaf->width),
                               constant(b, loc, 64, flags), address,
                               constant(b, loc, 64, left),
                               constant(b, loc, 64, right)})
                    .getResult(0);
            actual.push_back(handle);
            openHandles.push_back(handle);
          } else
            actual.push_back(buffer);
          if (arg.byref())
            outputs.push_back({arg, {input, buffer}});
        } else if (arg.byref()) {
          auto buffer = allocate(b, loc, arg.scalar);
          LLVM::StoreOp::create(b, loc, encodeScalar(b, loc, arg, logical),
                                buffer);
          actual.push_back(buffer);
          outputs.push_back({arg, {input, buffer}});
        } else
          actual.push_back(encodeScalar(b, loc, arg, logical));
      }
      // A context import may select another scope while calling back into SV.
      // Returning to this SV caller restores its original scope before any
      // following context import executes.
      func::FuncOp scopeGetter, scopeSetter;
      auto declaration = fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
      if (auto context = declaration.getAs<BoolAttr>("context");
          context && context.getValue()) {
        auto getterType = b.getFunctionType({}, {ptr});
        auto setterType = b.getFunctionType({ptr}, {ptr});
        OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToStart(module.getBody());
        scopeGetter = module.lookupSymbol<func::FuncOp>("svGetScope");
        scopeSetter = module.lookupSymbol<func::FuncOp>("svSetScope");
        if ((scopeGetter && scopeGetter.getFunctionType() != getterType) ||
            (scopeSetter && scopeSetter.getFunctionType() != setterType)) {
          fn.emitError("conflicting native DPI scope API declaration");
          signalPassFailure();
          return;
        }
        if (!scopeGetter) {
          scopeGetter = func::FuncOp::create(b, loc, "svGetScope", getterType);
          scopeGetter.setPrivate();
        }
        if (!scopeSetter) {
          scopeSetter = func::FuncOp::create(b, loc, "svSetScope", setterType);
          scopeSetter.setPrivate();
        }
      }
      Value savedScope;
      if (scopeGetter)
        savedScope = func::CallOp::create(b, loc, scopeGetter).getResult(0);
      if (declarationScope) {
        auto pointerScope =
            LLVM::IntToPtrOp::create(b, loc, ptr, declarationScope);
        func::CallOp::create(b, loc, scopeSetter, ValueRange{pointerScope});
      }
      auto call = func::CallOp::create(b, loc, external, actual);
      if (scopeSetter)
        func::CallOp::create(b, loc, scopeSetter, ValueRange{savedScope});
      for (auto handle : openHandles)
        func::CallOp::create(b, loc, openDelete, ValueRange{handle});
      for (auto &out : outputs) {
        auto &arg = out.first;
        auto [destination, buffer] = out.second;
        Value decoded =
            arg.vector() ? unpack(b, loc, arg, buffer)
                         : decodeScalar(b, loc, arg,
                                        LLVM::LoadOp::create(b, loc, arg.scalar,
                                                             buffer));
        LLVM::StoreOp::create(b, loc, decoded, destination);
      }
      SmallVector<Value> returned;
      for (auto &arg : args)
        if (arg.original.dir == sim::DPIDirection::Return)
          returned.push_back(decodeScalar(b, loc, arg, call.getResult(0)));
      func::ReturnOp::create(b, loc, returned);
      auto newType = sim::DPIFunctionType::get(&getContext(), normalized);
      module.walk([&](sim::DPICallOp site) {
        if (SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
                site, site.getCalleeAttr()) != fn)
          return;
        OpBuilder sb(site);
        SmallVector<Value> inputs;
        unsigned i = 0;
        for (auto &arg : args)
          if (sim::isCallOperandDir(arg.original.dir))
            inputs.push_back(castBits(sb, site.getLoc(), arg.logical,
                                      site.getInputs()[i++]));
        site.getInputsMutable().assign(inputs);
        sb.setInsertionPointAfter(site);
        for (auto [result, type] : llvm::zip(
                 site.getResults(), newType.getFunctionType().getResults())) {
          auto old = result.getType();
          if (old == type)
            continue;
          result.setType(type);
          auto decoded = castBits(sb, site.getLoc(), old, result);
          result.replaceUsesWithIf(decoded, [&](OpOperand &use) {
            return use.getOwner() != decoded.getDefiningOp();
          });
        }
      });
      fn.setDpiFunctionType(newType);
      fn.setVerilogNameAttr(bridge.getSymNameAttr());
      fn->setAttr("chipyard.native_dpi_bridge", FlatSymbolRefAttr::get(bridge));
    }
  }
};

// Independently imported RTL specializations can declare the same C function
// under different local symbols. The SDK otherwise creates an external C
// declaration for every import and fails with duplicate symbols. Coalesce only
// equivalent declarations; calls and their effects remain separate operations.
struct CoalesceDPI : PassWrapper<CoalesceDPI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CoalesceDPI)
  StringRef getArgument() const override { return "chipyard-coalesce-dpi"; }
  DictionaryAttr semanticAttrs(sim::DPIFuncOp function) {
    NamedAttrList attrs(function->getAttrs());
    for (auto name : {"sym_name", "sym_visibility", "dpi_function_type",
                      "argument_locs", "verilogName", "chipyard.dpi_decl"})
      attrs.erase(name);
    // Declaration scopes and source spellings may differ between instances,
    // but context, purity and task completion change the call contract.
    if (auto declaration =
            function->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl")) {
      NamedAttrList qualifiers;
      for (auto name : {"context", "pure", "task"}) {
        auto value = declaration.getAs<BoolAttr>(name);
        qualifiers.set(name,
                       value ? value : BoolAttr::get(&getContext(), false));
      }
      attrs.set("chipyard.dpi_qualifiers",
                qualifiers.getDictionary(&getContext()));
    }
    return attrs.getDictionary(&getContext());
  }
  bool compatible(sim::DPIFuncOp left, sim::DPIFuncOp right,
                  bool ignoreQualifiers = false) {
    auto a = left.getDpiFunctionType().getArguments();
    auto b = right.getDpiFunctionType().getArguments();
    NamedAttrList leftAttrs(semanticAttrs(left)),
        rightAttrs(semanticAttrs(right));
    if (ignoreQualifiers) {
      leftAttrs.erase("chipyard.dpi_qualifiers");
      rightAttrs.erase("chipyard.dpi_qualifiers");
    }
    if (a.size() != b.size() || leftAttrs.getDictionary(&getContext()) !=
                                    rightAttrs.getDictionary(&getContext()))
      return false;
    // Formal names and source locations do not change the C prototype. The
    // absolute argument order, types, and directions do: an out parameter and
    // a return value can have the same MLIR call shape but different C ABIs.
    for (auto [lhs, rhs] : llvm::zip(a, b))
      if (lhs.type != rhs.type || lhs.dir != rhs.dir)
        return false;
    return true;
  }
  void runOnOperation() override {
    llvm::StringMap<SmallVector<sim::DPIFuncOp>> canonical;
    SmallVector<std::pair<sim::DPIFuncOp, sim::DPIFuncOp>> duplicates;
    getOperation().walk([&](sim::DPICallOp call) {
      if (call->hasAttr("chipyard.dpi_decl"))
        return;
      if (auto function = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
              call, call.getCalleeAttr()))
        if (auto declaration = function->getAttr("chipyard.dpi_decl"))
          call->setAttr("chipyard.dpi_decl", declaration);
    });
    for (auto function : getOperation().getOps<sim::DPIFuncOp>()) {
      auto name = function.getVerilogName().value_or(function.getSymName());
      auto &variants = canonical[name];
      sim::DPIFuncOp equivalent;
      for (auto previous : variants) {
        if (compatible(previous, function)) {
          equivalent = previous;
          break;
        }
        // The native ABI bridge can share the external C prototype while
        // keeping separate wrappers for differing import qualifiers.
        if (previous->hasAttr("chipyard.dpi_decl") &&
            function->hasAttr("chipyard.dpi_decl") &&
            compatible(previous, function, true))
          continue;
        auto error = function.emitError()
                     << "incompatible DPI declarations for C symbol '" << name
                     << "'";
        error.attachNote(previous.getLoc()) << "previous declaration is here";
        signalPassFailure();
        return;
      }
      if (equivalent)
        duplicates.emplace_back(function, equivalent);
      else
        variants.push_back(function);
    }
    for (auto [duplicate, original] : duplicates) {
      if (failed(SymbolTable::replaceAllSymbolUses(
              duplicate, original.getSymNameAttr(), getOperation()))) {
        duplicate.emitError("cannot resolve all uses of duplicate DPI import");
        signalPassFailure();
        return;
      }
      duplicate.erase();
    }
    if (!duplicates.empty())
      llvm::errs() << "Coalesced " << duplicates.size()
                   << " compatible DPI declarations\n";
  }
};

// Materialize the native two-state initial value of each output formal before
// the SDK creates its C-ABI wrapper. Output and inout use the same C pointer
// ABI; giving only original outputs a constant input makes the SDK initialize
// their allocation on every call. Real inouts keep their caller-provided value
// and refs keep their original pointer. No retained actual-output dependency is
// introduced. Coalesce declarations first, while original directions still
// distinguish incompatible imports of the same C symbol.
struct SeedDPIOutputs : PassWrapper<SeedDPIOutputs, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SeedDPIOutputs)
  StringRef getArgument() const override { return "chipyard-seed-dpi-outputs"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, sim::SimDialect>();
  }
  void runOnOperation() override {
    DenseMap<Operation *, SmallVector<sim::DPIArgument>> original;
    for (auto function : getOperation().getOps<sim::DPIFuncOp>()) {
      auto arguments = function.getDpiFunctionType().getArguments();
      if (!llvm::any_of(arguments,
                        [](const sim::DPIArgument &arg) {
                          return arg.dir == sim::DPIDirection::Output;
                        }) ||
          llvm::any_of(arguments, [](const sim::DPIArgument &arg) {
            return arg.dir == sim::DPIDirection::Output &&
                   !isa<IntegerType>(arg.type);
          }))
        continue;
      original[function].append(arguments.begin(), arguments.end());
    }
    unsigned count = 0;
    getOperation().walk([&](sim::DPICallOp call) {
      auto function = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
          call, call.getCalleeAttr());
      auto entry = original.find(function);
      if (entry == original.end())
        return;
      OpBuilder builder(call);
      SmallVector<Value> inputs;
      unsigned index = 0;
      for (auto arg : entry->second) {
        if (arg.dir == sim::DPIDirection::Output) {
          auto type = cast<IntegerType>(arg.type);
          inputs.push_back(hw::ConstantOp::create(builder, call.getLoc(),
                                                  APInt(type.getWidth(), 0)));
          ++count;
        } else if (sim::isCallOperandDir(arg.dir)) {
          inputs.push_back(call.getInputs()[index++]);
        }
      }
      call.getInputsMutable().assign(inputs);
    });
    for (auto &[op, arguments] : original) {
      SmallVector<int32_t> outputs;
      for (auto [index, arg] : llvm::enumerate(arguments))
        if (arg.dir == sim::DPIDirection::Output) {
          outputs.push_back(index);
          arg.dir = sim::DPIDirection::InOut;
        }
      auto function = cast<sim::DPIFuncOp>(op);
      function->setAttr("chipyard.seeded_output_arguments",
                        DenseI32ArrayAttr::get(&getContext(), outputs));
      function.setDpiFunctionType(
          sim::DPIFunctionType::get(&getContext(), arguments));
    }
    if (count)
      llvm::errs() << "Seeded " << count << " DPI output arguments\n";
  }
};

// Arc conversion can outline immutable captures and from_immutable into
// transfer functions carrying !seq.immutable arguments or results. Expose these
// boundaries at their call sites before state lowering unwraps their values.
// Only calls carrying immutable types are expanded; ordinary arcs stay intact.
struct InlineImmutableArcs
    : PassWrapper<InlineImmutableArcs, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InlineImmutableArcs)
  StringRef getArgument() const override {
    return "chipyard-inline-immutable-arcs";
  }
  void runOnOperation() override {
    SmallVector<arc::CallOp> calls;
    auto collect = [&](arc::CallOp call) {
      auto immutable = [](Type type) { return isa<seq::ImmutableType>(type); };
      if (llvm::any_of(call.getOperandTypes(), immutable) ||
          llvm::any_of(call.getResultTypes(), immutable))
        calls.push_back(call);
    };
    getOperation().walk(collect);
    SymbolTableCollection symbols;
    for (unsigned index = 0; index < calls.size(); ++index) {
      auto call = calls[index];
      auto arc = symbols.lookupNearestSymbolFrom<arc::DefineOp>(
          call, call.getArcAttr());
      if (!arc || index >= 4096) {
        call.emitError(
            "cannot expose immutable arc arguments before state lowering");
        signalPassFailure();
        return;
      }
      IRMapping mapping;
      mapping.map(arc.getBodyBlock().getArguments(), call.getInputs());
      OpBuilder builder(call);
      for (auto &op : arc.getBodyBlock().without_terminator()) {
        auto *clone = builder.clone(op, mapping);
        clone->walk(collect);
      }
      SmallVector<Value> results;
      for (auto value : arc.getBodyBlock().getTerminator()->getOperands())
        results.push_back(mapping.lookupOrDefault(value));
      call.replaceAllUsesWith(results);
      call.erase();
    }
  }
};

// The SDK creates C-ABI wrapper functions for DPI declarations, but leaves
// procedural calls in the sim dialect. A coroutine already supplies their
// control flow and event gating; invoke the wrapper at that exact program
// point. Ordinary functions likewise supply a procedural call frame, including
// native string-literal getters called by compiled SV exports.
struct LowerProceduralDPI
    : PassWrapper<LowerProceduralDPI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerProceduralDPI)
  StringRef getArgument() const override {
    return "chipyard-lower-procedural-dpi";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<func::FuncDialect>();
  }
  void runOnOperation() override {
    // No symbol definitions are added, removed, or renamed by this pass.
    // Reuse scoped tables instead of scanning a large module at every call.
    SymbolTableCollection symbols;
    llvm::SmallPtrSet<Operation *, 32> annotatedWrappers;
    SmallVector<sim::DPICallOp> calls;
    getOperation().walk([&](sim::DPICallOp call) {
      // SDK wrappers use i1 for scalar logic/bit arguments. C++ bool and DPI
      // scalar logic consume a complete byte; the C ABI requires zero extension
      // rather than LLVM's unspecified high bits for an unannotated i1.
      auto wrapper = symbols.lookupNearestSymbolFrom<func::FuncOp>(
          call, call.getCalleeAttr());
      if (wrapper && annotatedWrappers.insert(wrapper.getOperation()).second)
        wrapper.walk([&](func::CallOp nested) {
          auto external = symbols.lookupNearestSymbolFrom<func::FuncOp>(
              nested, nested.getCalleeAttr());
          if (!external || !external.isExternal())
            return;
          auto unit = UnitAttr::get(&getContext());
          for (auto [index, type] :
               llvm::enumerate(external.getArgumentTypes()))
            if (type.isInteger(1))
              external.setArgAttr(index, LLVM::LLVMDialect::getZExtAttrName(),
                                  unit);
          for (auto [index, type] : llvm::enumerate(external.getResultTypes()))
            if (type.isInteger(1))
              external.setResultAttr(
                  index, LLVM::LLVMDialect::getZExtAttrName(), unit);
        });
      if (!call.getClock() && !call.getEnable() &&
          (call->getParentOfType<arc::CoroutineDefineOp>() ||
           call->getParentOfType<llhd::CoroutineOp>() ||
           call->getParentOfType<func::FuncOp>() ||
           call->getParentOfType<seq::InitialOp>()))
        calls.push_back(call);
    });
    for (auto call : calls) {
      auto callee = symbols.lookupNearestSymbolFrom<func::FuncOp>(
          call, call.getCalleeAttr());
      if (!callee)
        continue;
      ImplicitLocOpBuilder builder(call.getLoc(), call);
      auto direct = func::CallOp::create(builder, callee, call.getInputs());
      direct->setDiscardableAttrs(call->getDiscardableAttrDictionary());
      call.replaceAllUsesWith(direct.getResults());
      call.erase();
    }
  }
};

// The SDK folds real-valued formatters at compile time but cannot lower their
// dynamic counterparts. Split only print trees containing real formatters at
// their original execution point. Keep integer/literal fragments on the SDK
// formatter and pass reals through a fixed C ABI, using the same double and
// snprintf semantics as SimValueFormatter::formatConstant.
struct LowerRealFormats
    : PassWrapper<LowerRealFormats, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerRealFormats)
  StringRef getArgument() const override {
    return "chipyard-lower-real-formats";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, func::FuncDialect, sim::SimDialect,
                    LLVM::LLVMDialect>();
  }
  void runOnOperation() override {
    SmallVector<sim::PrintFormattedProcOp> prints;
    getOperation().walk(
        [&](sim::PrintFormattedProcOp op) { prints.push_back(op); });
    func::FuncOp callback, stringCallback;
    for (auto print : prints) {
      SmallVector<Value> pending{print.getInput()}, parts;
      bool supported = true, hasReal = false;
      while (!pending.empty()) {
        auto value = pending.pop_back_val();
        auto *op = value.getDefiningOp();
        if (auto concat = dyn_cast_or_null<sim::FormatStringConcatOp>(op)) {
          for (auto input : llvm::reverse(concat.getInputs()))
            pending.push_back(input);
          continue;
        }
        bool real = isa_and_nonnull<sim::FormatFloatOp, sim::FormatScientificOp,
                                    sim::FormatGeneralOp>(op);
        auto text = dyn_cast_or_null<sim::FormatStringOp>(op);
        bool string =
            text && isa_and_nonnull<sim::StringConstantOp, sim::IntToStringOp>(
                        text.getValue().getDefiningOp());
        if (!real && !string &&
            !isa_and_nonnull<sim::FormatLiteralOp, sim::FormatDecOp,
                             sim::FormatHexOp, sim::FormatOctOp,
                             sim::FormatBinOp, sim::FormatCharOp>(op)) {
          supported = false;
          break;
        }
        hasReal |= real || string;
        parts.push_back(value);
      }
      if (!supported || !hasReal)
        continue;
      OpBuilder builder(print);
      auto loc = print.getLoc();
      if (!callback) {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(getOperation().getBody());
        auto type = builder.getFunctionType(
            {builder.getF64Type(), builder.getI32Type(), builder.getI32Type(),
             builder.getI32Type(), builder.getI32Type(),
             sim::OutputStreamType::get(&getContext())},
            {});
        constexpr StringLiteral name = "chipyard_arc_format_real";
        callback = getOperation().lookupSymbol<func::FuncOp>(name);
        if (callback && callback.getFunctionType() != type) {
          callback.emitError(
              "native real formatter has a conflicting declaration");
          signalPassFailure();
          return;
        }
        if (!callback) {
          callback = func::FuncOp::create(builder, loc, name, type);
          callback.setPrivate();
        }
      }
      Value stream = print.getStream();
      if (!stream)
        stream = sim::StdoutStreamOp::create(builder, loc);
      for (auto part : parts) {
        auto *op = part.getDefiningOp();
        if (auto text = dyn_cast<sim::FormatStringOp>(op)) {
          auto field = text.getSpecifierWidth();
          int64_t width = field ? int64_t(*field) : -1;
          if (auto literal =
                  text.getValue().getDefiningOp<sim::StringConstantOp>()) {
            std::string bytes = literal.getLiteral().str();
            if (width > int64_t(bytes.size())) {
              std::string padding(size_t(width - bytes.size()),
                                  char(text.getPaddingChar()));
              bytes =
                  text.getIsLeftAligned() ? bytes + padding : padding + bytes;
            }
            auto value = sim::FormatLiteralOp::create(builder, loc, bytes);
            auto fragment =
                sim::PrintFormattedProcOp::create(builder, loc, value, stream);
            fragment->setDiscardableAttrs(
                print->getDiscardableAttrDictionary());
            continue;
          }
          auto value =
              text.getValue().getDefiningOp<sim::IntToStringOp>().getInput();
          auto bits = cast<IntegerType>(value.getType()).getWidth();
          auto pointer = LLVM::LLVMPointerType::get(&getContext());
          if (!stringCallback) {
            OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(getOperation().getBody());
            auto type = builder.getFunctionType(
                {pointer, builder.getI32Type(), builder.getI32Type(),
                 builder.getI32Type(), builder.getI32Type(),
                 sim::OutputStreamType::get(&getContext())},
                {});
            constexpr StringLiteral name = "chipyard_arc_format_packed_string";
            stringCallback = getOperation().lookupSymbol<func::FuncOp>(name);
            if (stringCallback && stringCallback.getFunctionType() != type) {
              stringCallback.emitError(
                  "native string formatter has a conflicting declaration");
              signalPassFailure();
              return;
            }
            if (!stringCallback) {
              stringCallback = func::FuncOp::create(builder, loc, name, type);
              stringCallback.setPrivate();
            }
          }
          Value data;
          if (bits) {
            auto one = arith::ConstantIntOp::create(builder, loc, 1, 64);
            data = LLVM::AllocaOp::create(builder, loc, pointer,
                                          value.getType(), one);
            LLVM::StoreOp::create(builder, loc, value, data);
          } else
            data = LLVM::ZeroOp::create(builder, loc, pointer);
          auto constant = [&](int64_t n) -> Value {
            return arith::ConstantIntOp::create(builder, loc, n, 32);
          };
          func::CallOp::create(builder, loc, stringCallback,
                               ValueRange{data, constant(bits), constant(width),
                                          constant(text.getIsLeftAligned()),
                                          constant(text.getPaddingChar()),
                                          stream});
          continue;
        }
        unsigned kind = 0;
        if (isa<sim::FormatFloatOp>(op))
          kind = 'f';
        else if (isa<sim::FormatScientificOp>(op))
          kind = 'e';
        else if (isa<sim::FormatGeneralOp>(op))
          kind = 'g';
        if (!kind) {
          auto fragment =
              sim::PrintFormattedProcOp::create(builder, loc, part, stream);
          fragment->setDiscardableAttrs(print->getDiscardableAttrDictionary());
          continue;
        }
        auto field = op->getAttrOfType<IntegerAttr>("fieldWidth");
        auto precision = op->getAttrOfType<IntegerAttr>("fracDigits");
        auto left = op->getAttrOfType<BoolAttr>("isLeftAligned");
        int64_t width = field ? field.getInt() : -1;
        int64_t digits = precision ? precision.getInt() : 6;
        if ((field && width < 0) || digits < 0) {
          op->emitError("real format width and precision must be nonnegative");
          signalPassFailure();
          return;
        }
        Value value = op->getOperand(0);
        unsigned bits = cast<FloatType>(value.getType()).getWidth();
        if (bits < 64)
          value = arith::ExtFOp::create(builder, loc, builder.getF64Type(),
                                        value, arith::FastMathFlagsAttr{});
        else if (bits > 64)
          value = arith::TruncFOp::create(builder, loc, builder.getF64Type(),
                                          value);
        auto constant = [&](int64_t n) -> Value {
          return arith::ConstantIntOp::create(builder, loc, n, 32);
        };
        func::CallOp::create(builder, loc, callback,
                             ValueRange{value, constant(width),
                                        constant(digits),
                                        constant(left && left.getValue()),
                                        constant(kind), stream});
      }
      print.erase();
    }
  }
};

// The SDK leaves SV file descriptors and channels untranslated. Resolve the
// filename before dynamic strings can escape into LLVM, retaining file-open
// side effects even if no subsequent print consumes the descriptor.
struct LowerNativeFiles
    : PassWrapper<LowerNativeFiles, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerNativeFiles)
  StringRef getArgument() const override {
    return "chipyard-lower-native-files";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, func::FuncDialect, sim::SimDialect,
                    LLVM::LLVMDialect>();
  }
  func::FuncOp callback(OpBuilder &b, Location loc, StringRef name,
                        TypeRange inputs, TypeRange results) {
    auto type = b.getFunctionType(inputs, results);
    auto fn = getOperation().lookupSymbol<func::FuncOp>(name);
    if (fn && fn.getFunctionType() != type) {
      fn.emitError("conflicting native file callback declaration");
      signalPassFailure();
      return {};
    }
    if (!fn) {
      OpBuilder::InsertionGuard guard(b);
      b.setInsertionPointToStart(getOperation().getBody());
      fn = func::FuncOp::create(b, loc, name, type);
      fn.setPrivate();
    }
    return fn;
  }
  void runOnOperation() override {
    SmallVector<Operation *> operations;
    getOperation().walk([&](Operation *op) {
      if (isa<sim::SVFOpenOp, sim::SVFCloseOp, sim::SVChannelToOutputStreamOp,
              sim::SVFFlushAllOp>(op))
        operations.push_back(op);
    });
    circt::Namespace names;
    names.add(getOperation());
    for (auto *operation : operations) {
      OpBuilder b(operation);
      auto loc = operation->getLoc();
      auto i32 = b.getI32Type();
      if (auto open = dyn_cast<sim::SVFOpenOp>(operation)) {
        auto literal =
            open.getFileName().getDefiningOp<sim::StringConstantOp>();
        if (!literal) {
          open.emitError(
              "native file open currently requires a constant filename");
          signalPassFailure();
          return;
        }
        auto pointer = LLVM::LLVMPointerType::get(&getContext());
        auto fn =
            callback(b, loc, "chipyard_arc_sv_fopen",
                     TypeRange{pointer, b.getI64Type(), i32}, TypeRange{i32});
        if (!fn)
          return;
        auto filename = literal.getLiteral();
        LLVM::GlobalOp global;
        {
          OpBuilder::InsertionGuard guard(b);
          b.setInsertionPointToStart(getOperation().getBody());
          std::string bytes = filename.str();
          bytes.push_back(0);
          global = LLVM::GlobalOp::create(
              b, loc, LLVM::LLVMArrayType::get(b.getI8Type(), bytes.size()),
              true, LLVM::Linkage::Internal,
              names.newName("__chipyard_filename"), b.getStringAttr(bytes), 0);
        }
        auto address = LLVM::AddressOfOp::create(b, loc, global);
        auto size = arith::ConstantIntOp::create(b, loc, filename.size(), 64);
        auto mode = open.getMode();
        auto modeValue = arith::ConstantIntOp::create(
            b, loc, mode ? int64_t(*mode) : -1, 32);
        auto call = func::CallOp::create(b, loc, fn,
                                         ValueRange{address, size, modeValue});
        open.replaceAllUsesWith(call.getResult(0));
        open.erase();
      } else if (auto close = dyn_cast<sim::SVFCloseOp>(operation)) {
        auto fn =
            callback(b, loc, "chipyard_arc_sv_fclose", TypeRange{i32}, {});
        if (!fn)
          return;
        func::CallOp::create(b, loc, fn, ValueRange{close.getFd()});
        close.erase();
      } else if (auto stream =
                     dyn_cast<sim::SVChannelToOutputStreamOp>(operation)) {
        auto fn =
            callback(b, loc, "chipyard_arc_sv_stream", TypeRange{i32},
                     TypeRange{sim::OutputStreamType::get(&getContext())});
        if (!fn)
          return;
        auto call =
            func::CallOp::create(b, loc, fn, ValueRange{stream.getChannel()});
        stream.replaceAllUsesWith(call.getResult(0));
        stream.erase();
      } else {
        auto fn = callback(b, loc, "chipyard_arc_sv_fflush_all", {}, {});
        if (!fn)
          return;
        func::CallOp::create(b, loc, fn, ValueRange{});
        operation->erase();
      }
    }
  }
};

// Func-to-LLVM conversion in the matching SDK drops call-site argument/result
// attributes. Restore the Boolean ABI from the annotated DPI declarations after
// conversion, without applying C conventions to internal model functions.
struct FinalizeDPIABI : PassWrapper<FinalizeDPIABI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FinalizeDPIABI)
  StringRef getArgument() const override { return "chipyard-finalize-dpi-abi"; }
  void runOnOperation() override {
    auto zext = LLVM::LLVMDialect::getZExtAttrName();
    auto sext = LLVM::LLVMDialect::getSExtAttrName();
    auto extend = [&](TypeRange types, ArrayAttr current, ArrayAttr formal) {
      if (!formal)
        return current;
      SmallVector<Attribute> updated;
      bool changed = false;
      for (auto [index, type] : llvm::enumerate(types)) {
        NamedAttrList attrs(current ? cast<DictionaryAttr>(current[index])
                                    : DictionaryAttr{});
        if (auto integer = dyn_cast<IntegerType>(type);
            integer && integer.getWidth() < 32 && index < formal.size())
          for (auto flag : {zext, sext})
            if (auto declared = cast<DictionaryAttr>(formal[index]).get(flag)) {
              attrs.set(flag, declared);
              changed = true;
            }
        updated.push_back(attrs.getDictionary(&getContext()));
      }
      return changed ? ArrayAttr::get(&getContext(), updated) : current;
    };
    // This pass changes call attributes only; symbol definitions stay stable.
    SymbolTableCollection symbols;
    getOperation().walk([&](LLVM::CallOp call) {
      auto symbol = call.getCalleeAttr();
      if (!symbol)
        return;
      auto callee =
          symbols.lookupNearestSymbolFrom<LLVM::LLVMFuncOp>(call, symbol);
      if (!callee || !callee.isExternal())
        return;
      if (auto attrs = extend(call.getArgOperands().getTypes(),
                              call.getArgAttrsAttr(), callee.getArgAttrsAttr()))
        call.setArgAttrsAttr(attrs);
      if (auto attrs = extend(call.getResultTypes(), call.getResAttrsAttr(),
                              callee.getResAttrsAttr()))
        call.setResAttrsAttr(attrs);
    });
  }
};

struct OutlineModel : PassWrapper<OutlineModel, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(OutlineModel)
  OutlineModel() = default;
  OutlineModel(const OutlineModel &other) : PassWrapper(other) {}
  Option<unsigned> maxOps{*this, "max-ops", llvm::cl::init(16384)};
  Option<unsigned> maxPorts{*this, "max-ports", llvm::cl::init(16384)};
  StringRef getArgument() const override { return "chipyard-outline-model"; }
  StringRef getDescription() const override {
    return "Outline allocated Arc model procedures into bounded functions";
  }
  ModuleOp module;
  unsigned count = 0;
  std::string modelName;
  bool address(Operation *op) {
    return op && op->getAttr("offset") &&
           isa<arc::StorageGetOp, arc::RootInputOp, arc::RootOutputOp,
               arc::AllocStateOp, arc::AllocMemoryOp, arc::AllocStorageOp>(op);
  }
  bool rematerialize(Operation *op) {
    return op && (address(op) ||
                  (op->getNumOperands() == 0 && op->getNumRegions() == 0 &&
                   (op->hasTrait<OpTrait::ConstantLike>() ||
                    op->getName().getStringRef() == "sim.stdout_stream" ||
                    op->getName().getStringRef() == "sim.stderr_stream")));
  }
  bool frameOwner(Operation *op) {
    bool found = false;
    op->walk([&](Operation *nested) {
      auto name = nested->getName().getStringRef();
      found |= name == "arc.arrayref.alloc" || name == "llvm.alloca" ||
               name == "memref.alloca";
    });
    return found;
  }
  unsigned weight(Operation *op) {
    unsigned n = 0;
    op->walk([&](Operation *) { ++n; });
    return n;
  }
  void outline(SmallVector<Operation *> &ops) {
    if (ops.empty())
      return;
    DenseSet<Operation *> members(ops.begin(), ops.end());
    auto inside = [&](Value value) {
      Operation *owner = value.getDefiningOp();
      if (!owner)
        owner = value.getParentBlock()->getParentOp();
      while (owner) {
        if (members.contains(owner))
          return true;
        owner = owner->getParentOp();
      }
      return false;
    };
    llvm::SetVector<Value> inputs, outputs;
    llvm::SetVector<Operation *> remats;
    std::function<void(Value)> gather = [&](Value value) {
      if (inside(value))
        return;
      auto *def = value.getDefiningOp();
      if (rematerialize(def)) {
        if (remats.contains(def))
          return;
        for (Value operand : def->getOperands())
          gather(operand);
        remats.insert(def);
      } else
        inputs.insert(value);
    };
    for (auto *op : ops) {
      op->walk([&](Operation *nested) {
        for (Value value : nested->getOperands())
          gather(value);
      });
      for (Value value : op->getResults())
        for (auto *user : value.getUsers()) {
          auto *owner = user;
          while (owner && !members.contains(owner))
            owner = owner->getParentOp();
          if (!owner) {
            outputs.insert(value);
            break;
          }
        }
    }
    if (inputs.size() + outputs.size() > maxPorts) {
      ops.clear();
      return;
    }
    // Format strings are compile-time expression trees, not runtime values.
    // Their consumers inspect the defining ops during LLVM lowering; passing
    // a shared formatter through a helper argument loses that definition.
    if (llvm::any_of(inputs, [](Value value) {
          return isa<sim::FormatStringType>(value.getType());
        })) {
      ops.clear();
      return;
    }
    // Opaque formatted values can own stack storage after LLVM lowering. Keep
    // any chunk returning such a value in its original frame.
    for (Value value : outputs) {
      auto type = value.getType();
      if (!hw::isHWValueType(type) &&
          !isa<IntegerType, IndexType, seq::ClockType, arc::ArrayRefType,
               arc::StateType, arc::MemoryType, arc::StorageType>(type)) {
        ops.clear();
        return;
      }
    }
    OpBuilder builder(module.getContext());
    builder.setInsertionPointToEnd(module.getBody());
    SmallVector<Type> inTypes, outTypes;
    for (Value v : inputs)
      inTypes.push_back(v.getType());
    for (Value v : outputs)
      outTypes.push_back(v.getType());
    std::string name = modelName + "_part_" + std::to_string(count++);
    while (module.lookupSymbol(name))
      name += "_";
    auto helper =
        func::FuncOp::create(builder, ops.front()->getLoc(), name,
                             builder.getFunctionType(inTypes, outTypes));
    helper.setPrivate();
    helper->setAttr("llvm.no_inline", builder.getUnitAttr());
    helper->setAttr(
        "llvm.linkage",
        LLVM::LinkageAttr::get(module.getContext(), LLVM::Linkage::Internal));
    auto *entry = helper.addEntryBlock();
    builder.setInsertionPointToStart(entry);
    DenseMap<Value, Value> mapping;
    for (auto [value, arg] : llvm::zip(inputs, entry->getArguments()))
      mapping[value] = arg;
    for (auto *op : remats) {
      Operation *copy;
      if (address(op)) {
        auto input = mapping.lookup(op->getOperand(0));
        assert(input);
        copy = arc::StorageGetOp::create(
                   builder, op->getLoc(), op->getResult(0).getType(), input,
                   cast<IntegerAttr>(op->getAttr("offset")))
                   .getOperation();
      } else
        copy = builder.clone(*op);
      for (auto [a, b] : llvm::zip(op->getResults(), copy->getResults()))
        mapping[a] = b;
    }
    OpBuilder caller(ops.front());
    auto call = func::CallOp::create(caller, ops.front()->getLoc(), helper,
                                     inputs.getArrayRef());
    for (auto *op : ops)
      op->moveBefore(entry, entry->end());
    for (auto *op : ops)
      op->walk([&](Operation *nested) {
        for (auto &operand : nested->getOpOperands())
          if (auto it = mapping.find(operand.get()); it != mapping.end())
            operand.set(it->second);
      });
    builder.setInsertionPointToEnd(entry);
    func::ReturnOp::create(builder, helper.getLoc(), outputs.getArrayRef());
    for (auto [original, replacement] : llvm::zip(outputs, call.getResults())) {
      Value value = original;
      value.replaceUsesWithIf(replacement, [&](OpOperand &use) {
        return use.getOwner()->getParentOfType<func::FuncOp>() != helper;
      });
    }
    ops.clear();
  }
  void block(Block &body) {
    for (auto &op : body)
      for (auto &region : op.getRegions())
        for (auto &nested : region)
          block(nested);
    unsigned total = 0;
    for (auto &op : body)
      total += weight(&op);
    if (total <= maxOps)
      return;
    SmallVector<Operation *> ops;
    unsigned size = 0;
    auto flush = [&] {
      if (size >= std::min(32u, maxOps.getValue()))
        outline(ops);
      else
        ops.clear();
      size = 0;
    };
    for (auto &op : body) {
      if (op.hasTrait<OpTrait::IsTerminator>()) {
        flush();
        continue;
      }
      if (rematerialize(&op))
        continue;
      if (frameOwner(&op)) {
        flush();
        continue;
      }
      unsigned n = weight(&op);
      if (size && size + n > maxOps)
        flush();
      ops.push_back(&op);
      size += n;
    }
    flush();
  }
  void runOnOperation() override {
    module = getOperation();
    SmallVector<arc::ModelOp> models;
    for (auto model : module.getOps<arc::ModelOp>())
      models.push_back(model);
    for (auto model : models) {
      modelName = model.getName().str();
      block(model.getBodyBlock());
    }
    llvm::errs() << "Outlined model into " << count << " procedures\n";
  }
};
} // namespace
namespace {
struct NormalizeArrays : PassWrapper<NormalizeArrays, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NormalizeArrays)
  StringRef getArgument() const override { return "chipyard-normalize-arrays"; }
  StringRef getDescription() const override {
    return "Normalize concatenations before singleton-array and i0 removal";
  }
  void runOnOperation() override {
    // The SDK's generic i0 conversion erases an operation if any operand
    // disappears. A concat can still have a live nonempty result, so discard
    // only its empty operands first. Keep the operation and its attributes.
    unsigned emptyConcats = 0;
    getOperation().walk([&](comb::ConcatOp concat) {
      if (concat.getType().getIntOrFloatBitWidth() == 0)
        return;
      SmallVector<Value> inputs;
      for (Value input : concat.getInputs())
        if (input.getType().getIntOrFloatBitWidth() != 0)
          inputs.push_back(input);
      if (inputs.size() != concat.getNumOperands()) {
        concat.getInputsMutable().assign(inputs);
        ++emptyConcats;
      }
    });
    SmallVector<hw::ArrayConcatOp> concats;
    getOperation().walk([&](hw::ArrayConcatOp op) { concats.push_back(op); });
    for (auto concat : concats) {
      OpBuilder builder(concat);
      SmallVector<Value> elements;
      for (Value array : concat.getInputs()) {
        auto type = cast<hw::ArrayType>(array.getType());
        auto indexType =
            builder.getIntegerType(llvm::Log2_64_Ceil(type.getNumElements()));
        for (uint64_t i = type.getNumElements(); i > 0; --i) {
          Value index = hw::ConstantOp::create(builder, concat.getLoc(),
                                               indexType, i - 1);
          elements.push_back(builder.createOrFold<hw::ArrayGetOp>(
              concat.getLoc(), array, index));
        }
      }
      auto create =
          hw::ArrayCreateOp::create(builder, concat.getLoc(), elements);
      create->setDiscardableAttrs(concat->getDiscardableAttrDictionary());
      concat.replaceAllUsesWith(create.getResult());
      concat.erase();
    }
    llvm::errs() << "Normalized " << concats.size()
                 << " array concatenations and " << emptyConcats
                 << " nonempty concatenations with i0 operands before state "
                    "allocation\n";
  }
};
// LowerState creates a current_time read for every coroutine instance, even
// processes that only wait for input changes. Record actual time dependencies
// before that lowering, while wakeup values and source time reads are distinct.
// A finite or unknown wakeup stays conservative: the native host also uses this
// flag to select femtosecond waveform precision.
struct RecordTimeDependencies
    : PassWrapper<RecordTimeDependencies, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RecordTimeDependencies)
  StringRef getArgument() const override {
    return "chipyard-record-time-dependencies";
  }
  void runOnOperation() override {
    // This walk only reads symbol definitions and records a module attribute.
    SymbolTableCollection symbols;
    bool timeReaders = false;
    getOperation().walk([&](Operation *op) {
      if (isa<arc::CurrentTimeOp, llhd::CurrentTimeOp, sim::FormatCurrentTimeOp,
              llhd::WaitOp>(op))
        timeReaders = true;
      // Nested coroutine calls may transport scheduling results through local
      // state. Leave those conservative until their protocol is proven too.
      if (isa<arc::CoroutineCallOp>(op))
        timeReaders = true;
      if (auto instance = dyn_cast<arc::CoroutineInstanceOp>(op)) {
        auto definition =
            symbols.lookupNearestSymbolFrom<arc::CoroutineDefineOp>(
                instance, instance.getCalleeAttr());
        if (!definition || definition.getBody().empty()) {
          timeReaders = true;
          return;
        }
        for (auto &block : definition.getBody()) {
          auto yield = dyn_cast<arc::CoroutineYieldOp>(block.getTerminator());
          if (!yield)
            continue; // Return/halt sentinel PCs never schedule resumption.
          auto values = yield.getYieldOperands();
          APInt wakeup;
          if (values.empty() || !values.back().getType().isInteger(64) ||
              !matchPattern(values.back(), m_ConstantInt(&wakeup)) ||
              !wakeup.isAllOnes())
            timeReaders = true;
        }
      }
    });
    getOperation()->setAttr("chipyard.source_time_dependence",
                            BoolAttr::get(&getContext(), timeReaders));
  }
};
struct ExportModelInfo : PassWrapper<ExportModelInfo, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ExportModelInfo)
  ExportModelInfo() = default;
  ExportModelInfo(const ExportModelInfo &other) : PassWrapper(other) {}
  Option<std::string> output{*this, "output-file", llvm::cl::init("")};
  StringRef getArgument() const override {
    return "chipyard-export-model-info";
  }
  StringRef getDescription() const override {
    return "Export the SDK's allocated model layout";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<func::FuncDialect, arc::ArcDialect>();
  }
  void runOnOperation() override {
    if (output.empty()) {
      getOperation().emitError("model info output path is required");
      signalPassFailure();
      return;
    }
    // Keep the host independent of the SDK's context storage offsets. These
    // accessors use the same operations as the SDK simulation driver.
    SymbolTable symbols(getOperation());
    bool timeReaders = false;
    getOperation().walk([&](arc::CurrentTimeOp) { timeReaders = true; });
    if (auto source = getOperation()->getAttrOfType<BoolAttr>(
            "chipyard.source_time_dependence"))
      timeReaders = source.getValue();
    SmallVector<arc::ModelOp> definitions;
    for (auto model : getOperation().getOps<arc::ModelOp>())
      definitions.push_back(model);
    for (auto model : definitions) {
      if (model->hasAttr("chipyard.time_set_fn"))
        continue;
      auto loc = model.getLoc();
      OpBuilder builder(getOperation().getBodyRegion());
      builder.setInsertionPointToEnd(getOperation().getBody());
      auto instanceType = arc::SimModelInstanceType::get(
          &getContext(), FlatSymbolRefAttr::get(model.getSymNameAttr()));
      auto i64 = builder.getI64Type();
      auto setter = func::FuncOp::create(
          builder, loc, (model.getSymName() + "_set_time").str(),
          builder.getFunctionType({instanceType, i64}, {}));
      symbols.insert(setter);
      auto *setBlock = setter.addEntryBlock();
      OpBuilder setBuilder = OpBuilder::atBlockBegin(setBlock);
      arc::SimSetTimeOp::create(setBuilder, loc, setBlock->getArgument(0),
                                setBlock->getArgument(1));
      func::ReturnOp::create(setBuilder, loc);
      auto getter = func::FuncOp::create(
          builder, loc, (model.getSymName() + "_next_wakeup").str(),
          builder.getFunctionType({instanceType}, {i64}));
      symbols.insert(getter);
      auto *getBlock = getter.addEntryBlock();
      OpBuilder getBuilder = OpBuilder::atBlockBegin(getBlock);
      Value context =
          arc::AsContextOp::create(getBuilder, loc, getBlock->getArgument(0));
      Value wakeup = arc::GetNextWakeupOp::create(getBuilder, loc, context);
      func::ReturnOp::create(getBuilder, loc, wakeup);
      model->setAttr("chipyard.time_set_fn",
                     FlatSymbolRefAttr::get(setter.getSymNameAttr()));
      model->setAttr("chipyard.next_wakeup_fn",
                     FlatSymbolRefAttr::get(getter.getSymNameAttr()));
      model->setAttr("chipyard.time_readers", builder.getBoolAttr(timeReaders));
    }
    SmallVector<arc::ModelInfo> models;
    if (failed(arc::collectModels(getOperation(), models))) {
      signalPassFailure();
      return;
    }
    std::error_code error;
    llvm::ToolOutputFile file(output, error, llvm::sys::fs::OF_None);
    if (error) {
      getOperation().emitError() << "model info output: " << error.message();
      signalPassFailure();
      return;
    }
    std::string serialized;
    llvm::raw_string_ostream stream(serialized);
    arc::serializeModelInfoToJson(stream, models);
    auto json = llvm::json::parse(serialized);
    if (!json || !json->getAsArray()) {
      if (!json)
        llvm::consumeError(json.takeError());
      getOperation().emitError(
          "SDK model layout serialization was not an array");
      signalPassFailure();
      return;
    }
    for (auto &entry : *json->getAsArray()) {
      auto *object = entry.getAsObject();
      auto model = symbols.lookup<arc::ModelOp>(*object->getString("name"));
      (*object)["timeSetFnSym"] =
          model->getAttrOfType<FlatSymbolRefAttr>("chipyard.time_set_fn")
              .getValue();
      (*object)["nextWakeupFnSym"] =
          model->getAttrOfType<FlatSymbolRefAttr>("chipyard.next_wakeup_fn")
              .getValue();
      (*object)["hasTimeReaders"] =
          model->getAttrOfType<BoolAttr>("chipyard.time_readers").getValue();
      if (auto names =
              model->getAttrOfType<ArrayAttr>("chipyard.write_only_outputs")) {
        llvm::json::Array outputs;
        for (Attribute name : names)
          outputs.push_back(cast<StringAttr>(name).getValue());
        (*object)["convergenceOutputs"] =
            llvm::json::Object{{"version", 1}, {"names", std::move(outputs)}};
      }
      if (auto fn = model->getAttrOfType<FlatSymbolRefAttr>(
              "chipyard.idle_clock_fn")) {
        llvm::json::Array inputs;
        for (Attribute name :
             model->getAttrOfType<ArrayAttr>("chipyard.idle_clock_inputs"))
          inputs.push_back(cast<StringAttr>(name).getValue());
        llvm::json::Object proof{{"version", 1},
                                 {"function", fn.getValue()},
                                 {"inputs", std::move(inputs)}};
        if (auto stable = model->getAttrOfType<FlatSymbolRefAttr>(
                "chipyard.stable_combinational_fn"))
          proof["stableFunction"] = stable.getValue();
        if (auto evaluator = model->getAttrOfType<FlatSymbolRefAttr>(
                "chipyard.stable_eval_fn"))
          proof["stableEvaluator"] = evaluator.getValue();
        (*object)["idleClock"] = std::move(proof);
      }
      if (auto controls = getOperation()->getAttrOfType<DictionaryAttr>(
              "chipyard.sampling_inputs"))
        if (auto name = controls.getAs<StringAttr>(model.getSymName()))
          (*object)["samplingInput"] = name.getValue();
      if (auto wires = getOperation()->getAttrOfType<ArrayAttr>(
              "chipyard.feedback_wires")) {
        llvm::json::Array bindings;
        for (auto item : wires) {
          auto wire = cast<DictionaryAttr>(item);
          if (wire.getAs<StringAttr>("model").getValue() != model.getSymName())
            continue;
          bindings.push_back(llvm::json::Object{
              {"input", wire.getAs<StringAttr>("input").getValue()},
              {"output", wire.getAs<StringAttr>("output").getValue()},
              {"bits", wire.getAs<IntegerAttr>("bits").getInt()}});
        }
        (*object)["feedbackWires"] = std::move(bindings);
      }
    }
    file.os() << llvm::formatv("{0:2}", *json) << '\n';
    file.keep();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardModelCompiler", "1", [] {
            PassRegistration<OutlineModel>();
            PassRegistration<NormalizeArrays>();
            PassRegistration<ExportModelInfo>();
            PassRegistration<RecordTimeDependencies>();
            PassRegistration<CoalesceDPI>();
            PassRegistration<InlineImmutableArcs>();
            PassRegistration<LowerProceduralDPI>();
            PassRegistration<FinalizeDPIABI>();
            PassRegistration<LowerRealFormats>();
            PassRegistration<LowerNativeFiles>();
            PassRegistration<SensitizeDPI>();
            PassRegistration<SeedDPIOutputs>();
            PassRegistration<NativeDPIABI>();
          }};
}
