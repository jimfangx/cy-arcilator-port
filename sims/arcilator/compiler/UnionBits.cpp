// Lower overlapping HW union storage to its packed integer representation.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/IR/AttrTypeSubElements.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"

using namespace mlir;
using namespace circt;
namespace {
struct UnionBits : PassWrapper<UnionBits, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(UnionBits)
  StringRef getArgument() const override { return "chipyard-lower-unions"; }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, comb::CombDialect, llhd::LLHDDialect>();
  }
  struct Projection {
    Operation *op;
    uint64_t offset, width, total;
    enum Kind { Create, Extract, Reference } kind;
  };
  void runOnOperation() override {
    SmallVector<Projection> projections;
    bool supported = true, found = false;
    AttrTypeWalker walker;
    walker.addWalk([&](hw::ModuleType type) {
      for (auto port : type.getPorts())
        walker.walk(port.type);
    });
    walker.addWalk([&](hw::UnionType type) {
      found = true;
      auto width = hw::getBitWidth(type);
      supported &= width >= 0 && width <= IntegerType::kMaxWidth;
    });
    getOperation().walk([&](Operation *op) {
      walker.walk(op->getAttrDictionary());
      for (auto type : op->getResultTypes())
        walker.walk(type);
      for (auto &region : op->getRegions())
        for (auto &block : region)
          for (auto argument : block.getArguments())
            walker.walk(argument.getType());
      if (auto create = dyn_cast<hw::UnionCreateOp>(op)) {
        auto type = hw::type_cast<hw::UnionType>(create.getType());
        auto field = type.getElements()[create.getFieldIndex()];
        projections.push_back(
            {op, field.offset, uint64_t(hw::getBitWidth(field.type)),
             uint64_t(hw::getBitWidth(type)), Projection::Create});
      } else if (auto extract = dyn_cast<hw::UnionExtractOp>(op)) {
        auto type = hw::type_cast<hw::UnionType>(extract.getInput().getType());
        auto field = type.getElements()[extract.getFieldIndex()];
        projections.push_back(
            {op, field.offset, uint64_t(hw::getBitWidth(field.type)),
             uint64_t(hw::getBitWidth(type)), Projection::Extract});
      } else if (auto extract = dyn_cast<llhd::SigStructExtractOp>(op)) {
        auto input =
            cast<llhd::RefType>(extract.getInput().getType()).getNestedType();
        if (auto type = hw::type_dyn_cast<hw::UnionType>(input)) {
          auto field = type.getFieldInfo(extract.getFieldAttr().getValue());
          projections.push_back(
              {op, field.offset, uint64_t(hw::getBitWidth(field.type)),
               uint64_t(hw::getBitWidth(type)), Projection::Reference});
        }
      }
    });
    if (!found)
      return;
    if (!supported) {
      getOperation().emitError("native unions require fixed packed widths");
      signalPassFailure();
      return;
    }
    for (auto projection : projections)
      if (projection.offset > projection.total ||
          projection.width > projection.total - projection.offset) {
        projection.op->emitError("invalid union field bit range");
        signalPassFailure();
        return;
      }
    // Replace nested types consistently in signatures, region arguments and
    // attributes as well as SSA results. This keeps union values transported
    // through arrays, structs, muxes, calls and CFG edges in the same storage.
    AttrTypeReplacer replacer;
    replacer.addReplacement([&](Type type) -> std::optional<Type> {
      if (auto module = dyn_cast<hw::ModuleType>(type)) {
        SmallVector<hw::ModulePort> ports(module.getPorts());
        for (auto &port : ports)
          port.type = replacer.replace(port.type);
        return hw::ModuleType::get(&getContext(), ports);
      }
      if (auto packed = hw::type_dyn_cast<hw::UnionType>(type))
        return IntegerType::get(&getContext(), hw::getBitWidth(packed));
      return std::nullopt;
    });
    replacer.recursivelyReplaceElementsIn(getOperation(), true, false, true);
    for (auto projection : projections) {
      auto *op = projection.op;
      OpBuilder b(op);
      auto loc = op->getLoc();
      auto integer = b.getIntegerType(projection.width);
      Value input = op->getOperand(0), result;
      if (projection.kind == Projection::Create) {
        if (input.getType() != integer)
          input = hw::BitcastOp::create(b, loc, integer, input);
        SmallVector<Value> pieces;
        auto high = projection.total - projection.offset - projection.width;
        if (high)
          pieces.push_back(hw::ConstantOp::create(b, loc, APInt(high, 0)));
        if (projection.width)
          pieces.push_back(input);
        if (projection.offset)
          pieces.push_back(
              hw::ConstantOp::create(b, loc, APInt(projection.offset, 0)));
        if (pieces.empty())
          result = hw::ConstantOp::create(b, loc, APInt(0u, uint64_t(0)));
        else if (pieces.size() == 1)
          result = pieces[0];
        else
          result = comb::ConcatOp::create(b, loc, pieces);
      } else if (projection.kind == Projection::Extract) {
        result = input;
        if (!projection.width)
          result = hw::ConstantOp::create(b, loc, APInt(0u, uint64_t(0)));
        else if (projection.width != projection.total || projection.offset)
          result = comb::ExtractOp::create(b, loc, input, projection.offset,
                                           projection.width);
        auto outputType = op->getResult(0).getType();
        if (result.getType() != outputType)
          result = hw::BitcastOp::create(b, loc, outputType, result);
      } else {
        auto low = hw::ConstantOp::create(b, loc, APInt(64, projection.offset));
        auto reference = UnrealizedConversionCastOp::create(
            b, loc, op->getResultTypes(), ValueRange{input, low});
        reference->setAttr("chipyard.packed_ref_slice", b.getUnitAttr());
        result = reference.getResult(0);
      }
      op->getResult(0).replaceAllUsesWith(result);
      op->erase();
    }
    llvm::errs() << "Lowered " << projections.size()
                 << " union storage projections\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardUnionBits", "1",
          [] { PassRegistration<UnionBits>(); }};
}
