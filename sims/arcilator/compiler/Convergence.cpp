// Certify write-only model outputs for native fixed-point comparison.
#include "circt/Dialect/Arc/ArcConstants.h"
#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include <algorithm>
#include <limits>
using namespace mlir;
using namespace circt;
namespace {
constexpr StringLiteral safeAttr = "chipyard.output_readback_safe";
constexpr StringLiteral contractAttr = "chipyard.no_output_readback";
struct RecordOutputEffects
    : PassWrapper<RecordOutputEffects, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(RecordOutputEffects)
  StringRef getArgument() const override {
    return "chipyard-record-output-effects";
  }
  void runOnOperation() override {
    // Record this before DPI lowering hides imports and context/export effects.
    // Only compiler-generated helpers carry the no-readback contract. Unknown
    // external code and raw memory access retain full-state comparisons.
    bool safe = true;
    SymbolTableCollection symbols;
    getOperation().walk([&](Operation *op) {
      auto dialect = op->getName().getDialectNamespace();
      if (isa<sim::DPIFuncOp, func::CallIndirectOp>(op) || dialect == "llvm" ||
          dialect == "memref" || dialect == "verif")
        safe = false;
      // Clock-triggered effects lower to edge-guarded state. A changed clock
      // remains convergence-relevant, and repeated settling does not retrigger
      // its print/termination body. Unguarded combinational effects fall back.
      bool scheduled = op->getParentOfType<llhd::ProcessOp>() ||
                       op->getParentOfType<sim::TriggeredOp>();
      if (dialect == "sim" &&
          !isa<sim::DPICallOp, sim::DPIFuncOp, sim::TriggeredOp>(op) &&
          !isMemoryEffectFree(op) && !scheduled)
        safe = false;
      if (auto call = dyn_cast<sim::DPICallOp>(op)) {
        auto decl = symbols.lookupNearestSymbolFrom<func::FuncOp>(
            call, call.getCalleeAttr());
        if (!decl || !decl->hasAttr(contractAttr))
          safe = false;
      }
      for (StringRef attr :
           {"chipyard.dpi_export", "chipyard.export_kernel",
            "chipyard.lowered_dpi_export", "chipyard.native_context_scope"})
        if (op->hasAttr(attr))
          safe = false;
      if (auto f = dyn_cast<func::FuncOp>(op))
        if (f.isExternal() && !f->hasAttr(contractAttr) &&
            !f->hasAttr("chipyard.native_sampling_identity"))
          safe = false;
      if (auto print = dyn_cast<sim::PrintFormattedProcOp>(op))
        if (!scheduled)
          safe = false;
    });
    getOperation()->setAttr(safeAttr, BoolAttr::get(&getContext(), safe));
  }
};
struct CertifyOutputs : PassWrapper<CertifyOutputs, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CertifyOutputs)
  CertifyOutputs() = default;
  CertifyOutputs(const CertifyOutputs &other) : PassWrapper(other) {}
  Option<bool> enabled{*this, "enabled", llvm::cl::init(true)};
  StringRef getArgument() const override {
    return "chipyard-certify-write-only-outputs";
  }
  struct Allocation {
    Operation *op;
    uint64_t start, end;
  };
  void runOnOperation() override {
    auto module = getOperation();
    auto effect = module->getAttrOfType<BoolAttr>(safeAttr);
    auto time =
        module->getAttrOfType<BoolAttr>("chipyard.source_time_dependence");
    bool aliases = false;
    module.walk([&](arc::StorageGetOp get) {
      if (!get->getParentOfType<arc::ModelOp>())
        aliases = true;
    });
    unsigned total = 0;
    for (auto model : module.getOps<arc::ModelOp>()) {
      model->removeAttr("chipyard.write_only_outputs");
      if (!enabled || !effect || !effect.getValue() || !time ||
          time.getValue() || aliases)
        continue;
      auto &body = model.getBodyBlock();
      if (body.getNumArguments() != 1 || !model.getStorageBytes() ||
          *model.getStorageBytes() < arc::kStateOffset ||
          (model.getTraceTapsAttr() && !model.getTraceTapsAttr().empty()))
        continue;
      Value storage = body.getArgument(0);
      SmallVector<Allocation> allocations;
      bool valid = true;
      for (auto *user : storage.getUsers()) {
        if (auto context = dyn_cast<arc::AsContextOp>(user)) {
          for (auto *consumer : context->getResult(0).getUsers())
            if (!isa<arc::CurrentTimeOp, arc::GetNextWakeupOp,
                     arc::SetNextWakeupOp, arc::TerminateOp>(consumer))
              valid = false;
          if (!valid)
            break;
          continue;
        }
        uint64_t bytes = 0;
        if (isa<arc::RootInputOp, arc::RootOutputOp, arc::AllocStateOp>(user) ||
            (isa<arc::StorageGetOp>(user) &&
             isa<arc::StateType>(user->getResult(0).getType()))) {
          auto type = dyn_cast<arc::StateType>(user->getResult(0).getType());
          if (!type) {
            valid = false;
            break;
          }
          auto bits = arc::computeLLVMBitWidth(type.getType());
          if (!bits) {
            valid = false;
            break;
          }
          bytes = *bits / 8 + (*bits % 8 != 0);
        } else if (isa<arc::StorageGetOp>(user) &&
                   isa<arc::MemoryType>(user->getResult(0).getType())) {
          auto type = cast<arc::MemoryType>(user->getResult(0).getType());
          bytes = uint64_t(type.getNumWords()) * type.getStride();
        } else if (auto memory = dyn_cast<arc::AllocMemoryOp>(user)) {
          auto stride = user->getAttrOfType<IntegerAttr>("stride");
          if (!stride || stride.getValue().isNegative()) {
            valid = false;
            break;
          }
          uint64_t count = memory.getMemory().getType().getNumWords();
          if (count && stride.getValue().getZExtValue() >
                           std::numeric_limits<uint64_t>::max() / count) {
            valid = false;
            break;
          }
          bytes = count * stride.getValue().getZExtValue();
        } else {
          valid = false;
          break;
        }
        auto offset = user->getAttrOfType<IntegerAttr>("offset");
        if (!offset || offset.getValue().isNegative()) {
          valid = false;
          break;
        }
        uint64_t start = offset.getValue().getZExtValue();
        if (start < arc::kStateOffset || start > *model.getStorageBytes() ||
            bytes > *model.getStorageBytes() - start) {
          valid = false;
          break;
        }
        if (bytes)
          allocations.push_back({user, start, start + bytes});
      }
      if (!valid)
        continue;
      // RootOutput is a descriptor; writes and reads may instead use
      // StorageGet aliases. Check every overlapping allocation, including
      // unused descriptors, rather than relying on the output's direct users.
      llvm::sort(allocations,
                 [](auto &a, auto &b) { return a.start < b.start; });
      // Prefix maxima bound overlap lookup without a quadratic port scan.
      SmallVector<uint64_t> ends;
      uint64_t greatestEnd = 0;
      for (auto &allocation : allocations) {
        greatestEnd = std::max(greatestEnd, allocation.end);
        ends.push_back(greatestEnd);
      }
      SmallVector<Attribute> names;
      auto writesOnly = [](Value state) {
        for (auto &use : state.getUses()) {
          auto write = dyn_cast<arc::StateWriteOp>(use.getOwner());
          if (!write || write.getState() != state)
            return false;
        }
        return true;
      };
      for (auto &item : allocations) {
        auto output = dyn_cast<arc::RootOutputOp>(item.op);
        if (!output || !writesOnly(output.getState()))
          continue;
        bool safeOutput = true;
        size_t first = std::upper_bound(ends.begin(), ends.end(), item.start) -
                       ends.begin();
        for (size_t j = first;
             j < allocations.size() && allocations[j].start < item.end; ++j) {
          auto &other = allocations[j];
          if (other.op == item.op || other.end <= item.start)
            continue;
          if (!isa<arc::StorageGetOp>(other.op) || other.start != item.start ||
              other.end != item.end ||
              other.op->getResult(0).getType() != output.getState().getType() ||
              !writesOnly(other.op->getResult(0))) {
            safeOutput = false;
            break;
          }
        }
        if (safeOutput) {
          names.push_back(output.getNameAttr());
          ++total;
        }
      }
      model->setAttr("chipyard.write_only_outputs",
                     ArrayAttr::get(&getContext(), names));
    }
    llvm::errs() << "Certified " << total << " write-only model outputs\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardConvergence", "1", [] {
            PassRegistration<RecordOutputEffects>();
            PassRegistration<CertifyOutputs>();
          }};
}
