// Preserve local format expression trees until native string storage is ready.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include <functional>
using namespace mlir;
using namespace circt;
namespace {
constexpr StringLiteral renderMarker = "chipyard.native_format_render";
constexpr StringLiteral fieldMarker = "chipyard.native_format_field";
constexpr StringLiteral printMarker = "chipyard.native_print";
struct PrepareFormatStrings
    : PassWrapper<PrepareFormatStrings, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PrepareFormatStrings)
  StringRef getArgument() const override {
    return "chipyard-prepare-format-strings";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<hw::HWDialect, sim::SimDialect, func::FuncDialect>();
  }
  circt::Namespace names;
  func::FuncOp render, field;
  bool dynamic(sim::FormatStringOp op) {
    return !isa_and_nonnull<sim::StringConstantOp, sim::IntToStringOp>(
        op.getValue().getDefiningOp());
  }
  bool needsNative(Value value) {
    auto *op = value.getDefiningOp();
    if (auto text = dyn_cast_or_null<sim::FormatStringOp>(op))
      return dynamic(text);
    if (auto concat = dyn_cast_or_null<sim::FormatStringConcatOp>(op))
      return llvm::any_of(concat.getInputs(),
                          [&](Value v) { return needsNative(v); });
    return false;
  }
  func::FuncOp marker(StringRef key, TypeRange inputs, TypeRange outputs) {
    OpBuilder b(getOperation().getBody(), getOperation().getBody()->begin());
    auto fn =
        func::FuncOp::create(b, getOperation().getLoc(), names.newName(key),
                             b.getFunctionType(inputs, outputs));
    fn.setPrivate();
    fn->setAttr(key, b.getUnitAttr());
    return fn;
  }
  LogicalResult outline(Operation *site, Value format, Value stream,
                        bool returnsString) {
    llvm::SetVector<Value> inputs;
    SmallVector<Operation *> expressions;
    llvm::SmallPtrSet<Operation *, 16> seen;
    std::function<LogicalResult(Value)> gather =
        [&](Value value) -> LogicalResult {
      if (!isa<sim::FormatStringType>(value.getType())) {
        inputs.insert(value);
        return success();
      }
      auto *op = value.getDefiningOp();
      if (!op || !isMemoryEffectFree(op) || op->getNumRegions())
        return site->emitError(
            "native string formatting requires a local format expression tree");
      if (!seen.insert(op).second)
        return success();
      for (Value operand : op->getOperands())
        if (failed(gather(operand)))
          return failure();
      expressions.push_back(op);
      return success();
    };
    if (failed(gather(format)))
      return failure();
    if (stream)
      inputs.insert(stream);
    OpBuilder b(getOperation().getBody(), getOperation().getBody()->end());
    SmallVector<Type> types;
    for (Value value : inputs)
      types.push_back(value.getType());
    auto string = sim::DynamicStringType::get(&getContext());
    auto fn = func::FuncOp::create(
        b, site->getLoc(),
        names.newName(returnsString ? "__native_format_string"
                                    : "__native_print_string"),
        b.getFunctionType(types,
                          returnsString ? TypeRange{string} : TypeRange{}));
    fn.setPrivate();
    fn->setAttr("chipyard.native_format", b.getUnitAttr());
    IRMapping mapping;
    auto *entry = fn.addEntryBlock();
    for (auto [input, argument] : llvm::zip(inputs, entry->getArguments()))
      mapping.map(input, argument);
    b.setInsertionPointToEnd(entry);
    for (auto *op : expressions)
      b.clone(*op, mapping);
    if (returnsString) {
      if (!render)
        render = marker(renderMarker,
                        {sim::FormatStringType::get(&getContext())}, {string});
      auto call = func::CallOp::create(b, site->getLoc(), render,
                                       ValueRange{mapping.lookup(format)});
      func::ReturnOp::create(b, site->getLoc(), call.getResults());
    } else {
      auto copy = b.clone(*site, mapping);
      copy->removeAttr(printMarker);
      func::ReturnOp::create(b, site->getLoc());
    }
    b.setInsertionPoint(site);
    auto call =
        func::CallOp::create(b, site->getLoc(), fn, inputs.getArrayRef());
    call->setAttr("chipyard.native_format", b.getUnitAttr());
    if (!returnsString) {
      call->setAttr(printMarker, b.getUnitAttr());
      if (site->hasAttr("chipyard.file_output") ||
          (stream && stream.getDefiningOp<sim::SVChannelToOutputStreamOp>()))
        call->setAttr("chipyard.file_output", b.getUnitAttr());
    } else
      site->getResult(0).replaceAllUsesWith(call.getResult(0));
    site->erase();
    return success();
  }
  void runOnOperation() override {
    names.add(getOperation());
    SmallVector<sim::PrintFormattedProcOp> prints;
    getOperation().walk([&](sim::PrintFormattedProcOp op) {
      if (needsNative(op.getInput()))
        prints.push_back(op);
    });
    for (auto op : prints)
      if (failed(outline(op, op.getInput(), op.getStream(), false))) {
        signalPassFailure();
        return;
      }
    SmallVector<sim::FormatToStringOp> formats;
    getOperation().walk(
        [&](sim::FormatToStringOp op) { formats.push_back(op); });
    for (auto op : formats)
      if (failed(outline(op, op.getFmtstring(), {}, true))) {
        signalPassFailure();
        return;
      }
    // Drop unused trees copied into their local helper frames.
    bool changed;
    do {
      changed = false;
      getOperation().walk<WalkOrder::PostOrder>([&](Operation *op) {
        if (op->getName().getStringRef().starts_with("sim.fmt.") &&
            op->use_empty() && isMemoryEffectFree(op)) {
          op->erase();
          changed = true;
        }
      });
    } while (changed);
    SmallVector<sim::FormatStringOp> fields;
    getOperation().walk([&](sim::FormatStringOp op) {
      if (dynamic(op))
        fields.push_back(op);
    });
    for (auto op : fields) {
      OpBuilder b(op);
      if (!field)
        field = marker(fieldMarker,
                       {sim::DynamicStringType::get(&getContext()),
                        b.getI32Type(), b.getI32Type(), b.getI32Type()},
                       {sim::FormatStringType::get(&getContext())});
      auto constant = [&](int32_t n) -> Value {
        return hw::ConstantOp::create(b, op.getLoc(), APInt(32, uint32_t(n)));
      };
      auto call = func::CallOp::create(
          b, op.getLoc(), field,
          ValueRange{
              op.getValue(), constant(op.getSpecifierWidth().value_or(-1)),
              constant(op.getIsLeftAligned()), constant(op.getPaddingChar())});
      op.replaceAllUsesWith(call.getResult(0));
      op.erase();
    }
  }
};
struct LowerFormatStrings
    : PassWrapper<LowerFormatStrings, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerFormatStrings)
  StringRef getArgument() const override {
    return "chipyard-lower-format-strings";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<hw::HWDialect, sim::SimDialect, func::FuncDialect>();
  }
  func::FuncOp callback(StringRef name, TypeRange inputs, TypeRange results) {
    auto type = FunctionType::get(&getContext(), inputs, results);
    auto fn = getOperation().lookupSymbol<func::FuncOp>(name);
    if (fn && (!fn.isExternal() || fn.getFunctionType() != type)) {
      fn.emitError("conflicting native formatting callback");
      return {};
    }
    if (!fn) {
      OpBuilder b(getOperation().getBody(), getOperation().getBody()->begin());
      fn = func::FuncOp::create(b, getOperation().getLoc(), name, type);
      fn.setPrivate();
    }
    return fn;
  }
  LogicalResult materializeFormatPhis() {
    struct PhiBlock {
      Block *block;
      SmallVector<BlockArgument> formats;
    };
    SmallVector<PhiBlock> blocks;
    auto top = getOperation();
    top.walk([&](Operation *op) {
      for (Region &region : op->getRegions())
        for (Block &block : region) {
          SmallVector<BlockArgument> formats;
          for (auto arg : block.getArguments())
            if (isa<sim::FormatStringType>(arg.getType()))
              formats.push_back(arg);
          if (!formats.empty() && !block.use_empty())
            blocks.push_back({&block, std::move(formats)});
        }
    });
    if (blocks.empty())
      return success();
    // Inlining can merge format trees through CFG arguments. Materialize the
    // incoming pieces into string handles, retaining their SSA value snapshots.
    // Rewrite every argument first so backedges need no recursive tree walk.
    circt::Namespace names;
    names.add(top);
    auto marker = [&](StringRef key, TypeRange inputs,
                      TypeRange outputs) -> func::FuncOp {
      for (auto fn : top.getOps<func::FuncOp>())
        if (fn->hasAttr(key))
          return fn;
      OpBuilder b(top.getBody(), top.getBody()->begin());
      auto fn = func::FuncOp::create(b, top.getLoc(), names.newName(key),
                                     b.getFunctionType(inputs, outputs));
      fn.setPrivate();
      fn->setAttr(key, b.getUnitAttr());
      return fn;
    };
    OpBuilder types(&getContext());
    auto formatType = sim::FormatStringType::get(&getContext());
    auto field = marker(fieldMarker,
                        {types.getI64Type(), types.getI32Type(),
                         types.getI32Type(), types.getI32Type()},
                        {formatType});
    auto render = marker(renderMarker, {formatType}, {types.getI64Type()});
    for (auto &plan : blocks) {
      for (auto &edge : plan.block->getUses()) {
        auto branch = dyn_cast<BranchOpInterface>(edge.getOwner());
        if (!branch)
          return edge.getOwner()->emitError(
              "native format argument requires a CFG branch");
        auto operands = branch.getSuccessorOperands(edge.getOperandNumber());
        for (auto arg : plan.formats)
          if (operands.isOperandProduced(arg.getArgNumber()))
            return branch->emitError(
                "native format argument cannot be produced by a branch");
      }
      OpBuilder b(plan.block, plan.block->begin());
      for (auto arg : plan.formats) {
        auto handle = plan.block->addArgument(b.getI64Type(), arg.getLoc());
        auto constant = [&](int32_t n) -> Value {
          return hw::ConstantOp::create(b, arg.getLoc(),
                                        APInt(32, uint32_t(n)));
        };
        auto call = func::CallOp::create(
            b, arg.getLoc(), field,
            ValueRange{handle, constant(-1), constant(0), constant(32)});
        arg.replaceAllUsesWith(call.getResult(0));
      }
    }
    for (auto &plan : blocks) {
      for (auto &edge : plan.block->getUses()) {
        auto branch = cast<BranchOpInterface>(edge.getOwner());
        auto index = edge.getOperandNumber();
        OpBuilder b(branch);
        SmallVector<Value> values;
        for (auto arg : plan.formats) {
          auto value = branch.getSuccessorOperands(index)[arg.getArgNumber()];
          values.push_back(func::CallOp::create(b, branch.getLoc(), render,
                                                ValueRange{value})
                               .getResult(0));
        }
        branch.getSuccessorOperands(index).append(values);
        for (auto arg : llvm::reverse(plan.formats))
          branch.getSuccessorOperands(index).erase(arg.getArgNumber());
      }
      for (auto arg : llvm::reverse(plan.formats))
        plan.block->eraseArgument(arg.getArgNumber());
    }
    return success();
  }
  void runOnOperation() override {
    auto top = getOperation();
    if (failed(materializeFormatPhis())) {
      signalPassFailure();
      return;
    }
    auto streamType = sim::OutputStreamType::get(&getContext());
    SmallVector<func::CallOp> renders, fields;
    SmallVector<func::FuncOp> declarations;
    // Only these root-module declarations are formatting intrinsics. Their
    // names and attributes remain stable until the final cleanup below;
    // callbacks added while lowering prints are ordinary functions.
    DenseMap<StringRef, func::FuncOp> markers;
    for (auto fn : top.getOps<func::FuncOp>())
      if (fn->hasAttr(renderMarker) || fn->hasAttr(fieldMarker)) {
        declarations.push_back(fn);
        markers[fn.getSymName()] = fn;
      }
    top.walk([&](func::CallOp op) {
      auto fn = markers.lookup(op.getCallee());
      if (fn && fn->hasAttr(renderMarker))
        renders.push_back(op);
      if (fn && fn->hasAttr(fieldMarker))
        fields.push_back(op);
    });
    for (auto call : renders) {
      OpBuilder b(call);
      auto begin =
          callback("chipyard_arc_format_buffer_begin", {}, {streamType});
      auto end = callback("chipyard_arc_format_buffer_end", {streamType},
                          {b.getI64Type()});
      if (!begin || !end) {
        signalPassFailure();
        return;
      }
      auto stream = func::CallOp::create(b, call.getLoc(), begin, ValueRange{})
                        .getResult(0);
      sim::PrintFormattedProcOp::create(b, call.getLoc(), call.getOperand(0),
                                        stream);
      auto result =
          func::CallOp::create(b, call.getLoc(), end, ValueRange{stream});
      call.replaceAllUsesWith(result.getResults());
      call.erase();
    }
    SmallVector<sim::PrintFormattedProcOp> prints;
    top.walk([&](sim::PrintFormattedProcOp op) { prints.push_back(op); });
    for (auto print : prints) {
      SmallVector<Value> pending{print.getInput()}, parts;
      bool native = false, supported = true;
      while (!pending.empty()) {
        auto value = pending.pop_back_val();
        auto *op = value.getDefiningOp();
        if (auto cat = dyn_cast_or_null<sim::FormatStringConcatOp>(op)) {
          for (Value part : llvm::reverse(cat.getInputs()))
            pending.push_back(part);
          continue;
        }
        if (!op) {
          supported = false;
          break;
        }
        auto field = dyn_cast<func::CallOp>(op);
        auto fn = field ? markers.lookup(field.getCallee())
                        : func::FuncOp{};
        native |= fn && fn->hasAttr(fieldMarker);
        parts.push_back(value);
      }
      if (!native)
        continue;
      if (!supported) {
        print.emitError(
            "native formatted value escaped its local helper frame");
        signalPassFailure();
        return;
      }
      OpBuilder b(print);
      Value stream = print.getStream();
      if (!stream)
        stream = sim::StdoutStreamOp::create(b, print.getLoc());
      for (Value part : parts) {
        auto field = part.getDefiningOp<func::CallOp>();
        auto fn = field ? markers.lookup(field.getCallee())
                        : func::FuncOp{};
        if (fn && fn->hasAttr(fieldMarker)) {
          auto write = callback("chipyard_arc_format_native_string",
                                {b.getI64Type(), b.getI32Type(), b.getI32Type(),
                                 b.getI32Type(), streamType},
                                {});
          if (!write) {
            signalPassFailure();
            return;
          }
          SmallVector<Value> args(field.getOperands());
          args.push_back(stream);
          func::CallOp::create(b, print.getLoc(), write, args);
        } else {
          auto fragment = sim::PrintFormattedProcOp::create(b, print.getLoc(),
                                                            part, stream);
          fragment->setDiscardableAttrs(print->getDiscardableAttrDictionary());
        }
      }
      print.erase();
    }
    bool changed;
    do {
      changed = false;
      top.walk<WalkOrder::PostOrder>([&](Operation *op) {
        if (op->getName().getStringRef().starts_with("sim.fmt.") &&
            op->use_empty() && isMemoryEffectFree(op)) {
          op->erase();
          changed = true;
        }
      });
    } while (changed);
    for (auto op : fields) {
      if (!op->use_empty()) {
        op.emitError("native string field has an unsupported consumer");
        signalPassFailure();
        return;
      }
      op.erase();
    }
    for (auto fn : declarations) {
      if (!SymbolTable::symbolKnownUseEmpty(fn, top)) {
        fn.emitError("unresolved native format intrinsic");
        signalPassFailure();
        return;
      }
      fn.erase();
    }
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardFormatStrings", "1", [] {
            PassRegistration<PrepareFormatStrings>();
            PassRegistration<LowerFormatStrings>();
          }};
}
