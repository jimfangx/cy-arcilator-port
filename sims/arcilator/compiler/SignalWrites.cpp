// Preserve the reference ranges of disjoint procedural writes before LLHD's
// drive hoister turns each process's partial update into a whole-value update.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include <set>

using namespace mlir;
using namespace circt;

namespace {
struct SplitSignalWrites
    : PassWrapper<SplitSignalWrites, OperationPass<hw::HWModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SplitSignalWrites)
  StringRef getArgument() const override {
    return "chipyard-split-signal-writes";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<hw::HWDialect, comb::CombDialect, llhd::LLHDDialect>();
  }
  Value packed(OpBuilder &builder, Location loc, Value value) {
    auto type = builder.getIntegerType(hw::getBitWidth(value.getType()));
    return value.getType() == type
               ? value
               : hw::BitcastOp::create(builder, loc, type, value);
  }
  Value extract(OpBuilder &builder, Location loc, Value value, uint64_t low,
                unsigned width) {
    value = packed(builder, loc, value);
    if (!low && value.getType().getIntOrFloatBitWidth() == width)
      return value;
    return comb::ExtractOp::create(builder, loc, value, low, width);
  }
  bool split(llhd::SignalOp signal) {
    auto total = hw::getBitWidth(signal.getType().getNestedType());
    if (total <= 1)
      return false;
    struct Condition {
      llhd::SigArrayGetOp alias;
      uint64_t element;
    };
    struct Slice {
      uint64_t offset;
      SmallVector<Condition, 2> conditions;
    };
    DenseMap<Value, SmallVector<Slice>> offsets;
    SmallVector<Value> references{signal.getResult()};
    SmallVector<llhd::ProbeOp> probes;
    SmallVector<llhd::DriveOp> drives;
    SmallPtrSet<Operation *, 8> writers;
    DenseMap<Operation *, SmallVector<std::pair<uint64_t, uint64_t>>>
        writerRanges;
    bool hasDynamicIndex = false;
    std::set<uint64_t> boundaries{0, uint64_t(total)};
    offsets[signal] = {{0, {}}};
    for (unsigned i = 0; i < references.size(); ++i) {
      Value reference = references[i];
      auto ranges = offsets.lookup(reference);
      auto referenceWidth = hw::getBitWidth(
          cast<llhd::RefType>(reference.getType()).getNestedType());
      auto validRange = [&](uint64_t low, int64_t width) {
        return width > 0 && low <= uint64_t(referenceWidth) &&
               uint64_t(width) <= uint64_t(referenceWidth) - low;
      };
      for (auto *user : reference.getUsers()) {
        if (auto probe = dyn_cast<llhd::ProbeOp>(user)) {
          probes.push_back(probe);
        } else if (auto drive = dyn_cast<llhd::DriveOp>(user)) {
          auto width = hw::getBitWidth(drive.getValue().getType());
          if (!validRange(0, width))
            return false;
          drives.push_back(drive);
          auto process = drive->getParentOfType<llhd::ProcessOp>();
          auto *writer =
              process ? process.getOperation() : drive.getOperation();
          writers.insert(writer);
          for (const auto &range : ranges)
            writerRanges[writer].emplace_back(range.offset,
                                              range.offset + width);
          for (const auto &range : ranges) {
            boundaries.insert(range.offset);
            boundaries.insert(range.offset + width);
          }
        } else if (auto alias = dyn_cast<llhd::SigExtractOp>(user)) {
          IntegerAttr index;
          if (!matchPattern(alias.getLowBit(), m_Constant(&index)) ||
              index.getValue().getActiveBits() > 63)
            return false;
          auto low = index.getValue().getZExtValue();
          auto width = hw::getBitWidth(alias.getType().getNestedType());
          if (!validRange(low, width))
            return false;
          SmallVector<Slice> projected;
          for (auto range : ranges) {
            range.offset += low;
            projected.push_back(std::move(range));
          }
          if (offsets.try_emplace(alias, std::move(projected)).second)
            references.push_back(alias);
        } else if (auto alias = dyn_cast<llhd::SigArrayGetOp>(user)) {
          auto width = hw::getBitWidth(alias.getType().getNestedType());
          if (width <= 0 || width > referenceWidth)
            return false;
          auto elements = uint64_t(referenceWidth) / width;
          SmallVector<Slice> projected;
          IntegerAttr index;
          if (matchPattern(alias.getIndex(), m_Constant(&index))) {
            if (index.getValue().getActiveBits() > 63)
              return false;
            auto element = index.getValue().getZExtValue();
            if (element >= elements)
              return false;
            for (auto range : ranges) {
              range.offset += element * width;
              projected.push_back(std::move(range));
            }
          } else {
            hasDynamicIndex = true;
            auto indexType = dyn_cast<IntegerType>(alias.getIndex().getType());
            if (!indexType || elements > 4096 ||
                ranges.size() > 4096 / elements)
              return false;
            if (indexType.getWidth() < 64)
              elements =
                  std::min(elements, uint64_t(1) << indexType.getWidth());
            for (const auto &base : ranges)
              for (uint64_t element = 0; element < elements; ++element) {
                auto range = base;
                range.offset += element * width;
                range.conditions.push_back({alias, element});
                projected.push_back(std::move(range));
              }
          }
          if (offsets.try_emplace(alias, std::move(projected)).second)
            references.push_back(alias);
        } else if (auto alias = dyn_cast<llhd::SigStructExtractOp>(user)) {
          auto type = dyn_cast<hw::StructType>(
              cast<llhd::RefType>(reference.getType()).getNestedType());
          if (!type)
            return false;
          uint64_t low = 0;
          bool found = false;
          for (auto field : llvm::reverse(type.getElements())) {
            if (field.name == alias.getFieldAttr()) {
              found = true;
              break;
            }
            auto width = hw::getBitWidth(field.type);
            if (width <= 0)
              return false;
            low += width;
          }
          auto width = hw::getBitWidth(alias.getType().getNestedType());
          if (!found || !validRange(low, width))
            return false;
          SmallVector<Slice> projected;
          for (auto range : ranges) {
            range.offset += low;
            projected.push_back(std::move(range));
          }
          if (offsets.try_emplace(alias, std::move(projected)).second)
            references.push_back(alias);
        } else {
          // Escaped and dynamically bit-indexed references retain their
          // original semantics and remain for the ordinary LLHD lowering to
          // handle.
          return false;
        }
      }
    }
    if (writers.size() < 2 || boundaries.size() <= 2 ||
        boundaries.size() > 4097)
      return false;
    // Dynamic expansion is needed to separate disjoint process writers. Fully
    // overlapping writers (notably memory initialization plus clocked updates)
    // already belong to the ordinary memory lowering path; turning those into
    // conditional first definitions also defeats LLHD reaching-definition
    // analysis.
    if (hasDynamicIndex) {
      for (auto first = writerRanges.begin(); first != writerRanges.end();
           ++first)
        for (auto second = std::next(first); second != writerRanges.end();
             ++second)
          for (auto a : first->second)
            for (auto b : second->second)
              if (a.first < b.second && b.first < a.second)
                return false;
    }
    SmallVector<uint64_t> points(boundaries.begin(), boundaries.end());
    SmallVector<llhd::SignalOp> pieces;
    OpBuilder builder(signal);
    auto loc = signal.getLoc();
    for (unsigned i = 0; i + 1 < points.size(); ++i) {
      auto width = points[i + 1] - points[i];
      auto type = builder.getIntegerType(width);
      Value init;
      if (signal.getInit())
        init = extract(builder, loc, signal.getInit(), points[i], width);
      auto name = builder.getStringAttr(
          (signal.getName().value_or("signal") + "_bits_" + Twine(points[i]))
              .str());
      pieces.push_back(llhd::SignalOp::create(
          builder, loc, llhd::RefType::get(type), name, init));
    }
    // Index comparisons stay at the original access, where the index dominates.
    // Keep the alias operation in each condition rather than caching its index:
    // replacing a probe below can also replace an index's defining value.
    auto condition = [&](OpBuilder &builder, Location loc, const Slice &range) {
      Value enabled;
      for (auto test : range.conditions) {
        auto index = test.alias.getIndex();
        auto bound = hw::ConstantOp::create(
            builder, loc,
            APInt(cast<IntegerType>(index.getType()).getWidth(), test.element));
        Value same = comb::ICmpOp::create(builder, loc, comb::ICmpPredicate::eq,
                                          index, bound);
        enabled = enabled ? comb::AndOp::create(
                                builder, loc, ValueRange{enabled, same}, false)
                          : same;
      }
      return enabled;
    };
    // Every read observes the same packed value, reconstructed at its original
    // program point. Whole-value writes still write every piece; no driver or
    // overlap is dropped, and each drive keeps its original delay and enable.
    for (auto probe : probes) {
      builder.setInsertionPoint(probe);
      SmallVector<Value> values;
      for (auto piece : llvm::reverse(pieces))
        values.push_back(llhd::ProbeOp::create(builder, probe.getLoc(), piece));
      Value whole = comb::ConcatOp::create(builder, probe.getLoc(), values);
      auto width = hw::getBitWidth(probe.getType());
      Value value;
      for (const auto &range : offsets.lookup(probe.getSignal())) {
        auto selected =
            extract(builder, probe.getLoc(), whole, range.offset, width);
        auto enabled = condition(builder, probe.getLoc(), range);
        if (!enabled) {
          value = selected;
          break;
        }
        if (!value)
          value =
              hw::ConstantOp::create(builder, probe.getLoc(), APInt(width, 0));
        value = comb::MuxOp::create(builder, probe.getLoc(), enabled, selected,
                                    value);
      }
      if (value.getType() != probe.getType())
        value = hw::BitcastOp::create(builder, probe.getLoc(), probe.getType(),
                                      value);
      probe.replaceAllUsesWith(value);
      probe.erase();
    }
    for (auto drive : drives) {
      builder.setInsertionPoint(drive);
      for (const auto &range : offsets.lookup(drive.getSignal())) {
        auto low = range.offset;
        auto high = low + hw::getBitWidth(drive.getValue().getType());
        Value enable = condition(builder, drive.getLoc(), range);
        if (drive.getEnable()) {
          if (enable)
            enable = comb::AndOp::create(builder, drive.getLoc(),
                                         ValueRange{enable, drive.getEnable()},
                                         false);
          else
            enable = drive.getEnable();
        }
        for (unsigned i = 0; i < pieces.size(); ++i) {
          if (points[i] < low || points[i + 1] > high)
            continue;
          auto value = extract(builder, drive.getLoc(), drive.getValue(),
                               points[i] - low, points[i + 1] - points[i]);
          llhd::DriveOp::create(builder, drive.getLoc(), pieces[i], value,
                                drive.getTime(), enable);
        }
      }
      drive.erase();
    }
    for (auto reference : llvm::reverse(references))
      reference.getDefiningOp()->erase();
    return true;
  }
  void runOnOperation() override {
    SmallVector<llhd::SignalOp> signals(
        getOperation().getOps<llhd::SignalOp>());
    unsigned count = 0;
    for (auto signal : signals)
      count += split(signal);
    if (count)
      llvm::errs() << "Split " << count
                   << " signals at their procedural write boundaries\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardSignalWrites", "1",
          [] { PassRegistration<SplitSignalWrites>(); }};
}
