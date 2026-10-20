// Native stack storage for automatic references in combinational regions.
// Descriptors stay inside one invocation; pointers never cross a suspension.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/BitVector.h"
#include <functional>
using namespace mlir;
using namespace circt;
namespace {
struct LocalMemory : PassWrapper<LocalMemory, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LocalMemory)
  StringRef getArgument() const override {
    return "chipyard-native-local-memory";
  }
  void getDependentDialects(DialectRegistry &r) const override {
    r.insert<hw::HWDialect, comb::CombDialect, llhd::LLHDDialect,
             LLVM::LLVMDialect, arith::ArithDialect>();
  }
  struct Range {
    Value offset, available;
  };
  struct Reference {
    Value pointer;
    Range range;
  };
  DenseSet<Operation *> consumed;
  Value constant(OpBuilder &b, Location loc, uint64_t value) {
    return hw::ConstantOp::create(b, loc, APInt(64, value));
  }
  static int64_t storageWidth(Type type) {
    if (type.isF32() || type.isF64())
      return cast<FloatType>(type).getWidth();
    return hw::getBitWidth(type);
  }
  static Value bitcast(OpBuilder &b, Location loc, Type type, Value value) {
    if (type == value.getType())
      return value;
    if (isa<FloatType>(type) || isa<FloatType>(value.getType()))
      return b.createOrFold<arith::BitcastOp>(loc, type, value);
    return hw::BitcastOp::create(b, loc, type, value);
  }
  Value word(OpBuilder &b, Location loc, Value value) {
    auto width = storageWidth(value.getType());
    if (!isa<IntegerType>(value.getType()))
      value = bitcast(b, loc, b.getIntegerType(width), value);
    if (width == 0)
      return constant(b, loc, 0);
    if (width == 64)
      return value;
    if (width > 64)
      return comb::ExtractOp::create(b, loc, value, 0, 64);
    return comb::ConcatOp::create(
        b, loc,
        SmallVector<Value>{hw::ConstantOp::create(b, loc, APInt(64 - width, 0)),
                           value});
  }
  // Keep the full index for bounds checks. Only a valid index can contribute
  // to a byte offset, so truncating an invalid wide index cannot make it live.
  Value indexValid(OpBuilder &b, Location loc, Value index, uint64_t limit) {
    unsigned width = storageWidth(index.getType());
    if (width < 64 && (limit >> width))
      return hw::ConstantOp::create(b, loc, APInt(1, 1));
    auto bound = hw::ConstantOp::create(b, loc, APInt(width, limit));
    return comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ult, index, bound);
  }
  Range subrange(OpBuilder &b, Location loc, Range parent, Value low,
                 uint64_t width, Value valid = {}) {
    Value fits = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ult, low,
                                      parent.available);
    if (valid)
      fits = comb::AndOp::create(b, loc, ValueRange{fits, valid}, false);
    Value remaining = comb::SubOp::create(b, loc, parent.available, low);
    Value maximum = constant(b, loc, width);
    Value smaller = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ult,
                                         remaining, maximum);
    remaining = comb::MuxOp::create(b, loc, smaller, remaining, maximum);
    remaining =
        comb::MuxOp::create(b, loc, fits, remaining, constant(b, loc, 0));
    Value offset =
        comb::AddOp::create(b, loc, ValueRange{parent.offset, low}, false);
    offset = comb::MuxOp::create(b, loc, fits, offset, constant(b, loc, 0));
    return {offset, remaining};
  }

  Value resize(OpBuilder &b, Location loc, Value value, unsigned bits) {
    auto n = storageWidth(value.getType());
    if (!isa<IntegerType>(value.getType()))
      value = bitcast(b, loc, b.getIntegerType(n), value);
    if (n == bits)
      return value;
    if (n > bits)
      return comb::ExtractOp::create(b, loc, value, 0, bits);
    return comb::ConcatOp::create(
        b, loc,
        ValueRange{hw::ConstantOp::create(b, loc, APInt(bits - n, 0)), value});
  }
  Value mask(OpBuilder &b, Location loc, Value available, unsigned bits) {
    auto empty = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::eq,
                                      available, constant(b, loc, 0));
    auto distance =
        comb::SubOp::create(b, loc, constant(b, loc, bits), available);
    auto safe =
        comb::MuxOp::create(b, loc, empty, constant(b, loc, 0), distance);
    Value ones = hw::ConstantOp::create(b, loc, APInt::getAllOnes(bits));
    auto shifted =
        comb::ShrUOp::create(b, loc, ones, resize(b, loc, safe, bits));
    Value zero = hw::ConstantOp::create(b, loc, APInt(bits, 0));
    return comb::MuxOp::create(b, loc, empty, zero, shifted);
  }
  static bool isPackedSlice(Operation *op) {
    if (!op || !isa<UnrealizedConversionCastOp>(op) ||
        !op->getAttrOfType<UnitAttr>("chipyard.packed_ref_slice") ||
        op->getNumOperands() != 2 || op->getNumResults() != 1)
      return false;
    auto input = dyn_cast<llhd::RefType>(op->getOperand(0).getType());
    auto result = dyn_cast<llhd::RefType>(op->getResult(0).getType());
    return input && result && isa<IntegerType>(op->getOperand(1).getType()) &&
           storageWidth(input.getNestedType()) > 0 &&
           storageWidth(result.getNestedType()) > 0;
  }
  bool lower(llhd::SignalOp first) {
    SmallVector<Value> references{first.getResult()};
    DenseSet<Value> seen{first.getResult()};
    SmallVector<llhd::SignalOp> roots;
    unsigned storageBits = 1;
    auto *region = first->getParentRegion();
    SmallVector<Operation *> aliases;
    SmallVector<llhd::ProbeOp> probes;
    SmallVector<llhd::DriveOp> drives;
    DenseMap<Block *, SmallVector<BlockArgument>> phis;
    auto enqueue = [&](Value value) {
      if (seen.insert(value).second)
        references.push_back(value);
    };
    auto isAlias = [](Operation *op) {
      return isPackedSlice(op) ||
             isa<llhd::SigArrayGetOp, llhd::SigArraySliceOp,
                 llhd::SigStructExtractOp, llhd::SigExtractOp>(op);
    };
    // Validate both directions before allocating or modifying any reference.
    // Following incoming edges collects every root of a cross-memory merge.
    for (unsigned i = 0; i < references.size(); ++i) {
      Value ref = references[i];
      auto type = dyn_cast<llhd::RefType>(ref.getType());
      if (!type || storageWidth(type.getNestedType()) <= 0)
        return false;
      if (auto arg = dyn_cast<BlockArgument>(ref)) {
        auto *block = arg.getOwner();
        if (block->getParent() != region || block->hasNoPredecessors())
          return false;
        phis[block].push_back(arg);
        for (auto *pred : block->getPredecessors()) {
          auto branch = dyn_cast<BranchOpInterface>(pred->getTerminator());
          if (!branch)
            return false;
          for (unsigned s = 0; s < branch->getNumSuccessors(); ++s)
            if (branch->getSuccessor(s) == block) {
              auto operands = branch.getSuccessorOperands(s);
              if (operands.isOperandProduced(arg.getArgNumber()))
                return false;
              enqueue(operands[arg.getArgNumber()]);
            }
        }
      } else if (auto root = ref.getDefiningOp<llhd::SignalOp>()) {
        if (root->getParentOp() != first->getParentOp())
          return false;
        auto bits = storageWidth(root.getType().getNestedType());
        if (bits <= 0 || bits > IntegerType::kMaxWidth)
          return false;
        storageBits = std::max(storageBits, unsigned(bits));
        roots.push_back(root);
      } else {
        auto *op = ref.getDefiningOp();
        if (!op || !isAlias(op))
          return false;
        if (auto field = dyn_cast<llhd::SigStructExtractOp>(op))
          if (!isa<hw::StructType>(
                  cast<llhd::RefType>(field.getInput().getType())
                      .getNestedType()))
            return false;
        aliases.push_back(op);
        enqueue(op->getOperand(0));
      }
      for (auto &use : ref.getUses()) {
        auto *user = use.getOwner();
        if (user->getParentRegion() != region)
          return false;
        if (auto probe = dyn_cast<llhd::ProbeOp>(user)) {
          probes.push_back(probe);
        } else if (auto drive = dyn_cast<llhd::DriveOp>(user)) {
          llhd::TimeAttr time;
          if (drive.getSignal() != ref ||
              !matchPattern(drive.getTime(), m_Constant(&time)) ||
              time.getTime() || time.getDelta() || time.getEpsilon() > 1)
            return false;
          drives.push_back(drive);
        } else if (isAlias(user)) {
          if (use.getOperandNumber() != 0)
            return false;
          enqueue(user->getResult(0));
        } else if (auto branch = dyn_cast<BranchOpInterface>(user)) {
          auto argument =
              branch.getSuccessorBlockArgument(use.getOperandNumber());
          if (!argument)
            return false;
          enqueue(*argument);
        } else {
          return false;
        }
      }
    }

    DenseMap<Value, Reference> resolved;
    auto i64 = IntegerType::get(&getContext(), 64);
    auto ptr = LLVM::LLVMPointerType::get(&getContext());
    auto storage = IntegerType::get(&getContext(), storageBits);
    // A connected component may merge roots of different sizes. Give each
    // allocation the same LLVM element type, retaining its actual extent in
    // the descriptor. Loads/stores then remain well typed across every edge.
    for (auto root : roots) {
      OpBuilder b(root);
      auto loc = root.getLoc();
      auto pointer =
          LLVM::AllocaOp::create(b, loc, ptr, storage, constant(b, loc, 1));
      Value initial = root.getInit();
      if (initial)
        initial = resize(b, loc, initial, storageBits);
      else
        // Unspecified bits use the native backend's two-state zero policy.
        initial = hw::ConstantOp::create(b, loc, APInt(storageBits, 0));
      LLVM::StoreOp::create(b, loc, initial, pointer);
      resolved[root] = {
          pointer,
          {constant(b, loc, 0),
           constant(b, loc, storageWidth(root.getType().getNestedType()))}};
    }
    for (auto &[block, arguments] : phis)
      for (auto argument : arguments) {
        auto loc = argument.getLoc();
        auto pointer = block->addArgument(ptr, loc);
        auto offset = block->addArgument(i64, loc);
        auto available = block->addArgument(i64, loc);
        resolved[argument] = {pointer, {offset, available}};
      }
    std::function<Reference(Value)> resolve = [&](Value ref) -> Reference {
      if (auto it = resolved.find(ref); it != resolved.end())
        return it->second;
      auto *alias = ref.getDefiningOp();
      auto parent = resolve(alias->getOperand(0));
      OpBuilder b(alias);
      auto loc = alias->getLoc();
      auto type = cast<llhd::RefType>(ref.getType()).getNestedType();
      Value low, valid;
      if (isPackedSlice(alias)) {
        auto index = alias->getOperand(1);
        auto parentType =
            cast<llhd::RefType>(alias->getOperand(0).getType()).getNestedType();
        valid = indexValid(b, loc, index, storageWidth(parentType));
        low = word(b, loc, index);
      } else if (auto get = dyn_cast<llhd::SigArrayGetOp>(alias)) {
        auto array = cast<hw::ArrayType>(
            cast<llhd::RefType>(get.getInput().getType()).getNestedType());
        valid = indexValid(b, loc, get.getIndex(), array.getNumElements());
        low = comb::MulOp::create(
            b, loc,
            ValueRange{word(b, loc, get.getIndex()),
                       constant(b, loc, storageWidth(array.getElementType()))},
            false);
      } else if (auto slice = dyn_cast<llhd::SigArraySliceOp>(alias)) {
        auto array = cast<hw::ArrayType>(
            cast<llhd::RefType>(slice.getInput().getType()).getNestedType());
        valid = indexValid(b, loc, slice.getLowIndex(), array.getNumElements());
        low = comb::MulOp::create(
            b, loc,
            ValueRange{word(b, loc, slice.getLowIndex()),
                       constant(b, loc, storageWidth(array.getElementType()))},
            false);
      } else if (auto field = dyn_cast<llhd::SigStructExtractOp>(alias)) {
        auto structure = cast<hw::StructType>(
            cast<llhd::RefType>(field.getInput().getType()).getNestedType());
        uint64_t offset = 0;
        for (auto element : llvm::reverse(structure.getElements())) {
          if (element.name == field.getFieldAttr())
            break;
          offset += storageWidth(element.type);
        }
        low = constant(b, loc, offset);
      } else {
        auto extract = cast<llhd::SigExtractOp>(alias);
        auto parentWidth = storageWidth(
            cast<llhd::RefType>(extract.getInput().getType()).getNestedType());
        valid = indexValid(b, loc, extract.getLowBit(), parentWidth);
        low = word(b, loc, extract.getLowBit());
      }
      auto range =
          subrange(b, loc, parent.range, low, storageWidth(type), valid);
      return resolved[ref] = {parent.pointer, range};
    };
    for (auto *alias : aliases)
      (void)resolve(alias->getResult(0));
    // Forward descriptors along the same successor edge as the old reference.
    for (auto &[block, arguments] : phis) {
      DenseSet<Operation *> predecessors;
      for (auto *pred : block->getPredecessors())
        predecessors.insert(pred->getTerminator());
      for (auto *op : predecessors) {
        auto branch = cast<BranchOpInterface>(op);
        for (unsigned s = 0; s < op->getNumSuccessors(); ++s) {
          if (op->getSuccessor(s) != block)
            continue;
          SmallVector<Value> values;
          for (auto argument : arguments) {
            auto ref = resolve(
                branch.getSuccessorOperands(s)[argument.getArgNumber()]);
            values.append({ref.pointer, ref.range.offset, ref.range.available});
          }
          branch.getSuccessorOperands(s).append(values);
        }
      }
    }

    for (auto probe : probes) {
      OpBuilder b(probe);
      auto loc = probe.getLoc();
      auto ref = resolve(probe.getSignal());
      Value value = LLVM::LoadOp::create(b, loc, storage, ref.pointer);
      value = comb::ShrUOp::create(
          b, loc, value, resize(b, loc, ref.range.offset, storageBits));
      value = comb::AndOp::create(
          b, loc,
          ValueRange{value, mask(b, loc, ref.range.available, storageBits)},
          false);
      value = resize(b, loc, value, storageWidth(probe.getType()));
      if (value.getType() != probe.getType())
        value = bitcast(b, loc, probe.getType(), value);
      probe.replaceAllUsesWith(value);
      probe.erase();
    }
    for (auto drive : drives) {
      OpBuilder b(drive);
      auto loc = drive.getLoc();
      auto ref = resolve(drive.getSignal());
      Value old = LLVM::LoadOp::create(b, loc, storage, ref.pointer);
      auto lowMask = mask(b, loc, ref.range.available, storageBits);
      auto offset = resize(b, loc, ref.range.offset, storageBits);
      Value incoming = resize(b, loc, drive.getValue(), storageBits);
      incoming =
          comb::AndOp::create(b, loc, ValueRange{incoming, lowMask}, false);
      incoming = comb::ShlOp::create(b, loc, incoming, offset);
      Value writtenMask = comb::ShlOp::create(b, loc, lowMask, offset);
      auto ones =
          hw::ConstantOp::create(b, loc, APInt::getAllOnes(storageBits));
      auto keep =
          comb::XorOp::create(b, loc, ValueRange{writtenMask, ones}, false);
      Value value = comb::AndOp::create(b, loc, ValueRange{old, keep}, false);
      value = comb::OrOp::create(b, loc, ValueRange{value, incoming}, false);
      if (drive.getEnable())
        value = comb::MuxOp::create(b, loc, drive.getEnable(), value, old);
      LLVM::StoreOp::create(b, loc, value, ref.pointer);
      drive.erase();
    }
    for (auto &[block, arguments] : phis) {
      SmallVector<unsigned> indices;
      for (auto argument : arguments)
        indices.push_back(argument.getArgNumber());
      llvm::sort(indices, std::greater<unsigned>());
      DenseSet<Operation *> predecessors;
      for (auto *pred : block->getPredecessors())
        predecessors.insert(pred->getTerminator());
      for (auto *op : predecessors) {
        auto branch = cast<BranchOpInterface>(op);
        for (unsigned s = 0; s < op->getNumSuccessors(); ++s)
          if (op->getSuccessor(s) == block)
            for (auto index : indices)
              branch.getSuccessorOperands(s).erase(index);
      }
    }
    for (auto *alias : aliases)
      alias->dropAllReferences();
    for (auto *alias : aliases)
      alias->erase();
    for (auto &[block, arguments] : phis) {
      llvm::BitVector removed(block->getNumArguments());
      for (auto argument : arguments)
        removed.set(argument.getArgNumber());
      block->eraseArguments(removed);
    }
    for (auto root : roots) {
      consumed.insert(root.getOperation());
      root.erase();
    }
    return true;
  }

  void runOnOperation() override {
    SmallVector<llhd::SignalOp> signals;
    getOperation().walk([&](llhd::SignalOp op) {
      if (isa<llhd::CombinationalOp, func::FuncOp>(op->getParentOp()))
        signals.push_back(op);
    });
    unsigned count = 0;
    for (auto root : signals)
      if (!consumed.contains(root.getOperation()) && lower(root))
        ++count;
    if (count)
      llvm::errs()
          << "Lowered " << count
          << " automatic local reference components to stack storage\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardLocalMemory", "1",
          [] { PassRegistration<LocalMemory>(); }};
}
