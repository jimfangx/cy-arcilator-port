// Split optimized LLVM IR into balanced, independently compiled ELF objects.
// Local symbols are temporarily given unique hidden names. The object builder
// restores their local binding after combining the partition objects.
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <vector>
using namespace llvm;
namespace {
// DataLayout::operator== compares its integer-specification vectors. An
// explicit default such as i1:8 creates an extra breakpoint even though the
// canonical i8:8 fallback gives exactly the same alignment. Remove a rule only
// after proving both ABI and preferred alignment unchanged without it.
// Integer alignment lookup uses the next specification at/above a width (or
// the largest specification). Removing width N can therefore affect only the
// interval ending at N (unbounded above if N is largest), and all widths in
// that interval select the same rule. Testing N proves that interval unchanged. Other
// layout fields are not normalized and must still pass LLVM's full equality.
DataLayout normalizeRedundantIntegerSpecs(const DataLayout &layout,
                                         LLVMContext &context,
                                         json::Array &removed) {
  SmallVector<std::string> parts;
  SmallVector<StringRef> original;
  StringRef(layout.getStringRepresentation()).split(original, '-');
  for (StringRef part : original)
    parts.push_back(part.str());
  DataLayout current(layout);
  for (unsigned i = 0; i < parts.size();) {
    StringRef part(parts[i]);
    unsigned width = 0;
    if (!part.consume_front("i") ||
        part.split(':').first.getAsInteger(10, width) || !width ||
        width > IntegerType::MAX_INT_BITS) {
      ++i;
      continue;
    }
    std::string candidate;
    for (unsigned j = 0; j < parts.size(); ++j)
      if (j != i) {
        if (!candidate.empty())
          candidate += '-';
        candidate += parts[j];
      }
    auto parsed = DataLayout::parse(candidate);
    if (!parsed) {
      consumeError(parsed.takeError());
      ++i;
      continue;
    }
    Type *type = IntegerType::get(context, width);
    if (current.getABITypeAlign(type) != parsed->getABITypeAlign(type) ||
        current.getPrefTypeAlign(type) != parsed->getPrefTypeAlign(type)) {
      ++i;
      continue;
    }
    removed.push_back(parts[i]);
    current = *parsed;
    parts.erase(parts.begin() + i);
  }
  return current;
}

uint64_t work(const Function &function, const DataLayout &layout) {
  uint64_t cost = 1;
  auto words = [&](Type *type) -> uint64_t {
    if (!type->isSized())
      return 0;
    auto bits = layout.getTypeSizeInBits(type);
    return bits.isScalable() ? 1 : (bits.getFixedValue() + 63) / 64;
  };
  for (const auto &block : function)
    for (const auto &op : block) {
      ++cost;
      // Aggregate insert/extract operations do not copy their entire SSA
      // container. Count scalar/vector arithmetic and actual memory payloads.
      if (op.getType()->isIntOrIntVectorTy() || op.getType()->isFPOrFPVectorTy())
        cost += words(op.getType());
      for (const auto &operand : op.operands())
        if (operand->getType()->isIntOrIntVectorTy())
          cost += words(operand->getType());
      if (const auto *store = dyn_cast<StoreInst>(&op))
        cost += words(store->getValueOperand()->getType());
      else if (const auto *load = dyn_cast<LoadInst>(&op))
        if (load->getType()->isAggregateType())
          cost += words(load->getType());
    }
  return cost;
}

struct SplitCodegen : RequiredPassInfoMixin<SplitCodegen> {
  unsigned requested;
  explicit SplitCodegen(unsigned requested) : requested(requested) {}

  PreservedAnalyses run(Module &module, ModuleAnalysisManager &) {
    const char *directory = std::getenv("CHIPYARD_CODEGEN_SPLIT_DIR");
    if (!directory || !*directory) {
      module.getContext().emitError("missing code-generation partition directory");
      return PreservedAnalyses::all();
    }
    auto path = [&](StringRef name) {
      SmallString<256> result(directory);
      sys::path::append(result, name);
      return std::string(result);
    };
    auto write = [&](Module &part, StringRef name) {
      if (verifyModule(part, &errs())) {
        module.getContext().emitError("invalid code-generation partition");
        return false;
      }
      std::error_code error;
      raw_fd_ostream file(path(name), error, sys::fs::OF_None);
      if (error) {
        module.getContext().emitError(error.message());
        return false;
      }
      WriteBitcodeToFile(part, file);
      file.close();
      if (file.has_error()) {
        module.getContext().emitError("cannot write code-generation partition");
        return false;
      }
      return true;
    };
    const std::string originalLayout = module.getDataLayoutStr();
    bool canonicalLayout = false;
    std::string targetLayout;
    json::Array removedOriginal, removedTarget;
    auto manifest = [&](json::Object data) {
      if (!targetLayout.empty()) {
        data["original_data_layout"] = originalLayout;
        data["target_data_layout"] = targetLayout;
        data["canonical_data_layout_applied"] = canonicalLayout;
        data["data_layout_equivalence"] =
            "LLVM-DataLayout-operator==-after-proven-redundant-integer-spec-removal";
        data["redundant_original_integer_specs"] = std::move(removedOriginal);
        data["redundant_target_integer_specs"] = std::move(removedTarget);
      }
      std::error_code error;
      raw_fd_ostream file(path("partitions.json"), error, sys::fs::OF_Text);
      if (error) {
        module.getContext().emitError(error.message());
        return;
      }
      file << json::Value(std::move(data)) << '\n';
      file.close();
      if (file.has_error())
        module.getContext().emitError("cannot write partition manifest");
    };
    auto fallback = [&](StringRef reason) {
      if (write(module, "part-0.bc"))
        manifest(json::Object{{"version", 1}, {"fallback", reason},
                              {"local_symbols", json::Array()},
                              {"partitions", json::Array{json::Object{
                                   {"file", "part-0.bc"}, {"work", 0}}}}});
      return PreservedAnalyses::all();
    };
    // ThinLTO compares the input spelling with its target-machine layout.
    // CIRCT can spell default alignments explicitly; rewriting that spelling
    // is legal only when LLVM itself proves the complete layouts equivalent.
    // The ordinary post-O3 splitter leaves the layout untouched.
    if (const char *layout = std::getenv("CHIPYARD_THIN_TARGET_LAYOUT")) {
      targetLayout = layout;
      if (targetLayout.empty() || originalLayout.empty())
        return fallback("ThinLTO requires a concrete target data layout");
      auto parsed = DataLayout::parse(targetLayout);
      if (!parsed) {
        consumeError(parsed.takeError());
        return fallback("ThinLTO target data layout is malformed");
      }
      auto normalizedOriginal = normalizeRedundantIntegerSpecs(
          module.getDataLayout(), module.getContext(), removedOriginal);
      auto normalizedTarget = normalizeRedundantIntegerSpecs(
          *parsed, module.getContext(), removedTarget);
      if (normalizedOriginal != normalizedTarget)
        return fallback("ThinLTO target data layout is not semantically equivalent");
      canonicalLayout = targetLayout != originalLayout;
      module.setDataLayout(*parsed);
    }
    Triple triple = module.getTargetTriple();
    if (triple.str().empty())
      triple = Triple(sys::getDefaultTargetTriple());
    if (!triple.isOSBinFormatELF())
      return fallback("parallel object merging currently requires ELF");
    if (!module.getModuleInlineAsm().empty())
      return fallback("module assembly may reference local symbol spellings");
    for (const auto &function : module)
      for (const auto &block : function)
        for (const auto &op : block)
          if (const auto *call = dyn_cast<CallBase>(&op))
            if (call->isInlineAsm())
              return fallback("inline assembly may reference local symbols");

    SmallVector<GlobalValue *> definitions;
    DenseMap<const GlobalValue *, unsigned> indices;
    DenseMap<const GlobalValue *, std::string> originalNames;
    for (auto &value : module.global_values())
      if (!value.isDeclaration()) {
        indices[&value] = definitions.size();
        originalNames[&value] = value.getName().str();
        definitions.push_back(&value);
      }
    std::vector<unsigned> parent(definitions.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto leader = [&](unsigned i) {
      while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
      }
      return i;
    };
    auto unite = [&](const GlobalValue *a, const GlobalValue *b) {
      auto ai = indices.find(a), bi = indices.find(b);
      if (ai != indices.end() && bi != indices.end()) {
        unsigned x = leader(ai->second), y = leader(bi->second);
        parent[std::max(x, y)] = std::min(x, y);
      }
    };
    DenseMap<const Comdat *, GlobalValue *> comdats;
    for (auto *value : definitions) {
      if (const auto *group = value->getComdat()) {
        auto entry = comdats.try_emplace(group, value);
        if (!entry.second)
          unite(value, entry.first->second);
      }
      // Match LLVM SplitModule's alias/ifunc ownership rule.
      if (const auto *root = value->getAliaseeObject()) {
        if (const auto *indirect = dyn_cast<GlobalIFunc>(root))
          root = indirect->getResolverFunction();
        unite(value, root);
      }
      // Block addresses are meaningful only in the object defining their
      // function. Keep all instruction/global users and referenced owners
      // together, including constants nested inside aggregate initializers.
      SmallVector<const Value *> pending;
      SmallPtrSet<const Value *, 32> visited;
      if (const auto *function = dyn_cast<Function>(value)) {
        for (const auto &block : *function)
          for (const auto &op : block)
            for (const auto &operand : op.operands())
              pending.push_back(operand.get());
      } else {
        for (const auto &operand : value->operands())
          pending.push_back(operand.get());
      }
      while (!pending.empty()) {
        auto *operand = pending.pop_back_val();
        if (!visited.insert(operand).second)
          continue;
        if (const auto *address = dyn_cast<BlockAddress>(operand)) {
          unite(value, address->getFunction());
          continue;
        }
        if (isa<GlobalValue>(operand))
          continue;
        if (const auto *constant = dyn_cast<Constant>(operand))
          for (const auto &child : constant->operands())
            pending.push_back(child.get());
      }
    }
    struct Group {
      uint64_t cost = 0;
      SmallVector<GlobalValue *> values;
    };
    std::vector<Group> groups(definitions.size());
    uint64_t total = 0, largestFunction = 0;
    std::string largestName;
    for (unsigned i = 0; i < definitions.size(); ++i) {
      auto *value = definitions[i];
      auto &group = groups[leader(i)];
      group.values.push_back(value);
      if (const auto *function = dyn_cast<Function>(value)) {
        uint64_t weight = work(*function, module.getDataLayout());
        group.cost += weight;
        total += weight;
        if (weight > largestFunction) {
          largestFunction = weight;
          largestName = originalNames[value];
        }
      }
    }
    llvm::erase_if(groups, [](const Group &group) { return group.values.empty(); });
    unsigned functionGroups = llvm::count_if(
        groups, [](const Group &group) { return group.cost != 0; });
    unsigned count = std::min(requested, functionGroups);
    if (count < 2)
      return fallback("fewer than two independent definition groups");
    llvm::stable_sort(groups, [](const Group &a, const Group &b) {
      return a.cost > b.cost;
    });
    std::vector<uint64_t> loads(count, 0);
    std::vector<unsigned> sizes(count, 0), functions(count, 0);
    DenseMap<const GlobalValue *, unsigned> owners;
    for (const auto &group : groups) {
      unsigned owner = 0;
      for (unsigned i = 1; i < count; ++i)
        if (std::make_pair(loads[i], sizes[i]) <
            std::make_pair(loads[owner], sizes[owner]))
          owner = i;
      loads[owner] += group.cost;
      sizes[owner] += group.values.size();
      for (auto *value : group.values) {
        owners[value] = owner;
        functions[owner] += isa<Function>(value);
      }
    }
    json::Array locals;
    unsigned serial = 0;
    for (auto *value : definitions)
      if (value->hasLocalLinkage()) {
        std::string name;
        do {
          name = "__chipyard_codegen_local_" + std::to_string(serial++);
        } while (module.getNamedValue(name));
        value->setName(name);
        value->setLinkage(GlobalValue::ExternalLinkage);
        value->setVisibility(GlobalValue::HiddenVisibility);
        value->setDSOLocal(true);
        locals.push_back(name);
      }
    json::Array parts;
    for (unsigned i = 0; i < count; ++i) {
      ValueToValueMapTy map;
      auto part = CloneModule(module, map, [&](const GlobalValue *value) {
        auto found = owners.find(value);
        return found != owners.end() && found->second == i;
      });
      for (auto it = part->begin(); it != part->end();) {
        Function &function = *it++;
        if (function.isDeclaration() && function.use_empty())
          function.eraseFromParent();
      }
      for (auto it = part->global_begin(); it != part->global_end();) {
        GlobalVariable &global = *it++;
        if (global.isDeclaration() && global.use_empty())
          global.eraseFromParent();
      }
      std::string name = "part-" + std::to_string(i) + ".bc";
      if (!write(*part, name))
        return PreservedAnalyses::none();
      parts.push_back(json::Object{{"file", name}, {"work", loads[i]},
                                  {"functions", functions[i]}});
    }
    manifest(json::Object{{"version", 1}, {"partitions", std::move(parts)},
                          {"local_symbols", std::move(locals)},
                          {"total_work", total},
                          {"largest_group_work", groups.front().cost},
                          {"largest_function_work", largestFunction},
                          {"largest_function", largestName}});
    return PreservedAnalyses::none();
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ChipyardSplitCodegen", "1",
          [](PassBuilder &builder) {
            builder.registerPipelineParsingCallback(
                [](StringRef name, ModulePassManager &manager,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (!name.consume_front("chipyard-split-codegen<") ||
                      !name.consume_back(">"))
                    return false;
                  unsigned count = 0;
                  if (name.getAsInteger(10, count) || count < 1)
                    return false;
                  manager.addPass(SplitCodegen(count));
                  return true;
                });
          }};
}
