// Bind context imports to native physical declaration scopes before flattening.
#include "ScopeFrames.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/Inliner.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>
using namespace mlir;
using namespace circt;
namespace {
// An imported SV task has a C int completion status even though its SV
// prototype has no return value. Normal synchronous completion returns zero.
// The frontend currently rejects SV disable statements and timed export frames;
// a nonzero completion cannot be silently accepted as normal execution.
struct NativeDPITasks : PassWrapper<NativeDPITasks, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeDPITasks)
  StringRef getArgument() const override { return "chipyard-native-dpi-tasks"; }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<comb::CombDialect, hw::HWDialect, sim::SimDialect,
             cf::ControlFlowDialect, ub::UBDialect>();
  }
  void runOnOperation() override {
    auto top = getOperation();
    SmallVector<sim::DPIFuncOp> tasks;
    for (auto fn : top.getOps<sim::DPIFuncOp>()) {
      auto decl = fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
      auto task = decl ? decl.getAs<BoolAttr>("task") : BoolAttr{};
      if (task && task.getValue() &&
          !fn->hasAttr("chipyard.native_task_status"))
        tasks.push_back(fn);
    }
    sim::DPIFuncOp finishedQuery;
    if (!tasks.empty()) {
      OpBuilder b(top.getBody(), top.getBody()->begin());
      circt::Namespace names;
      names.add(top);
      SmallVector<sim::DPIArgument> args{{b.getStringAttr("finished"),
                                          b.getI64Type(),
                                          sim::DPIDirection::Return}};
      StringRef cname = "chipyard_arc_export_finished";
      finishedQuery = sim::DPIFuncOp::create(
          b, top.getLoc(), names.newName(cname), b.getStringAttr("private"),
          sim::DPIFunctionType::get(&getContext(), args), ArrayAttr{},
          b.getStringAttr(cname));
      finishedQuery->setAttr(
          "chipyard.dpi_abi",
          b.getArrayAttr({b.getDictionaryAttr(
              {b.getNamedAttr("direction", b.getStringAttr("return")),
               b.getNamedAttr("kind", b.getStringAttr("longint")),
               b.getNamedAttr("width", b.getI64IntegerAttr(64)),
               b.getNamedAttr("signed", b.getBoolAttr(false)),
               b.getNamedAttr("four_state", b.getBoolAttr(false))})}));
      finishedQuery->setAttr(
          "chipyard.dpi_decl",
          b.getDictionaryAttr({b.getNamedAttr("c_name", b.getStringAttr(cname)),
                               b.getNamedAttr("context", b.getBoolAttr(false)),
                               b.getNamedAttr("pure", b.getBoolAttr(false)),
                               b.getNamedAttr("task", b.getBoolAttr(false))}));
    }
    for (auto fn : tasks) {
      auto abi = fn->getAttrOfType<ArrayAttr>("chipyard.dpi_abi");
      auto args = fn.getDpiFunctionType().getArguments();
      if (!abi || abi.size() != args.size() || llvm::any_of(args, [](auto arg) {
            return arg.dir == sim::DPIDirection::Return;
          })) {
        fn.emitError("imported task requires original task ABI provenance");
        signalPassFailure();
        return;
      }
      auto uses = SymbolTable::getSymbolUses(fn, top);
      SmallVector<sim::DPICallOp> calls;
      if (!uses) {
        fn.emitError("cannot resolve imported task call sites");
        signalPassFailure();
        return;
      }
      for (auto use : *uses) {
        auto call = dyn_cast<sim::DPICallOp>(use.getUser());
        if (!call || call.getClock() || call.getEnable() ||
            !call->getParentWithTrait<OpTrait::IsIsolatedFromAbove>() ||
            (!call->getParentOfType<llhd::ProcessOp>() &&
             !call->getParentOfType<llhd::CoroutineOp>() &&
             !call->getParentOfType<func::FuncOp>())) {
          fn.emitError(
              "imported task requires an unclocked procedural call frame");
          signalPassFailure();
          return;
        }
        calls.push_back(call);
      }
      OpBuilder b(fn);
      SmallVector<sim::DPIArgument> augmented(args);
      augmented.push_back({b.getStringAttr("native_task_status"),
                           b.getI32Type(), sim::DPIDirection::Return});
      SmallVector<Attribute> metadata(abi.begin(), abi.end());
      metadata.push_back(b.getDictionaryAttr(
          {b.getNamedAttr("direction", b.getStringAttr("return")),
           b.getNamedAttr("kind", b.getStringAttr("int")),
           b.getNamedAttr("width", b.getI64IntegerAttr(32)),
           b.getNamedAttr("signed", b.getBoolAttr(true)),
           b.getNamedAttr("four_state", b.getBoolAttr(false)),
           b.getNamedAttr("native_task_status", b.getBoolAttr(true))}));
      fn->setAttr("chipyard.original_task_abi", abi);
      fn->setAttr("chipyard.native_task_status", b.getUnitAttr());
      fn->setAttr("chipyard.dpi_abi", b.getArrayAttr(metadata));
      fn.setDpiFunctionType(
          sim::DPIFunctionType::get(&getContext(), augmented));
      for (auto call : calls) {
        b.setInsertionPoint(call);
        SmallVector<Type> types(call.getResultTypes());
        types.push_back(b.getI32Type());
        auto direct = sim::DPICallOp::create(b, call.getLoc(), types,
                                             call.getCalleeAttr(), Value{},
                                             Value{}, call.getInputs());
        direct->setDiscardableAttrs(call->getDiscardableAttrDictionary());
        for (auto [old, replacement] :
             llvm::zip(call.getResults(),
                       direct.getResults().take_front(call.getNumResults())))
          old.replaceAllUsesWith(replacement);
        auto loc = call.getLoc();
        auto *block = call->getBlock();
        auto *continuation = block->splitBlock(std::next(call->getIterator()));
        auto *failure = new Block();
        block->getParent()->getBlocks().insert(continuation->getIterator(),
                                               failure);
        auto *checkStatus = new Block();
        auto *finished = new Block();
        block->getParent()->getBlocks().insert(failure->getIterator(),
                                               checkStatus);
        block->getParent()->getBlocks().insert(failure->getIterator(),
                                               finished);
        b.setInsertionPoint(call);
        auto zero = hw::ConstantOp::create(b, loc, APInt(32, 0));
        auto invalid = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ne,
                                            direct.getResults().back(), zero);
        auto stopped =
            sim::DPICallOp::create(b, loc, TypeRange{b.getI64Type()},
                                   FlatSymbolRefAttr::get(finishedQuery),
                                   Value{}, Value{}, ValueRange{})
                .getResult(0);
        auto zero64 = hw::ConstantOp::create(b, loc, APInt(64, 0));
        auto terminated = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ne,
                                               stopped, zero64);
        cf::CondBranchOp::create(b, loc, terminated, finished, ValueRange{},
                                 checkStatus, ValueRange{});
        b.setInsertionPointToEnd(checkStatus);
        cf::CondBranchOp::create(b, loc, invalid, failure, ValueRange{},
                                 continuation, ValueRange{});
        call.erase();
        b.setInsertionPointToEnd(finished);
        sim::TerminateOp::create(b, loc, true, false);
        ub::UnreachableOp::create(b, loc);
        b.setInsertionPointToEnd(failure);
        auto message = sim::FormatLiteralOp::create(
            b, loc,
            "ERROR: native DPI task returned a disable status without a "
            "supported disable frame\n");
        sim::PrintFormattedProcOp::create(b, loc, message);
        sim::TerminateOp::create(b, loc, false, false);
        ub::UnreachableOp::create(b, loc);
      }
    }
  }
};
struct ContextInliner : InlinerInterface {
  using InlinerInterface::InlinerInterface;
  bool isLegalToInline(Operation *, Operation *callee, bool) const override {
    return isa<func::FuncOp>(callee);
  }
  bool isLegalToInline(Region *, Region *, bool, IRMapping &) const override {
    return true;
  }
  bool isLegalToInline(Operation *, Region *, bool,
                       IRMapping &) const override {
    return true;
  }
  bool shouldAnalyzeRecursively(Operation *) const override { return false; }
  bool allowSingleBlockOptimization(
      iterator_range<Region::iterator> blocks) const override {
    // The fast path replaces call results from the terminator operands and
    // removes the terminator. Neither is valid for a non-returning function.
    return !llvm::hasSingleElement(blocks) ||
           isa<func::ReturnOp>(blocks.begin()->getTerminator());
  }
  void handleTerminator(Operation *op, Block *destination) const override {
    if (isa<ub::UnreachableOp>(op))
      return;
    InlinerInterface::handleTerminator(op, destination);
  }

  void processInlinedCallBlocks(
      Operation *call, iterator_range<Region::iterator> blocks) const override {
    chipyard::carryScopeFrames(call, blocks);
    auto lookup = call->getAttrOfType<StringAttr>("chipyard.sv_call_lookup");
    if (!lookup || lookup.getValue() != "absolute")
      return;
    for (auto &block : blocks)
      for (auto &op : block)
        op.walk([&](Operation *nested) {
          if (isa<sim::FormatHierPathOp>(nested))
            nested->setAttr("chipyard.sv_format_lookup", lookup);
          auto key = isa<sim::DPICallOp>(nested) ? "chipyard.dpi_context_lookup"
                                                 : "chipyard.sv_call_lookup";
          if (auto origin = nested->getAttrOfType<StringAttr>(key);
              origin && origin.getValue() == "lexical")
            nested->setAttr(key, lookup);
        });
  }
};
// The frontend keeps the source declaration for metadata, while calls use
// actual-shape specializations. Only private, unused source templates may go.
struct PruneOpenDPI : PassWrapper<PruneOpenDPI, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PruneOpenDPI)
  StringRef getArgument() const override { return "chipyard-prune-open-dpi"; }
  bool unspecialized(DictionaryAttr info) {
    if (!info || info.getAs<DictionaryAttr>("native_open_actual"))
      return false;
    auto kind = info.getAs<StringAttr>("kind");
    return (kind && kind.getValue() == "open_array") ||
           unspecialized(info.getAs<DictionaryAttr>("element"));
  }
  void runOnOperation() override {
    SmallVector<sim::DPIFuncOp> unused;
    for (auto fn : getOperation().getOps<sim::DPIFuncOp>()) {
      auto abi = fn->getAttrOfType<ArrayAttr>("chipyard.dpi_abi");
      if (fn.isPrivate() && abi &&
          SymbolTable::symbolKnownUseEmpty(fn, getOperation()) &&
          llvm::any_of(abi, [&](Attribute a) {
            return unspecialized(dyn_cast<DictionaryAttr>(a));
          }))
        unused.push_back(fn);
    }
    for (auto fn : unused)
      fn.erase();
  }
};
struct NativeContexts : PassWrapper<NativeContexts, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(NativeContexts)
  NativeContexts() = default;
  NativeContexts(const NativeContexts &other) : PassWrapper(other) {}
  Option<std::string> root{*this, "root", llvm::cl::init("")};
  StringRef getArgument() const override { return "chipyard-native-contexts"; }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<hw::HWDialect, llhd::LLHDDialect, sim::SimDialect,
             func::FuncDialect, cf::ControlFlowDialect, ub::UBDialect>();
  }
  circt::Namespace names;
  struct ScopePath {
    std::string source, native, raw;
    hw::HWModuleOp module;
  };
  SmallVector<ScopePath> paths;
  DenseMap<Operation *, sim::DPIFuncOp> bound;
  hw::HWModuleExternOp scopeDecl, rootDecl;
  sim::DPIFuncOp resolveDecl, exportResolveDecl, keepDecl;
  LogicalResult inlineContextFunctions() {
    auto top = getOperation();
    llvm::SmallPtrSet<Operation *, 32> needed;
    bool changed;
    do {
      changed = false;
      for (auto fn : top.getOps<func::FuncOp>()) {
        if (needed.contains(fn))
          continue;
        bool context = false;
        // Source hierarchy and runtime print/stop controls belong to the
        // physical hardware caller. Keep these effects out of shared helpers.
        fn.walk([&](Operation *op) {
          context |= isa<sim::FormatHierPathOp, sim::PrintFormattedProcOp,
                         sim::TerminateOp, sim::PauseOp>(op);
        });
        fn.walk([&](sim::DPICallOp call) {
          auto target = top.lookupSymbol<sim::DPIFuncOp>(call.getCallee());
          auto decl =
              target
                  ? target->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl")
                  : DictionaryAttr{};
          auto flag = decl ? decl.getAs<BoolAttr>("context") : BoolAttr{};
          context |= flag && flag.getValue();
        });
        fn.walk([&](func::CallOp call) {
          context |= needed.contains(top.lookupSymbol(call.getCallee()));
        });
        if (context) {
          needed.insert(fn);
          changed = true;
        }
      }
    } while (changed);
    DenseMap<Operation *, unsigned> visited;
    std::function<bool(Operation *)> acyclic = [&](Operation *fn) {
      auto state = visited[fn];
      if (state)
        return state == 2;
      visited[fn] = 1;
      bool valid = true;
      fn->walk([&](func::CallOp call) {
        auto *target = top.lookupSymbol(call.getCallee());
        if (needed.contains(target))
          valid &= acyclic(target);
      });
      visited[fn] = valid ? 2 : 3;
      return valid;
    };
    for (auto *fn : needed)
      if (!acyclic(fn))
        return fn->emitError(
            "recursive context SV functions require native call frames");
    ContextInliner inliner(&getContext());
    InlinerConfig config;
    while (true) {
      func::CallOp candidate;
      top.walk([&](func::CallOp call) {
        if (!candidate && needed.contains(top.lookupSymbol(call.getCallee())))
          candidate = call;
      });
      if (!candidate)
        break;
      if (candidate->hasAttr("chipyard.sv_scope_unresolved"))
        return candidate.emitError(
            "context SV function requires verified source call provenance");
      auto lookup =
          candidate->getAttrOfType<StringAttr>("chipyard.sv_call_lookup");
      if (!lookup ||
          (lookup.getValue() != "lexical" && lookup.getValue() != "absolute"))
        return candidate.emitError(
            "context SV function requires call lookup provenance");
      auto callee = top.lookupSymbol<func::FuncOp>(candidate.getCallee());
      if (failed(inlineCall(inliner, config.getCloneCallback(), candidate,
                            callee, &callee.getBody())))
        return candidate.emitError(
            "cannot inline context SV function at physical caller");
      candidate.erase();
    }
    // A fatal/finish function never returns to its SV caller. Native stop
    // callbacks record status for the scheduler; they return at the C ABI.
    // End the physical process so LLVM cannot execute unreachable machine code
    // before that scheduler gets control back. Keep other undefined behavior.
    SmallVector<ub::UnreachableOp> terminated;
    top.walk([&](ub::UnreachableOp op) {
      if (op->getParentOfType<llhd::ProcessOp>() &&
          llvm::any_of(*op->getBlock(), [](Operation &previous) {
            return isa<sim::TerminateOp>(previous);
          }))
        terminated.push_back(op);
    });
    for (auto op : terminated) {
      OpBuilder builder(op);
      llhd::HaltOp::create(builder, op.getLoc(), ValueRange{});
      op.erase();
    }
    for (auto *op : needed) {
      auto fn = cast<func::FuncOp>(op);
      if (fn.isPrivate() && SymbolTable::symbolKnownUseEmpty(fn, top))
        fn.erase();
    }
    return success();
  }
  StringAttr str(OpBuilder &b, StringRef value) {
    return StringAttr::get(value, hw::StringType::get(&getContext()));
  }
  ArrayAttr abi(OpBuilder &b, ArrayRef<StringRef> kinds) {
    SmallVector<Attribute> entries;
    for (auto [i, kind] : llvm::enumerate(kinds)) {
      NamedAttrList info;
      info.set("kind", b.getStringAttr(kind));
      info.set("direction",
               b.getStringAttr(i + 1 == kinds.size() ? "return" : "in"));
      if (kind == "longint") {
        info.set("width", b.getI64IntegerAttr(64));
        info.set("signed", b.getBoolAttr(false));
        info.set("four_state", b.getBoolAttr(false));
      }
      entries.push_back(info.getDictionary(&getContext()));
    }
    return b.getArrayAttr(entries);
  }
  sim::DPIFuncOp helper(StringRef cname, bool resolver) {
    auto top = getOperation();
    OpBuilder b(top.getBody(), top.getBody()->begin());
    SmallVector<sim::DPIArgument> args;
    SmallVector<StringRef> kinds;
    if (resolver) {
      for (unsigned i = 0; i < 4; i++) {
        args.push_back({b.getStringAttr("arg" + Twine(i)),
                        i < 2 ? Type(sim::DynamicStringType::get(&getContext()))
                              : Type(b.getI64Type()),
                        sim::DPIDirection::Input});
        kinds.push_back(i < 2 ? "string" : "longint");
      }
      args.push_back({b.getStringAttr("result"), b.getI64Type(),
                      sim::DPIDirection::Return});
      kinds.push_back("longint");
    } else {
      args.push_back(
          {b.getStringAttr("scope"), b.getI64Type(), sim::DPIDirection::Input});
    }
    auto fn = sim::DPIFuncOp::create(
        b, top.getLoc(), names.newName(cname), b.getStringAttr("private"),
        sim::DPIFunctionType::get(&getContext(), args), ArrayAttr{},
        b.getStringAttr(cname));
    if (resolver)
      fn->setAttr("chipyard.dpi_abi", abi(b, kinds));
    else
      fn->setAttr("chipyard.dpi_abi",
                  b.getArrayAttr({b.getDictionaryAttr(
                      {b.getNamedAttr("kind", b.getStringAttr("longint")),
                       b.getNamedAttr("width", b.getI64IntegerAttr(64)),
                       b.getNamedAttr("signed", b.getBoolAttr(false)),
                       b.getNamedAttr("direction", b.getStringAttr("in"))})}));
    fn->setAttr(
        "chipyard.dpi_decl",
        b.getDictionaryAttr({b.getNamedAttr("context", b.getBoolAttr(false)),
                             b.getNamedAttr("pure", b.getBoolAttr(false)),
                             b.getNamedAttr("task", b.getBoolAttr(false))}));
    return fn;
  }
  hw::HWModuleExternOp declaration(bool rootMarker) {
    auto top = getOperation();
    OpBuilder b(top.getBody(), top.getBody()->begin());
    auto string = hw::StringType::get(&getContext());
    SmallVector<hw::PortInfo> ports{
        {{b.getStringAttr("scope"), b.getI64Type(), hw::ModulePort::Output},
         0}};
    SmallVector<Attribute> params{hw::ParamDeclAttr::get("ROOTKEY", string)};
    if (!rootMarker) {
      params.push_back(hw::ParamDeclAttr::get("MODE", string));
      params.push_back(hw::ParamDeclAttr::get("SCOPE", string));
    }
    auto module = hw::HWModuleExternOp::create(
        b, top.getLoc(),
        b.getStringAttr(
            names.newName(rootMarker ? "__ArcDpiScopeRoot" : "__ArcDpiScope")),
        ports, rootMarker ? "ArcDpiScopeRoot" : "ArcDpiScope",
        b.getArrayAttr(params));
    module.setPrivate();
    return module;
  }
  Value marker(hw::HWModuleOp module, StringRef mode, StringRef target,
               bool rootMarker = false) {
    if (rootMarker && !rootDecl)
      rootDecl = declaration(true);
    if (!rootMarker && !scopeDecl)
      scopeDecl = declaration(false);
    OpBuilder b(module.getBodyBlock(), module.getBodyBlock()->begin());
    SmallVector<Attribute> params{
        hw::ParamDeclAttr::get("ROOTKEY", str(b, root.getValue()))};
    if (!rootMarker) {
      params.push_back(hw::ParamDeclAttr::get("MODE", str(b, mode)));
      params.push_back(hw::ParamDeclAttr::get("SCOPE", str(b, target)));
    }
    return hw::InstanceOp::create(b, module.getLoc(),
                                  rootMarker ? rootDecl : scopeDecl,
                                  names.newName("context_scope"),
                                  SmallVector<Value>{}, b.getArrayAttr(params))
        .getResult(0);
  }
  sim::DPIFuncOp augment(sim::DPIFuncOp original) {
    auto found = bound.find(original);
    if (found != bound.end())
      return found->second;
    OpBuilder b(original);
    auto originalABI = original->getAttrOfType<ArrayAttr>("chipyard.dpi_abi");
    SmallVector<sim::DPIArgument> args{{b.getStringAttr("native_scope"),
                                        b.getI64Type(),
                                        sim::DPIDirection::Input}};
    llvm::append_range(args, original.getDpiFunctionType().getArguments());
    SmallVector<Attribute> entries{b.getDictionaryAttr(
        {b.getNamedAttr("kind", b.getStringAttr("longint")),
         b.getNamedAttr("width", b.getI64IntegerAttr(64)),
         b.getNamedAttr("signed", b.getBoolAttr(false)),
         b.getNamedAttr("direction", b.getStringAttr("in")),
         b.getNamedAttr("native_context_scope", b.getBoolAttr(true))})};
    llvm::append_range(entries, originalABI);
    auto fn = sim::DPIFuncOp::create(
        b, original.getLoc(), names.newName("context_import"),
        b.getStringAttr("private"),
        sim::DPIFunctionType::get(&getContext(), args), ArrayAttr{},
        b.getStringAttr(
            original.getVerilogName().value_or(original.getSymName())));
    fn->setAttr("chipyard.dpi_abi", b.getArrayAttr(entries));
    fn->setAttr("chipyard.dpi_decl", original->getAttr("chipyard.dpi_decl"));
    fn->setAttr("chipyard.original_dpi_abi", originalABI);
    for (auto name :
         {"chipyard.original_task_abi", "chipyard.native_task_status"})
      if (auto attr = original->getAttr(name))
        fn->setAttr(name, attr);
    fn->setAttr("chipyard.native_context_scope", b.getUnitAttr());
    bound[original] = fn;
    return fn;
  }
  const ScopePath *physicalOwner(StringRef source) {
    const ScopePath *owner = nullptr;
    for (auto &path : paths)
      if (chipyard::scopePrefix(source, path.source) &&
          (!owner || path.source.size() > owner->source.size()))
        owner = &path;
    return owner;
  }
  FailureOr<std::string> exportScopeTable(sim::DPICallOp call,
                                          DictionaryAttr info,
                                          StringRef importedScope) {
    auto declaration = info.getAs<DictionaryAttr>("declaration");
    auto canonical =
        declaration ? declaration.getAs<StringAttr>("scope") : StringAttr{};
    auto *owner = canonical ? physicalOwner(canonical) : nullptr;
    if (!owner)
      return call.emitError("exported call frame lacks its physical "
                            "declaration provenance"),
             failure();
    std::string table;
    for (auto &physical : paths) {
      if (physical.module != owner->module)
        continue;
      auto suffix = canonical.getValue().drop_front(owner->source.size());
      auto actualCaller = physical.source + suffix.str();
      auto actual = chipyard::resolveScopeFrames(call, importedScope, canonical,
                                                 actualCaller, true);
      if (failed(actual))
        return failure();
      auto *target = physicalOwner(*actual);
      if (!target)
        return call.emitError("exported context call has no physical "
                              "declaration instance"),
               failure();
      // Native path components retain SV escaping. Tab/newline delimiters
      // cannot occur inside an SV identifier (whitespace terminates escapes).
      table += physical.native + suffix.str() + "\t1\t" + target->native +
               StringRef(*actual).drop_front(target->source.size()).str() +
               "\n";
    }
    return table;
  }
  void runOnOperation() override {
    auto top = getOperation();
    names.add(top);
    if (failed(inlineContextFunctions())) {
      signalPassFailure();
      return;
    }
    SmallVector<sim::DPICallOp> calls;
    top.walk([&](sim::DPICallOp call) {
      auto fn = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
          call, call.getCalleeAttr());
      if (!fn || fn->hasAttr("chipyard.native_context_scope"))
        return;
      auto decl = fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
      auto ctx = decl ? decl.getAs<BoolAttr>("context") : BoolAttr{};
      if (!ctx || !ctx.getValue())
        return;
      Operation *owner = call->getParentOfType<func::FuncOp>();
      if (!owner)
        owner = call->getParentOfType<llhd::CoroutineOp>();
      auto info =
          owner ? owner->getAttrOfType<DictionaryAttr>("chipyard.dpi_export")
                : DictionaryAttr{};
      auto lookup =
          call->getAttrOfType<StringAttr>("chipyard.dpi_context_lookup");
      if (info && lookup &&
          lookup == StringAttr::get(&getContext(), "lexical") &&
          info.getAs<DictionaryAttr>("declaration")
                  .getAs<StringAttr>("scope") ==
              decl.getAs<StringAttr>("scope"))
        return;
      calls.push_back(call);
    });
    if (calls.empty())
      return;
    auto rootModule = top.lookupSymbol<hw::HWModuleOp>(root.getValue());
    if (!rootModule) {
      top.emitError("native context binding requires its frontend root");
      signalPassFailure();
      return;
    }
    bool invalid = false;
    std::function<void(hw::HWModuleOp, std::string, std::string, std::string,
                       SmallVector<Operation *>)>
        visit;
    visit = [&](hw::HWModuleOp module, std::string path, std::string nativePath,
                std::string raw, SmallVector<Operation *> ancestors) {
      if (llvm::is_contained(ancestors, module.getOperation())) {
        invalid = true;
        return;
      }
      ancestors.push_back(module);
      paths.push_back({path, nativePath, raw, module});
      for (auto inst : module.getOps<hw::InstanceOp>())
        if (auto child =
                top.lookupSymbol<hw::HWModuleOp>(inst.getModuleName())) {
          auto name = inst.getInstanceName();
          bool simple =
              !name.empty() &&
              (llvm::isAlpha(name[0]) || name[0] == '_' || name[0] == '$') &&
              llvm::all_of(name, [](char c) {
                return llvm::isAlnum(c) || c == '_' || c == '$';
              });
          std::string nativeComponent =
              simple ? name.str() : "\\" + name.str() + " ";
          std::string sourcePath = path + "." + name.str();
          auto originalParent =
              module->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
          auto originalChild =
              inst->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
          if (originalParent && originalChild) {
            auto relative = originalChild.getValue();
            if (!relative.consume_front(originalParent.getValue()) ||
                !relative.starts_with(".")) {
              inst.emitError(
                  "native DPI hierarchy lacks relative source provenance");
              invalid = true;
              continue;
            }
            sourcePath = path + relative.str();
          }
          visit(child, sourcePath, nativePath + "." + nativeComponent,
                raw.empty() ? name.str() : raw + "/" + name.str(), ancestors);
        }
    };
    auto rootSource =
        rootModule->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
    visit(rootModule, rootSource ? rootSource.str() : root.getValue(), "", "",
          {});
    if (invalid) {
      top.emitError("invalid or recursive RTL hierarchy cannot carry a "
                    "physical DPI scope");
      signalPassFailure();
      return;
    }
    auto rootValue = marker(rootModule, "", "", true);
    keepDecl = helper("chipyard_arc_context_keep_root", false);
    {
      OpBuilder b(rootModule.getBodyBlock()->getTerminator());
      sim::DPICallOp::create(b, rootModule.getLoc(), TypeRange{},
                             FlatSymbolRefAttr::get(keepDecl), Value{}, Value{},
                             ValueRange{rootValue});
    }
    NamedAttrList physicalContexts;
    for (auto call : calls) {
      auto original = SymbolTable::lookupNearestSymbolFrom<sim::DPIFuncOp>(
          call, call.getCalleeAttr());
      auto decl = original->getAttrOfType<DictionaryAttr>("chipyard.dpi_decl");
      auto scope = decl.getAs<StringAttr>("scope");
      auto lookup =
          call->getAttrOfType<StringAttr>("chipyard.dpi_context_lookup");
      if (!scope || !lookup ||
          (lookup.getValue() != "lexical" && lookup.getValue() != "absolute")) {
        call.emitError("native context DPI requires call lookup provenance");
        invalid = true;
        continue;
      }
      auto source = scope.getValue();
      hw::HWModuleOp declarationOwner;
      StringRef ownerPath, nativePath;
      for (auto &path : paths)
        if ((source == path.source || source.starts_with(path.source + ".")) &&
            path.source.size() > ownerPath.size()) {
          declarationOwner = path.module;
          ownerPath = path.source;
          nativePath = path.native;
        }
      auto caller = call->getParentOfType<hw::HWModuleOp>();
      StringRef mode = "global", target = source;
      std::string physicalTarget;
      if (caller && lookup.getValue() == "lexical" &&
          declarationOwner == caller) {
        mode = "local";
        target = source.drop_front(ownerPath.size());
      } else if (declarationOwner) {
        mode = "root";
        physicalTarget =
            nativePath.str() + source.drop_front(ownerPath.size()).str();
        target = physicalTarget;
      }
      if (target.empty() && mode == "global")
        target = "$unit";
      Value scopeValue;
      if (caller) {
        scopeValue = marker(caller, mode, target);
        if (call->hasAttr("chipyard.sv_scope_unresolved")) {
          call.emitError(
              "context DPI requires verified source call provenance");
          invalid = true;
          continue;
        }
        if (declarationOwner && call->hasAttr("chipyard.sv_scope_call")) {
          auto canonical =
              caller->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
          if (!canonical) {
            call.emitError("context DPI lost physical caller provenance");
            invalid = true;
            continue;
          }
          auto inst = scopeValue.getDefiningOp<hw::InstanceOp>();
          inst->setAttr("chipyard.native_context_id",
                        inst.getInstanceNameAttr());
          for (auto &physical : paths) {
            if (physical.module != caller)
              continue;
            auto actual = chipyard::resolveScopeFrames(call, source, canonical,
                                                       physical.source, true);
            if (failed(actual)) {
              invalid = true;
              continue;
            }
            const ScopePath *owner = nullptr;
            for (auto &path : paths)
              if (chipyard::scopePrefix(*actual, path.source) &&
                  (!owner || path.source.size() > owner->source.size()))
                owner = &path;
            if (!owner) {
              call.emitError(
                  "context call frame has no physical declaration instance");
              invalid = true;
              continue;
            }
            std::string key =
                physical.raw.empty()
                    ? inst.getInstanceName().str()
                    : physical.raw + "/" + inst.getInstanceName().str();
            auto value = StringAttr::get(
                &getContext(),
                owner->native +
                    StringRef(*actual).drop_front(owner->source.size()).str());
            if (auto old = physicalContexts.get(key); old && old != value) {
              call.emitError(
                  "ambiguous physical context scope after flattening");
              invalid = true;
              continue;
            }
            physicalContexts.set(key, value);
          }
        }
      } else {
        Operation *fn = call->getParentOfType<func::FuncOp>();
        if (!fn)
          fn = call->getParentOfType<llhd::CoroutineOp>();
        auto info =
            fn ? fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_export")
               : DictionaryAttr{};
        if (!info) {
          call.emitError("context DPI needs an inlined physical caller or "
                         "exported SV frame");
          invalid = true;
          continue;
        }
        if (call->hasAttr("chipyard.sv_scope_unresolved")) {
          call.emitError(
              "exported context DPI requires verified source call provenance");
          invalid = true;
          continue;
        }
        std::string table;
        if (declarationOwner && call->hasAttr("chipyard.sv_scope_call")) {
          auto resolved = exportScopeTable(call, info, source);
          if (failed(resolved)) {
            invalid = true;
            continue;
          }
          table = *resolved;
        }
        auto &resolver = table.empty() ? resolveDecl : exportResolveDecl;
        if (!resolver)
          resolver = helper(table.empty() ? "chipyard_arc_context_resolve"
                                          : "chipyard_arc_export_scope_lookup",
                            true);
        OpBuilder b(call);
        auto key = sim::StringConstantOp::create(
            b, call.getLoc(), sim::DynamicStringType::get(&getContext()),
            b.getStringAttr(root.getValue()));
        auto path = sim::StringConstantOp::create(
            b, call.getLoc(), sim::DynamicStringType::get(&getContext()),
            b.getStringAttr(table.empty() ? target : StringRef(table)));
        auto how = hw::ConstantOp::create(
            b, call.getLoc(),
            APInt(64, table.empty() ? mode == "global" ? 2 : 1 : 0));
        auto zero = hw::ConstantOp::create(b, call.getLoc(), APInt(64, 0));
        scopeValue =
            sim::DPICallOp::create(b, call.getLoc(), TypeRange{b.getI64Type()},
                                   FlatSymbolRefAttr::get(resolver), Value{},
                                   Value{}, ValueRange{key, path, how, zero})
                .getResult(0);
      }
      auto fn = augment(original);
      SmallVector<Value> inputs{scopeValue};
      llvm::append_range(inputs, call.getInputs());
      call.getInputsMutable().assign(inputs);
      call.setCalleeAttr(FlatSymbolRefAttr::get(fn));
    }
    if (invalid) {
      signalPassFailure();
      return;
    }
    if (!physicalContexts.empty())
      rootModule->setAttr("chipyard.native_contexts",
                          physicalContexts.getDictionary(&getContext()));
    SmallVector<sim::DPIFuncOp> unused;
    for (auto &[old, fn] : bound)
      if (SymbolTable::symbolKnownUseEmpty(old, top))
        unused.push_back(cast<sim::DPIFuncOp>(old));
    for (auto fn : unused)
      fn.erase();
  }
};
// Context marker parameters must be selected after flattening: one canonical
// module may be instantiated several times with different relative call
// targets.
struct ResolveNativeContexts
    : PassWrapper<ResolveNativeContexts, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ResolveNativeContexts)
  ResolveNativeContexts() = default;
  ResolveNativeContexts(const ResolveNativeContexts &other)
      : PassWrapper(other) {}
  Option<std::string> root{*this, "root", llvm::cl::init("")};
  StringRef getArgument() const override {
    return "chipyard-resolve-native-contexts";
  }
  void runOnOperation() override {
    auto owner = getOperation().lookupSymbol<hw::HWModuleOp>(root.getValue());
    if (!owner) {
      getOperation().emitError(
          "native context resolution lost its hardware root");
      signalPassFailure();
      return;
    }
    auto paths =
        owner->getAttrOfType<DictionaryAttr>("chipyard.native_contexts");
    bool invalid = false;
    for (auto inst : owner.getOps<hw::InstanceOp>()) {
      if (!inst->hasAttr("chipyard.native_context_id"))
        continue;
      auto target = paths ? paths.getAs<StringAttr>(inst.getInstanceName())
                          : StringAttr{};
      if (!target) {
        inst.emitError("flattened context marker lost its physical call frame");
        invalid = true;
        continue;
      }
      SmallVector<Attribute> parameters;
      for (auto attr : inst.getParameters()) {
        auto param = cast<hw::ParamDeclAttr>(attr);
        if (param.getName() == "MODE" || param.getName() == "SCOPE") {
          auto text =
              param.getName() == "MODE" ? StringRef("root") : target.getValue();
          param = hw::ParamDeclAttr::get(
              param.getName(),
              StringAttr::get(text, hw::StringType::get(&getContext())));
        }
        parameters.push_back(param);
      }
      inst.setParametersAttr(ArrayAttr::get(&getContext(), parameters));
      inst->removeAttr("chipyard.native_context_id");
    }
    owner->removeAttr("chipyard.native_contexts");
    if (invalid)
      signalPassFailure();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardNativeContexts",
          LLVM_VERSION_STRING, []() {
            PassRegistration<PruneOpenDPI>();
            PassRegistration<NativeDPITasks>();
            PassRegistration<NativeContexts>();
            PassRegistration<ResolveNativeContexts>();
          }};
}
