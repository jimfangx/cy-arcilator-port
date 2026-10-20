// Preserve source hierarchy across imported-RTL and whole-design flattening.
#include "ScopeFrames.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
using namespace mlir;
using namespace circt;
namespace {
constexpr StringLiteral mapKey = "chipyard.hierarchy_formats";
constexpr StringLiteral idKey = "chipyard.hierarchy_id";
constexpr StringLiteral suffixKey = "chipyard.hierarchy_suffix";
static std::string escape(StringRef value) {
  bool simple = !value.empty() &&
                (llvm::isAlpha(value.front()) || value.front() == '_' ||
                 value.front() == '$') &&
                llvm::all_of(value, [](char c) {
                  return llvm::isAlnum(c) || c == '_' || c == '$';
                });
  return simple ? value.str() : "\\" + value.str() + " ";
}
static std::string plain(StringRef value) {
  std::string result;
  while (!value.empty()) {
    if (value.front() != '\\') {
      result += value.front();
      value = value.drop_front();
      continue;
    }
    value = value.drop_front();
    while (!value.empty() && !llvm::isSpace(value.front())) {
      result += value.front();
      value = value.drop_front();
    }
    if (!value.empty())
      value = value.drop_front();
  }
  return result;
}
// Slang lookup paths use :: at package/class boundaries. Formatters use dots;
// separator-looking bytes inside escaped identifiers are part of their names.
static std::string formatterPath(StringRef value) {
  std::string result;
  bool escaped = false;
  while (!value.empty()) {
    char next = value.front();
    if (!escaped && value.starts_with("::")) {
      result += '.';
      value = value.drop_front(2);
      continue;
    }
    result += next;
    value = value.drop_front();
    if (!escaped && next == '\\')
      escaped = true;
    else if (escaped && llvm::isSpace(next))
      escaped = false;
  }
  return result;
}
struct CaptureHierarchyFormats
    : PassWrapper<CaptureHierarchyFormats, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CaptureHierarchyFormats)
  CaptureHierarchyFormats() = default;
  CaptureHierarchyFormats(const CaptureHierarchyFormats &other)
      : PassWrapper(other) {}
  Option<std::string> root{*this, "root", llvm::cl::init("")};
  Option<bool> local{*this, "local", llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-capture-hierarchy-formats";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<sim::SimDialect>();
  }
  void runOnOperation() override {
    auto top = getOperation();
    SmallVector<sim::FormatHierPathOp> staticFormats, formats;
    bool missingFrame = false;
    top.walk([&](sim::FormatHierPathOp op) {
      if (op->hasAttr("chipyard.sv_scope_unresolved")) {
        op.emitError(
            "hierarchy formatter requires verified source call provenance");
        missingFrame = true;
      }
      if (op->hasAttr("chipyard.sv_format_static"))
        staticFormats.push_back(op);
      else if (!op->hasAttr(idKey))
        formats.push_back(op);
    });
    if (missingFrame) {
      signalPassFailure();
      return;
    }
    for (auto op : staticFormats) {
      auto kind = op->getAttrOfType<StringAttr>("chipyard.sv_format_static");
      auto lexical = op->getAttrOfType<StringAttr>("chipyard.sv_format_scope");
      if (!kind || !lexical ||
          (kind.getValue() != "package" && kind.getValue() != "unit")) {
        op.emitError("static hierarchy formatter requires valid lexical scope "
                     "provenance");
        signalPassFailure();
        return;
      }
      std::string value = kind.getValue() == "unit" ? "$unit." : "";
      value += formatterPath(lexical.getValue());
      OpBuilder builder(op);
      auto literal = sim::FormatLiteralOp::create(
          builder, op.getLoc(), op.getUseEscapes() ? value : plain(value));
      op.replaceAllUsesWith(literal.getResult());
      op.erase();
    }
    if (formats.empty())
      return;
    auto owner = top.lookupSymbol<hw::HWModuleOp>(root.getValue());
    if (!owner) {
      top.emitError("hierarchy formatting requires its hardware root");
      signalPassFailure();
      return;
    }
    unsigned next = 0;
    for (auto op : formats) {
      if (!op->getParentOfType<hw::HWModuleOp>()) {
        op.emitError("hierarchy formatting outside hardware scope requires "
                     "inlining or a call frame");
        signalPassFailure();
        return;
      }
      auto id =
          StringAttr::get(&getContext(), "__chipyard_hier_" + Twine(next++));
      op->setAttr(idKey, id);
      op->setAttr("name", id);
    }
    struct PhysicalNode {
      hw::HWModuleOp module;
      std::string raw, formatted, lookup;
    };
    SmallVector<PhysicalNode> nodes;
    llvm::StringMap<std::string> lookupPaths;
    bool invalid = false;
    std::function<void(hw::HWModuleOp, std::string, std::string, std::string,
                       SmallVector<Operation *>)>
        visit;
    visit = [&](hw::HWModuleOp module, std::string raw, std::string formatted,
                std::string lookup, SmallVector<Operation *> ancestors) {
      if (llvm::is_contained(ancestors, module.getOperation())) {
        module.emitError("recursive hierarchy cannot format a physical path");
        invalid = true;
        return;
      }
      ancestors.push_back(module);
      nodes.push_back({module, raw, formatted, lookup});
      auto [entry, inserted] = lookupPaths.try_emplace(lookup, formatted);
      if (!inserted && entry->second != formatted) {
        module.emitError("ambiguous physical source hierarchy for a formatter");
        invalid = true;
      }
      for (auto inst : module.getOps<hw::InstanceOp>()) {
        auto child = top.lookupSymbol<hw::HWModuleOp>(inst.getModuleName());
        if (!child)
          continue;
        auto name = inst.getInstanceName();
        std::string relative = "." + escape(name);
        auto parentScope =
            module->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
        auto childScope =
            inst->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
        if (parentScope && childScope) {
          auto suffix = childScope.getValue();
          if (!suffix.consume_front(parentScope.getValue()) ||
              !suffix.starts_with(".")) {
            inst.emitError(
                "hierarchy formatter lacks relative SV source provenance");
            invalid = true;
            continue;
          }
          relative = suffix.str();
        }
        bool adapter = local && module == owner && raw.empty() && name == "rtl";
        visit(child, raw.empty() ? name.str() : raw + "/" + name.str(),
              adapter ? formatted : formatted + formatterPath(relative),
              lookup + relative, ancestors);
      }
    };
    auto rootSource =
        owner->getAttrOfType<StringAttr>("chipyard.sv_source_scope");
    visit(owner, "", local ? "" : escape(root.getValue()),
          rootSource ? rootSource.str() : escape(root.getValue()), {});
    NamedAttrList paths;
    for (auto &node : nodes) {
      node.module.walk([&](sim::FormatHierPathOp op) {
        if (op->hasAttr(suffixKey))
          return;
        auto id = op->getAttrOfType<StringAttr>(idKey);
        if (!id)
          return;
        auto lexical =
            op->getAttrOfType<StringAttr>("chipyard.sv_format_scope");
        auto lookup =
            op->getAttrOfType<StringAttr>("chipyard.sv_format_lookup");
        std::string formatted = node.formatted;
        if (op->hasAttr("chipyard.sv_scope_frames")) {
          auto declared = node.module->getAttrOfType<StringAttr>(
              "chipyard.sv_source_scope");
          if (!declared || !lexical) {
            op.emitError(
                "hierarchy formatter lost source call frame provenance");
            invalid = true;
            return;
          }
          auto actual =
              chipyard::resolveScopeFrames(op, lexical, declared, node.lookup);
          if (failed(actual)) {
            invalid = true;
            return;
          }
          bool found = false;
          size_t longest = 0;
          for (auto &entry : lookupPaths) {
            auto suffix = StringRef(*actual);
            if (suffix.consume_front(entry.getKey()) &&
                (suffix.empty() || suffix.starts_with(".")) &&
                (!found || entry.getKey().size() > longest)) {
              found = true;
              longest = entry.getKey().size();
              formatted = entry.second + formatterPath(suffix);
            }
          }
          if (!found) {
            op.emitError("call frame has no physical declaration instance");
            invalid = true;
            return;
          }
        } else if (lookup && lookup.getValue() == "absolute") {
          bool found = false;
          size_t longest = 0;
          if (lexical)
            for (auto &entry : lookupPaths) {
              auto suffix = lexical.getValue();
              if (suffix.consume_front(entry.getKey()) &&
                  (suffix.empty() || suffix.starts_with(".")) &&
                  (!found || entry.getKey().size() > longest)) {
                found = true;
                longest = entry.getKey().size();
                formatted = entry.second + formatterPath(suffix);
              }
            }
          if (!found) {
            op.emitError("hierarchy formatter cannot resolve its physical "
                         "declaration instance");
            invalid = true;
            return;
          }
        } else {
          if (lookup && lookup.getValue() != "lexical") {
            op.emitError(
                "hierarchy formatter has invalid call lookup provenance");
            invalid = true;
            return;
          }
          auto declared = node.module->getAttrOfType<StringAttr>(
              "chipyard.sv_source_scope");
          if (declared) {
            if (!lexical) {
              op.emitError("hierarchy formatter requires frontend lexical "
                           "scope provenance");
              invalid = true;
              return;
            }
            auto relative = lexical.getValue();
            if (!relative.consume_front(declared.getValue()) ||
                (!relative.empty() && !relative.starts_with("."))) {
              op.emitError("hierarchy formatter requires a physical function "
                           "call scope");
              invalid = true;
              return;
            }
            formatted += formatterPath(relative);
          }
        }
        std::string key =
            node.raw.empty() ? id.str() : node.raw + "/" + id.str();
        auto value = StringAttr::get(&getContext(), formatted);
        if (auto previous = paths.get(key); previous && previous != value) {
          op.emitError("ambiguous hierarchy path after flattening");
          invalid = true;
          return;
        }
        paths.set(key, value);
      });
    }
    if (invalid) {
      signalPassFailure();
      return;
    }
    owner->setAttr(mapKey, paths.getDictionary(&getContext()));
  }
};
struct ResolveHierarchyFormats
    : PassWrapper<ResolveHierarchyFormats, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ResolveHierarchyFormats)
  ResolveHierarchyFormats() = default;
  ResolveHierarchyFormats(const ResolveHierarchyFormats &other)
      : PassWrapper(other) {}
  Option<std::string> root{*this, "root", llvm::cl::init("")};
  Option<bool> local{*this, "local", llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-resolve-hierarchy-formats";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<sim::SimDialect>();
  }
  void runOnOperation() override {
    auto top = getOperation();
    auto owner = top.lookupSymbol<hw::HWModuleOp>(root.getValue());
    if (!owner) {
      top.emitError("hierarchy formatting lost its hardware root");
      signalPassFailure();
      return;
    }
    auto paths = owner->getAttrOfType<DictionaryAttr>(mapKey);
    SmallVector<sim::FormatHierPathOp> formats;
    owner.walk([&](sim::FormatHierPathOp op) { formats.push_back(op); });
    for (auto op : formats) {
      auto name = op->getAttrOfType<StringAttr>("name");
      auto id = op->getAttrOfType<StringAttr>(idKey);
      if (!name || !id) {
        op.emitError("hierarchy formatter lost its source provenance");
        signalPassFailure();
        return;
      }
      auto suffix = op->getAttrOfType<StringAttr>(suffixKey);
      std::string text;
      if (local) {
        auto original =
            paths ? paths.getAs<StringAttr>(name.getValue()) : StringAttr{};
        if (!original) {
          op.emitError(
              "flattened hierarchy formatter has no original source path");
          signalPassFailure();
          return;
        }
        op->setAttr(suffixKey, original);
        op->setAttr("name", id);
        continue;
      }
      if (suffix) {
        auto raw = name.getValue();
        if (!raw.consume_back(id.getValue()) || !raw.consume_back("/rtl/")) {
          op.emitError(
              "hierarchy formatter lost its imported port adapter boundary");
          signalPassFailure();
          return;
        }
        text = escape(root.getValue());
        SmallVector<StringRef> components;
        raw.split(components, '/');
        for (auto component : components)
          if (!component.empty())
            text += "." + escape(component);
        text += suffix.str();
      } else {
        auto original =
            paths ? paths.getAs<StringAttr>(name.getValue()) : StringAttr{};
        if (!original) {
          op.emitError(
              "flattened hierarchy formatter has no original source path");
          signalPassFailure();
          return;
        }
        text = original.str();
      }
      OpBuilder b(op);
      auto literal = sim::FormatLiteralOp::create(
          b, op.getLoc(), op.getUseEscapes() ? text : plain(text));
      op.replaceAllUsesWith(literal.getResult());
      op.erase();
    }
    owner->removeAttr(mapKey);
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardHierarchyFormats", "1", [] {
            PassRegistration<CaptureHierarchyFormats>();
            PassRegistration<ResolveHierarchyFormats>();
          }};
}
