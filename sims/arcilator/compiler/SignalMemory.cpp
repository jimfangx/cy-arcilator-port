// Preserve procedural reference accesses that cannot become SSA registers.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LLHD/LLHDOps.h"
#include "circt/Dialect/Sim/SimOps.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include <functional>
#include <optional>

using namespace mlir;
using namespace circt;

namespace {
struct SignalMemory : PassWrapper<SignalMemory, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SignalMemory)
  SignalMemory() = default;
  SignalMemory(const SignalMemory &other) : PassWrapper(other) {}
  Option<bool> exports{*this, "exports", llvm::cl::init(false)};
  Option<bool> capturesOnly{*this, "captures-only", llvm::cl::init(false)};
  StringRef getArgument() const override {
    return "chipyard-native-signal-memory";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry
        .insert<hw::HWDialect, comb::CombDialect, llhd::LLHDDialect,
                sim::SimDialect, func::FuncDialect, LLVM::LLVMDialect,
                cf::ControlFlowDialect, ub::UBDialect, arith::ArithDialect>();
  }
  struct Range {
    Value offset, available;
  };
  DenseMap<unsigned, hw::HWModuleExternOp> declarations;
  sim::DPIFuncOp readFunction, writeFunction;
  unsigned count = 0;
  DenseSet<Operation *> formalRoots;

  Value constant(OpBuilder &b, Location loc, uint64_t value) {
    return hw::ConstantOp::create(b, loc, APInt(64, value));
  }
  // Real DPI values occupy their IEEE representation in the same bounded
  // signal storage used for packed values. HW bitcasts exclude floating types.
  static int64_t storageWidth(Type type) {
    if (type.isF32() || type.isF64())
      return cast<FloatType>(type).getWidth();
    return hw::getBitWidth(type);
  }
  static Type exportInputType(Type type) {
    // Packet loads use integer storage for unpacked aggregate values. Keep the
    // original array type inside the SV body, where its indexing is defined.
    if (isa<hw::ArrayType>(type))
      return IntegerType::get(type.getContext(), storageWidth(type));
    return type;
  }
  static Value bitcast(OpBuilder &b, Location loc, Type type, Value value) {
    if (type == value.getType())
      return value;
    if (isa<FloatType>(type) || isa<FloatType>(value.getType()))
      return b.createOrFold<arith::BitcastOp>(loc, type, value);
    return b.createOrFold<hw::BitcastOp>(loc, type, value);
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
    return {comb::AddOp::create(b, loc, ValueRange{parent.offset, low}, false),
            remaining};
  }
  sim::DPIFuncOp function(StringRef name, unsigned inputs, bool returns) {
    auto top = getOperation();
    OpBuilder b(top.getBodyRegion());
    b.setInsertionPointToStart(top.getBody());
    circt::Namespace names;
    names.add(top);
    SmallVector<sim::DPIArgument> arguments;
    SmallVector<Attribute> abi;
    for (unsigned i = 0; i < inputs + unsigned(returns); ++i) {
      bool result = i == inputs;
      arguments.push_back(
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
        b, top.getLoc(), names.newName(name), b.getStringAttr("private"),
        sim::DPIFunctionType::get(&getContext(), arguments), ArrayAttr{},
        b.getStringAttr(name));
    fn->setAttr("chipyard.dpi_abi", b.getArrayAttr(abi));
    fn->setAttr(
        "chipyard.dpi_decl",
        b.getDictionaryAttr({b.getNamedAttr("c_name", b.getStringAttr(name)),
                             b.getNamedAttr("context", b.getBoolAttr(false)),
                             b.getNamedAttr("pure", b.getBoolAttr(false)),
                             b.getNamedAttr("task", b.getBoolAttr(false))}));
    return fn;
  }
  struct Reference {
    Value handle, version;
    Range range;
  };
  DenseSet<Operation *> consumed;
  DenseMap<Operation *, Reference> argumentReferences;
  SmallVector<llhd::SignalOp> argumentRoots;

  std::optional<APInt> initialValue(llhd::SignalOp signal) {
    auto width = storageWidth(signal.getType().getNestedType());
    if (width <= 0 || width > IntegerType::kMaxWidth)
      return {};
    APInt initial(width, 0);
    if (auto init = signal.getInit()) {
      Attribute attr;
      if (!matchPattern(init, m_Constant(&attr)))
        return {};
      if (auto integer = dyn_cast<IntegerAttr>(attr))
        initial = integer.getValue();
      else if (auto floating = dyn_cast<FloatAttr>(attr))
        initial = floating.getValue().bitcastToAPInt();
      else {
        auto array = dyn_cast<ArrayAttr>(attr);
        if (!array ||
            failed(hw::aggregateAttrToAPInt(init.getType(), array, initial)))
          return {};
      }
    }
    return initial;
  }

  Reference allocate(llhd::SignalOp signal, const APInt &initial) {
    unsigned width = initial.getBitWidth();
    if (!readFunction)
      readFunction = function("chipyard_arc_signal_read", 5, true);
    if (!writeFunction)
      writeFunction = function("chipyard_arc_signal_write", 8, false);
    if (auto found = argumentReferences.find(signal.getOperation());
        found != argumentReferences.end())
      return found->second;
    auto top = getOperation();
    auto loc = signal.getLoc();
    OpBuilder b(signal);
    auto declaration = declarations.lookup(width);
    if (!declaration) {
      OpBuilder outer(top.getBodyRegion());
      outer.setInsertionPointToStart(top.getBody());
      circt::Namespace names;
      names.add(top);
      SmallVector<hw::PortInfo> ports{
          {{outer.getStringAttr("handle"), outer.getI64Type(),
            hw::ModulePort::Output},
           0},
          {{outer.getStringAttr("version"), outer.getI64Type(),
            hw::ModulePort::Output},
           1}};
      declaration = hw::HWModuleExternOp::create(
          outer, loc,
          outer.getStringAttr(
              names.newName("__ArcSignalMemory_i" + Twine(width))),
          ports, "ArcSignalMemory",
          outer.getArrayAttr(
              {hw::ParamDeclAttr::get("BITS", outer.getI32Type()),
               hw::ParamDeclAttr::get("INIT", outer.getIntegerType(width)),
               hw::ParamDeclAttr::get("KEY",
                                      hw::StringType::get(&getContext())),
               hw::ParamDeclAttr::get("CAPTURES",
                                      hw::StringType::get(&getContext())),
               hw::ParamDeclAttr::get("PATH",
                                      hw::StringType::get(&getContext()))}));
      declaration.setPrivate();
      declarations[width] = declaration;
    }
    llvm::json::Array captured;
    if (auto entries =
            signal->getAttrOfType<ArrayAttr>("chipyard.dpi_captures"))
      for (auto attr : entries) {
        auto entry = cast<DictionaryAttr>(attr);
        auto fn = getOperation().lookupSymbol<func::FuncOp>(
            entry.getAs<FlatSymbolRefAttr>("function").getValue());
        if (!fn || !fn->hasAttr("chipyard.export_id"))
          continue;
        auto string = [&](StringRef key, StringRef fallback) -> std::string {
          auto value = entry.getAs<StringAttr>(key);
          return (value ? value.getValue() : fallback).str();
        };
        captured.push_back(llvm::json::Object{
            {"kernel", fn->getAttrOfType<StringAttr>("chipyard.export_id")
                           .getValue()
                           .str()},
            {"capture", entry.getAs<IntegerAttr>("capture").getInt()},
            {"instance", string("instance", "")},
            {"scope", string("scope", "")},
            {"storage", string("storage", signal.getName().value_or(""))}});
      }
    std::string captureText;
    llvm::raw_string_ostream captureStream(captureText);
    captureStream << llvm::formatv("{0}",
                                   llvm::json::Value(std::move(captured)));
    captureStream.flush();
    auto stringType = hw::StringType::get(&getContext());
    auto parameters = b.getArrayAttr(
        {hw::ParamDeclAttr::get("BITS", b.getI32IntegerAttr(width)),
         hw::ParamDeclAttr::get(
             "INIT", b.getIntegerAttr(b.getIntegerType(width), initial)),
         hw::ParamDeclAttr::get(
             "KEY",
             StringAttr::get(
                 signal->getAttrOfType<StringAttr>("chipyard.global_key")
                     ? signal->getAttrOfType<StringAttr>("chipyard.global_key")
                           .getValue()
                     : StringRef{},
                 hw::StringType::get(&getContext()))),
         hw::ParamDeclAttr::get("CAPTURES",
                                StringAttr::get(captureText, stringType)),
         hw::ParamDeclAttr::get(
             "PATH",
             StringAttr::get(signal.getName().value_or(""), stringType))});
    auto instance = hw::InstanceOp::create(
        b, loc, declaration, "signal_memory_" + std::to_string(count++),
        SmallVector<Value>{}, parameters);
    return {instance.getResult(0),
            instance.getResult(1),
            {constant(b, loc, 0), constant(b, loc, width)}};
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
  bool lower(llhd::SignalOp signal) {
    auto initial = initialValue(signal);
    if (!initial)
      return false;
    SmallVector<Value> references{signal.getResult()};
    DenseSet<Value> seen{signal.getResult()};
    SmallVector<llhd::ProbeOp> probes;
    SmallVector<llhd::DriveOp> drives;
    // Validate the entire reference tree before changing any operation.
    for (unsigned i = 0; i < references.size(); ++i) {
      for (auto *user : references[i].getUsers()) {
        if (auto probe = dyn_cast<llhd::ProbeOp>(user))
          probes.push_back(probe);
        else if (auto drive = dyn_cast<llhd::DriveOp>(user)) {
          llhd::TimeAttr time;
          if (!matchPattern(drive.getTime(), m_Constant(&time)) ||
              time.getTime() ||
              !((time.getDelta() == 0 && time.getEpsilon() <= 1) ||
                (time.getDelta() == 1 && time.getEpsilon() == 0)))
            return false;
          if (formalRoots.contains(signal.getOperation()) && time.getDelta())
            return false;
          drives.push_back(drive);
        } else if (isPackedSlice(user) ||
                   isa<llhd::SigArrayGetOp, llhd::SigArraySliceOp,
                       llhd::SigStructExtractOp, llhd::SigExtractOp>(user)) {
          auto ty =
              cast<llhd::RefType>(user->getResult(0).getType()).getNestedType();
          if (storageWidth(ty) <= 0)
            return false;
          if (auto field = dyn_cast<llhd::SigStructExtractOp>(user))
            if (!isa<hw::StructType>(
                    cast<llhd::RefType>(field.getInput().getType())
                        .getNestedType()))
              return false;
          if (seen.insert(user->getResult(0)).second)
            references.push_back(user->getResult(0));
        } else
          return false;
      }
    }
    auto allocated = allocate(signal, *initial);
    Value handle = allocated.handle, version = allocated.version;
    OpBuilder b(signal);
    auto loc = signal.getLoc();
    DenseMap<Value, Range> ranges;
    ranges[signal] = allocated.range;
    for (auto reference : llvm::drop_begin(references)) {
      auto *alias = reference.getDefiningOp();
      b.setInsertionPoint(alias);
      auto parent = ranges.lookup(alias->getOperand(0));
      auto type = cast<llhd::RefType>(reference.getType()).getNestedType();
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
      ranges[reference] =
          subrange(b, loc, parent, low, storageWidth(type), valid);
    }
    for (auto probe : probes) {
      b.setInsertionPoint(probe);
      auto range = ranges.lookup(probe.getSignal());
      auto bits = storageWidth(probe.getType());
      SmallVector<Value> chunks;
      Value observedVersion = version;
      if (auto region = probe->getParentOfType<llhd::CombinationalOp>())
        if (llvm::any_of(drives, [&](auto drive) {
              return drive->template getParentOfType<llhd::CombinationalOp>() ==
                     region;
            }))
          observedVersion = constant(b, loc, 0);
      for (int64_t offset = 0; offset < bits; offset += 64) {
        auto part = subrange(b, loc, range, constant(b, loc, offset),
                             std::min<int64_t>(64, bits - offset));
        auto call = sim::DPICallOp::create(
            b, loc, TypeRange{b.getI64Type()},
            FlatSymbolRefAttr::get(readFunction), Value{}, Value{},
            ValueRange{handle, observedVersion, part.offset, part.available,
                       constant(b, loc, std::min<int64_t>(64, bits - offset))});
        Value value = call.getResult(0);
        if (bits - offset < 64)
          value = comb::ExtractOp::create(b, loc, value, 0, bits - offset);
        chunks.push_back(value);
      }
      std::reverse(chunks.begin(), chunks.end());
      Value value = chunks.size() == 1
                        ? chunks[0]
                        : comb::ConcatOp::create(b, loc, chunks).getResult();
      if (value.getType() != probe.getType())
        value = bitcast(b, loc, probe.getType(), value);
      probe.replaceAllUsesWith(value);
      probe.erase();
    }
    for (auto drive : drives) {
      b.setInsertionPoint(drive);
      auto range = ranges.lookup(drive.getSignal());
      auto bits = storageWidth(drive.getValue().getType());
      Value value = drive.getValue();
      if (!isa<IntegerType>(value.getType()))
        value = bitcast(b, loc, b.getIntegerType(bits), value);
      llhd::TimeAttr time;
      (void)matchPattern(drive.getTime(), m_Constant(&time));
      Value enabled = drive.getEnable() ? word(b, loc, drive.getEnable())
                                        : constant(b, loc, 1);
      for (int64_t offset = 0; offset < bits; offset += 64) {
        auto n = std::min<int64_t>(64, bits - offset);
        auto part = subrange(b, loc, range, constant(b, loc, offset), n);
        Value chunk = value;
        if (bits != n)
          chunk = comb::ExtractOp::create(b, loc, value, offset, n);
        sim::DPICallOp::create(
            b, loc, TypeRange{}, FlatSymbolRefAttr::get(writeFunction), Value{},
            Value{},
            ValueRange{handle, constant(b, loc, 0), part.offset, part.available,
                       constant(b, loc, n), word(b, loc, chunk),
                       constant(b, loc, time.getDelta() == 1), enabled});
      }
      drive.erase();
    }
    consumed.insert(signal.getOperation());
    for (auto reference : llvm::reverse(references))
      reference.getDefiningOp()->erase();
    return true;
  }
  // Lower a connected reference component, including references transported
  // through CFG edges. Every reference becomes a handle, version and bounded
  // bit range; no native reference type crosses a branch afterward.
  bool lowerCFG(llhd::SignalOp first) {
    SmallVector<Value> references{first.getResult()};
    DenseSet<Value> seen{first.getResult()};
    SmallVector<llhd::SignalOp> roots;
    DenseMap<Operation *, APInt> initializers;
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
        if (block->hasNoPredecessors())
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
        auto initial = initialValue(root);
        if (!initial)
          return false;
        roots.push_back(root);
        initializers.try_emplace(root.getOperation(), *initial);
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
        if (auto probe = dyn_cast<llhd::ProbeOp>(user)) {
          probes.push_back(probe);
        } else if (auto drive = dyn_cast<llhd::DriveOp>(user)) {
          llhd::TimeAttr time;
          if (drive.getSignal() != ref ||
              !matchPattern(drive.getTime(), m_Constant(&time)) ||
              time.getTime() ||
              !((time.getDelta() == 0 && time.getEpsilon() <= 1) ||
                (time.getDelta() == 1 && time.getEpsilon() == 0)))
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
    if (phis.empty())
      return false;
    // Track the possible roots of loop-carried and merged references. A
    // combinational process excludes only memories it can write from its
    // implicit sensitivity; a read-only root merged with a written one must
    // still observe changes to the read-only memory.
    DenseMap<Value, DenseSet<Operation *>> origins;
    for (auto ref : references)
      origins.try_emplace(ref);
    for (auto root : roots)
      origins[root].insert(root.getOperation());
    bool changed;
    do {
      changed = false;
      for (auto ref : references) {
        SmallVector<Value> inputs;
        if (auto arg = dyn_cast<BlockArgument>(ref)) {
          auto *block = arg.getOwner();
          for (auto *pred : block->getPredecessors()) {
            auto branch = cast<BranchOpInterface>(pred->getTerminator());
            for (unsigned s = 0; s < branch->getNumSuccessors(); ++s)
              if (branch->getSuccessor(s) == block)
                inputs.push_back(
                    branch.getSuccessorOperands(s)[arg.getArgNumber()]);
          }
        } else if (isAlias(ref.getDefiningOp())) {
          inputs.push_back(ref.getDefiningOp()->getOperand(0));
        }
        for (auto input : inputs)
          for (auto *root : origins[input])
            changed |= origins[ref].insert(root).second;
      }
    } while (changed);
    for (auto drive : drives) {
      llhd::TimeAttr time;
      (void)matchPattern(drive.getTime(), m_Constant(&time));
      if (time.getDelta())
        for (auto *root : origins[drive.getSignal()])
          if (formalRoots.contains(root))
            return false;
    }
    DenseMap<Operation *, DenseSet<Operation *>> written;
    for (auto drive : drives)
      if (auto region = drive->getParentOfType<llhd::CombinationalOp>())
        for (auto *root : origins[drive.getSignal()])
          written[region].insert(root);
    DenseMap<Value, Reference> resolved;
    for (auto root : roots)
      resolved[root] = allocate(root, initializers.lookup(root.getOperation()));
    // New arguments exist before resolving aliases, so loop-carried reference
    // ranges can refer to their incoming iteration without recursive cloning.
    auto i64 = IntegerType::get(&getContext(), 64);
    for (auto &[block, arguments] : phis)
      for (auto argument : arguments) {
        auto loc = argument.getLoc();
        auto handle = block->addArgument(i64, loc);
        auto version = block->addArgument(i64, loc);
        auto offset = block->addArgument(i64, loc);
        auto available = block->addArgument(i64, loc);
        resolved[argument] = {handle, version, {offset, available}};
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
      return resolved[ref] = {parent.handle, parent.version, range};
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
            values.append({ref.handle, ref.version, ref.range.offset,
                           ref.range.available});
          }
          branch.getSuccessorOperands(s).append(values);
        }
      }
    }
    for (auto probe : probes) {
      OpBuilder b(probe);
      auto loc = probe.getLoc();
      auto ref = resolve(probe.getSignal());
      auto bits = storageWidth(probe.getType());
      SmallVector<Value> chunks;
      Value observedVersion = ref.version;
      if (auto region = probe->getParentOfType<llhd::CombinationalOp>()) {
        for (auto root : roots) {
          if (!written[region].contains(root.getOperation()) ||
              !origins[probe.getSignal()].contains(root.getOperation()))
            continue;
          auto same = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::eq,
                                           ref.handle, resolved[root].handle);
          observedVersion = comb::MuxOp::create(
              b, loc, same, constant(b, loc, 0), observedVersion);
        }
      }
      for (int64_t offset = 0; offset < bits; offset += 64) {
        auto n = std::min<int64_t>(64, bits - offset);
        auto part = subrange(b, loc, ref.range, constant(b, loc, offset), n);
        auto call = sim::DPICallOp::create(
            b, loc, TypeRange{b.getI64Type()},
            FlatSymbolRefAttr::get(readFunction), Value{}, Value{},
            ValueRange{ref.handle, observedVersion, part.offset, part.available,
                       constant(b, loc, n)});
        Value value = call.getResult(0);
        if (n < 64)
          value = comb::ExtractOp::create(b, loc, value, 0, n);
        chunks.push_back(value);
      }
      std::reverse(chunks.begin(), chunks.end());
      Value value = chunks.size() == 1
                        ? chunks[0]
                        : comb::ConcatOp::create(b, loc, chunks).getResult();
      if (value.getType() != probe.getType())
        value = bitcast(b, loc, probe.getType(), value);
      probe.replaceAllUsesWith(value);
      probe.erase();
    }
    for (auto drive : drives) {
      OpBuilder b(drive);
      auto loc = drive.getLoc();
      auto ref = resolve(drive.getSignal());
      auto bits = storageWidth(drive.getValue().getType());
      Value value = drive.getValue();
      if (!isa<IntegerType>(value.getType()))
        value = bitcast(b, loc, b.getIntegerType(bits), value);
      llhd::TimeAttr time;
      (void)matchPattern(drive.getTime(), m_Constant(&time));
      Value enabled = drive.getEnable() ? word(b, loc, drive.getEnable())
                                        : constant(b, loc, 1);
      for (int64_t offset = 0; offset < bits; offset += 64) {
        auto n = std::min<int64_t>(64, bits - offset);
        auto part = subrange(b, loc, ref.range, constant(b, loc, offset), n);
        Value chunk = value;
        if (bits != n)
          chunk = comb::ExtractOp::create(b, loc, value, offset, n);
        sim::DPICallOp::create(
            b, loc, TypeRange{}, FlatSymbolRefAttr::get(writeFunction), Value{},
            Value{},
            ValueRange{ref.handle, constant(b, loc, 0), part.offset,
                       part.available, constant(b, loc, n), word(b, loc, chunk),
                       constant(b, loc, time.getDelta() == 1), enabled});
      }
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

  // These references must become shared native storage, never independent
  // per-instance registers. Every imported variant uses the qualified key.
  SmallVector<llhd::SignalOp> globalRoots;
  void materializeGlobals() {
    auto top = getOperation();
    SmallVector<llhd::GlobalSignalOp> globals;
    top.walk([&](llhd::GlobalSignalOp op) { globals.push_back(op); });
    for (auto global : globals) {
      SmallVector<llhd::GetGlobalSignalOp> gets;
      bool supported = true;
      top.walk([&](llhd::GetGlobalSignalOp get) {
        if (SymbolTable::lookupNearestSymbolFrom<llhd::GlobalSignalOp>(
                get, get.getGlobalNameAttr()) != global)
          return;
        if (!get->getParentOfType<hw::HWModuleOp>())
          supported = false;
        gets.push_back(get);
      });
      if (!supported)
        continue;
      auto width = storageWidth(global.getType());
      if (width <= 0 || width > IntegerType::kMaxWidth)
        continue;
      APInt initial(width, 0);
      if (!global.getInitRegion().empty()) {
        // A folded result does not make initializer side effects disposable.
        // Keep runtime initialization intact until it has its own lowering.
        if (llvm::any_of(global.getInitBlock()->without_terminator(),
                         [](Operation &op) { return !isPure(&op); }))
          continue;
        auto yield =
            dyn_cast<llhd::YieldOp>(global.getInitBlock()->getTerminator());
        Attribute attr;
        if (!yield || yield->getNumOperands() != 1 ||
            !matchPattern(yield->getOperand(0), m_Constant(&attr)))
          continue;
        if (auto integer = dyn_cast<IntegerAttr>(attr))
          initial = integer.getValue();
        else if (auto floating = dyn_cast<FloatAttr>(attr))
          initial = floating.getValue().bitcastToAPInt();
        else if (auto array = dyn_cast<ArrayAttr>(attr)) {
          if (failed(
                  hw::aggregateAttrToAPInt(global.getType(), array, initial)))
            continue;
        } else
          continue;
      }
      DenseMap<Operation *, llhd::SignalOp> roots;
      for (auto get : gets) {
        auto module = get->getParentOfType<hw::HWModuleOp>();
        auto &root = roots[module];
        if (!root) {
          OpBuilder b(module.getBodyBlock(), module.getBodyBlock()->begin());
          auto loc = global.getLoc();
          Value value = hw::ConstantOp::create(b, loc, initial);
          if (value.getType() != global.getType())
            value = bitcast(b, loc, global.getType(), value);
          root = llhd::SignalOp::create(
              b, loc, llhd::RefType::get(global.getType()),
              b.getStringAttr(global.getSymName()), value);
          root->setAttr("chipyard.global_key",
                        b.getStringAttr(global.getSymName()));
          globalRoots.push_back(root);
        }
        get.replaceAllUsesWith(root.getResult());
        get.erase();
      }
      if (SymbolTable::symbolKnownUseEmpty(global, top))
        global.erase();
    }
  }

  LogicalResult outlineExportPrints(func::FuncOp fn) {
    // A formatted value describes a compile-time expression tree. CFG
    // canonicalization may merge different diagnostic blocks by passing that
    // opaque value as a block argument, which the SDK cannot lower. Keep the
    // expression in a private printing function; only its runtime leaves cross
    // the call boundary. Later LLVM optimizations can inline the lowered call.
    SmallVector<sim::PrintFormattedProcOp> prints;
    fn.walk([&](sim::PrintFormattedProcOp print) { prints.push_back(print); });
    auto top = getOperation();
    circt::Namespace names;
    names.add(top);
    for (auto print : prints) {
      SmallVector<Operation *> expressions;
      llvm::SetVector<Value> inputs;
      llvm::SmallPtrSet<Operation *, 16> seen;
      std::function<LogicalResult(Value)> gather =
          [&](Value value) -> LogicalResult {
        if (!isa<sim::FormatStringType>(value.getType())) {
          inputs.insert(value);
          return success();
        }
        auto *op = value.getDefiningOp();
        if (!op || !isMemoryEffectFree(op) || op->getNumRegions())
          return print.emitError(
              "exported print requires a local format expression tree");
        if (!seen.insert(op).second)
          return success();
        for (Value operand : op->getOperands())
          if (failed(gather(operand)))
            return failure();
        expressions.push_back(op);
        return success();
      };
      for (Value operand : print->getOperands())
        if (failed(gather(operand)))
          return failure();
      OpBuilder b(top.getBody(), top.getBody()->end());
      SmallVector<Type> types;
      for (Value input : inputs)
        types.push_back(input.getType());
      auto helper = func::FuncOp::create(
          b, print.getLoc(), names.newName("__chipyard_export_print"),
          b.getFunctionType(types, {}));
      helper.setPrivate();
      auto *entry = helper.addEntryBlock();
      IRMapping mapping;
      for (auto [input, argument] : llvm::zip(inputs, entry->getArguments()))
        mapping.map(input, argument);
      b.setInsertionPointToEnd(entry);
      for (auto *op : expressions)
        b.clone(*op, mapping);
      b.clone(*print, mapping);
      func::ReturnOp::create(b, print.getLoc());
      b.setInsertionPoint(print);
      func::CallOp::create(b, print.getLoc(), helper, inputs.getArrayRef());
      print.erase();
    }
    return success();
  }

  // Export bodies have no lexical Arc model context. Read the active native
  // model's physical clock, then retain the frontend's lexical unit conversion.
  // Follow ordinary SV helper calls too; a shared helper can contain $realtime.
  void lowerExportTimes(func::FuncOp fn, DenseSet<Operation *> &visited) {
    if (!visited.insert(fn.getOperation()).second)
      return;
    SmallVector<llhd::CurrentTimeOp> times;
    fn.walk([&](llhd::CurrentTimeOp op) { times.push_back(op); });
    if (!times.empty()) {
      auto clock = function("chipyard_arc_export_time", 0, true);
      for (auto time : times) {
        OpBuilder b(time);
        auto value = sim::DPICallOp::create(
            b, time.getLoc(), TypeRange{b.getI64Type()},
            FlatSymbolRefAttr::get(clock), Value{}, Value{}, ValueRange{})
                         .getResult(0);
        auto physical = llhd::IntToTimeOp::create(b, time.getLoc(), value);
        time.replaceAllUsesWith(physical.getResult());
        time.erase();
      }
    }
    SmallVector<func::FuncOp> callees;
    fn.walk([&](func::CallOp call) {
      if (auto callee = SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(
              call, call.getCalleeAttr()))
        callees.push_back(callee);
    });
    for (auto callee : callees)
      lowerExportTimes(callee, visited);
  }

  void lowerExportControls(func::FuncOp fn) {
    SmallVector<Operation *> effects;
    fn.walk([&](Operation *op) {
      if (auto print = dyn_cast<sim::PrintFormattedProcOp>(op)) {
        if (print->hasAttr("chipyard.file_output") ||
            (print.getStream() &&
             print.getStream().getDefiningOp<sim::SVChannelToOutputStreamOp>()))
          return;
        effects.push_back(op);
      } else if (auto call = dyn_cast<func::CallOp>(op);
                 call && call->hasAttr("chipyard.native_print")) {
        if (!call->hasAttr("chipyard.file_output"))
          effects.push_back(op);
      } else if (isa<sim::TerminateOp>(op))
        effects.push_back(op);
    });
    if (effects.empty())
      return;
    auto control = function("chipyard_arc_runtime_control", 1, true);
    sim::DPIFuncOp stop, finish;
    for (auto *op : effects) {
      OpBuilder b(op);
      auto loc = op->getLoc();
      bool terminating = isa<sim::TerminateOp>(op);
      auto which = constant(b, loc, terminating ? 1 : 0);
      auto active = sim::DPICallOp::create(b, loc, TypeRange{b.getI64Type()},
                                           FlatSymbolRefAttr::get(control),
                                           Value{}, Value{}, ValueRange{which})
                        .getResult(0);
      auto enabled = comb::ICmpOp::create(b, loc, comb::ICmpPredicate::ne,
                                          active, constant(b, loc, 0));
      auto *origin = op->getBlock();
      auto *continuation = origin->splitBlock(std::next(op->getIterator()));
      auto *perform = new Block();
      origin->getParent()->getBlocks().insert(continuation->getIterator(),
                                              perform);
      b.setInsertionPoint(op);
      cf::CondBranchOp::create(b, loc, enabled, perform, ValueRange{},
                               continuation, ValueRange{});
      if (auto term = dyn_cast<sim::TerminateOp>(op)) {
        bool success = term.getSuccess();
        auto &helper = success ? finish : stop;
        if (!helper)
          helper = function(success ? "chipyard_arc_export_finish"
                                    : "chipyard_arc_stop_fail",
                            0, false);
        b.setInsertionPointToEnd(perform);
        sim::DPICallOp::create(b, loc, TypeRange{},
                               FlatSymbolRefAttr::get(helper), Value{}, Value{},
                               ValueRange{});
        term.erase();
      } else {
        op->moveBefore(perform, perform->end());
      }
      b.setInsertionPointToEnd(perform);
      cf::BranchOp::create(b, loc, continuation);
    }
  }

  LogicalResult prepareExports() {
    SmallVector<llhd::CoroutineOp> tasks;
    getOperation().walk([&](llhd::CoroutineOp task) {
      if (task->hasAttr("circt.dpi.export"))
        tasks.push_back(task);
    });
    for (auto task : tasks) {
      bool suspends = false;
      task.walk([&](Operation *op) {
        if (isa<llhd::WaitOp, llhd::CallCoroutineOp>(op))
          suspends = true;
      });
      if (suspends)
        return task.emitError(
            "suspending DPI exports require a scheduler call frame");
      auto metadata =
          task->getAttrOfType<DictionaryAttr>("chipyard.dpi_export");
      if (!metadata)
        return task.emitError(
            "native DPI task export requires original ABI metadata");
      if (task.getFunctionType().getNumResults())
        return task.emitError(
            "native DPI task completion must have no value results");
      OpBuilder b(task);
      // The C ABI of an exported task returns int completion status, even
      // though an SV task itself has no value result (IEEE1800 35.8).
      auto sourceABI = metadata.getAs<ArrayAttr>("abi");
      if (!sourceABI)
        return task.emitError("exported task lacks original ABI");
      auto fn = func::FuncOp::create(
          b, task.getLoc(), task.getSymName(),
          b.getFunctionType(task.getFunctionType().getInputs(),
                            {b.getI32Type()}));
      for (auto attribute : task->getAttrs())
        if (attribute.getName() != "sym_name" &&
            attribute.getName() != "function_type" &&
            attribute.getName() != "sym_visibility")
          fn->setAttr(attribute.getName(), attribute.getValue());
      fn.setSymVisibilityAttr(task.getSymVisibilityAttr());
      NamedAttrList contract(metadata);
      contract.set("task_mode", b.getStringAttr("synchronous"));
      contract.set("original_task_abi", sourceABI);
      SmallVector<Attribute> taskABI(sourceABI.begin(), sourceABI.end());
      taskABI.push_back(b.getDictionaryAttr(
          {b.getNamedAttr("direction", b.getStringAttr("return")),
           b.getNamedAttr("kind", b.getStringAttr("int")),
           b.getNamedAttr("width", b.getI64IntegerAttr(32)),
           b.getNamedAttr("signed", b.getBoolAttr(true)),
           b.getNamedAttr("four_state", b.getBoolAttr(false)),
           b.getNamedAttr("native_task_completion", b.getBoolAttr(true))}));
      contract.set("abi", b.getArrayAttr(taskABI));
      fn->setAttr("chipyard.dpi_export", contract.getDictionary(&getContext()));
      fn.getBody().takeBody(task.getBody());
      SmallVector<llhd::ReturnOp> returns;
      fn.walk([&](llhd::ReturnOp op) { returns.push_back(op); });
      for (auto op : returns) {
        OpBuilder at(op);
        auto complete = hw::ConstantOp::create(at, op.getLoc(), APInt(32, 0));
        func::ReturnOp::create(at, op.getLoc(), ValueRange{complete});
        op.erase();
      }
      // A nested task's protocol failure must return to C without executing
      // undefined machine code or the SV task's normal continuation. The C
      // caller receives nonzero and propagates it through its task return.
      SmallVector<ub::UnreachableOp> aborted;
      fn.walk([&](ub::UnreachableOp op) {
        if (llvm::any_of(*op->getBlock(), [](Operation &previous) {
              return isa<sim::TerminateOp>(previous);
            }))
          aborted.push_back(op);
      });
      for (auto op : aborted) {
        OpBuilder at(op);
        auto failure = hw::ConstantOp::create(at, op.getLoc(), APInt(32, 1));
        func::ReturnOp::create(at, op.getLoc(), ValueRange{failure});
        op.erase();
      }
      task.erase();
    }
    SmallVector<func::FuncOp> functions;
    bool unsupported = false;
    getOperation().walk([&](Operation *op) {
      if (!op->hasAttr("circt.dpi.export"))
        return;
      if (auto fn = dyn_cast<func::FuncOp>(op))
        functions.push_back(fn);
      else {
        op->emitError(
            "native DPI exported tasks require a suspension/string binding");
        unsupported = true;
      }
    });
    if (unsupported)
      return failure();
    DenseSet<Operation *> timeFunctions;
    for (auto fn : functions) {
      auto metadata = fn->getAttrOfType<DictionaryAttr>("chipyard.dpi_export");
      auto formalCount = metadata ? metadata.getAs<IntegerAttr>("formal_count")
                                  : IntegerAttr{};
      if (!formalCount)
        return fn.emitError("native DPI export requires original ABI metadata");
      if (fn.isExternal())
        return fn.emitError("export needs an SV body");
      lowerExportTimes(fn, timeFunctions);
      lowerExportControls(fn);
      if (failed(outlineExportPrints(fn)))
        return failure();
      auto original = fn.getFunctionType();
      for (auto type : original.getInputs()) {
        if (auto ref = dyn_cast<llhd::RefType>(type)) {
          auto width = storageWidth(ref.getNestedType());
          if (width <= 0 || width > IntegerType::kMaxWidth)
            return fn.emitError("unsupported exported reference type");
        } else if (!isa<IntegerType>(type) && !type.isF32() && !type.isF64()) {
          auto width = storageWidth(type);
          if (!isa<hw::ArrayType>(type) || width <= 0 ||
              width > IntegerType::kMaxWidth)
            return fn.emitError("unsupported exported value type");
        }
      }
      if (original.getNumResults() > 1 ||
          llvm::any_of(original.getResults(), [](Type type) {
            return !isa<IntegerType>(type) && !type.isF32() && !type.isF64();
          }))
        return fn.emitError("unsupported exported return type");
      // Capture widths come from typed references, rather than the integral
      // width field (which is absent for real and shortreal in the source ABI).
      if (auto captures = metadata.getAs<ArrayAttr>("captures")) {
        SmallVector<Attribute> updated;
        for (auto attr : captures) {
          auto capture = cast<DictionaryAttr>(attr);
          auto index = capture.getAs<IntegerAttr>("argument").getInt();
          if (index < 0 || uint64_t(index) >= original.getNumInputs())
            return fn.emitError("invalid exported capture argument");
          auto ref = dyn_cast<llhd::RefType>(original.getInput(index));
          if (!ref)
            return fn.emitError("exported capture requires a reference");
          NamedAttrList fields(capture);
          fields.set("width",
                     IntegerAttr::get(IntegerType::get(&getContext(), 64),
                                      storageWidth(ref.getNestedType())));
          updated.push_back(fields.getDictionary(&getContext()));
        }
        NamedAttrList fields(metadata);
        fields.set("captures", ArrayAttr::get(&getContext(), updated));
        fn->setAttr("chipyard.dpi_export", fields.getDictionary(&getContext()));
      }
      // Body text includes parameter-specialized constants and SV ABI metadata.
      std::string body;
      llvm::raw_string_ostream out(body);
      fn.print(out);
      out.flush();
      llvm::MD5 hash;
      hash.update(body);
      llvm::MD5::MD5Result digest;
      hash.final(digest);
      SmallString<32> key;
      llvm::MD5::stringifyResult(digest, key);
      auto id = key.str().str();
      auto loc = fn.getLoc();
      auto *ctx = &getContext();
      auto oldName = fn.getSymName().str();
      auto name = "__chipyard_export_body_" + id;
      if (failed(SymbolTable::replaceAllSymbolUses(
              StringAttr::get(ctx, oldName), StringAttr::get(ctx, name),
              getOperation())))
        return fn.emitError("cannot rename export references");
      fn.setSymName(name);
      fn->setAttr("chipyard.export_id", StringAttr::get(ctx, id));
      auto &region = fn.getBody();
      auto &oldEntry = region.front();
      SmallVector<Type> inputs;
      for (auto type : original.getInputs()) {
        if (isa<llhd::RefType>(type))
          inputs.append(4, IntegerType::get(ctx, 64));
        else
          inputs.push_back(exportInputType(type));
      }
      auto *entry = new Block();
      region.getBlocks().push_front(entry);
      for (auto type : inputs)
        entry->addArgument(type, loc);
      OpBuilder b(entry, entry->begin());
      unsigned next = 0;
      SmallVector<Attribute> kinds;
      for (auto arg : oldEntry.getArguments()) {
        auto ref = dyn_cast<llhd::RefType>(arg.getType());
        kinds.push_back(b.getBoolAttr(bool(ref)));
        if (!ref) {
          arg.replaceAllUsesWith(
              bitcast(b, loc, arg.getType(), entry->getArgument(next++)));
          continue;
        }
        auto width = storageWidth(ref.getNestedType());
        Value zero = hw::ConstantOp::create(b, loc, APInt(width, 0));
        if (zero.getType() != ref.getNestedType())
          zero = bitcast(b, loc, ref.getNestedType(), zero);
        auto signal = llhd::SignalOp::create(b, loc, ref, StringAttr{}, zero);
        argumentReferences[signal.getOperation()] = {
            entry->getArgument(next),
            entry->getArgument(next + 1),
            {entry->getArgument(next + 2), entry->getArgument(next + 3)}};
        next += 4;
        if (arg.getArgNumber() < formalCount.getInt())
          formalRoots.insert(signal.getOperation());
        argumentRoots.push_back(signal);
        arg.replaceAllUsesWith(signal);
      }
      oldEntry.eraseArguments([](BlockArgument) { return true; });
      cf::BranchOp::create(b, loc, &oldEntry);
      fn.setType(b.getFunctionType(inputs, original.getResults()));
      fn->removeAttr("arg_attrs");
      fn->removeAttr("res_attrs");
      OpBuilder outer(fn);
      auto ptr = LLVM::LLVMPointerType::get(ctx);
      auto wrapper =
          func::FuncOp::create(outer, loc, "__chipyard_export_packet_" + id,
                               b.getFunctionType({ptr, ptr}, {}));
      wrapper->setAttr(
          "chipyard.export_kernel",
          b.getDictionaryAttr(
              {b.getNamedAttr("id", b.getStringAttr(id)),
               b.getNamedAttr("arguments", b.getArrayAttr(kinds)),
               b.getNamedAttr("c_name", fn->getAttr("circt.dpi.export"))}));
      if (auto abi = fn->getAttr("chipyard.dpi_export"))
        wrapper->setAttr("chipyard.dpi_export", abi);
      auto *block = wrapper.addEntryBlock();
      b.setInsertionPointToStart(block);
      auto pointerAt = [&](Value base, Type element, unsigned index) -> Value {
        return LLVM::GEPOp::create(b, loc, ptr, element, base,
                                   ArrayRef<LLVM::GEPArg>{int32_t(index)});
      };
      auto valuePointer = [&](unsigned index) -> Value {
        return LLVM::LoadOp::create(
            b, loc, ptr, pointerAt(block->getArgument(0), ptr, index));
      };
      SmallVector<Value> values;
      unsigned refIndex = 0;
      for (auto [i, type] : llvm::enumerate(original.getInputs())) {
        if (isa<llhd::RefType>(type)) {
          for (unsigned n = 0; n < 4; ++n)
            values.push_back(LLVM::LoadOp::create(
                b, loc, b.getI64Type(),
                pointerAt(block->getArgument(1), b.getI64Type(), refIndex++)));
        } else {
          auto load = LLVM::LoadOp::create(b, loc, exportInputType(type),
                                           valuePointer(i));
          load.setAlignment(1);
          values.push_back(load);
        }
      }
      auto call = func::CallOp::create(b, loc, fn, values);
      if (original.getNumResults()) {
        auto store = LLVM::StoreOp::create(
            b, loc, call.getResult(0), valuePointer(original.getNumInputs()));
        store.setAlignment(1);
      }
      func::ReturnOp::create(b, loc);
      fn->setAttr("chipyard.lowered_dpi_export",
                  fn->getAttr("circt.dpi.export"));
      fn->removeAttr("circt.dpi.export");
    }
    return success();
  }

  LogicalResult removeExportAnchors() {
    SmallVector<func::FuncOp> anchors;
    getOperation().walk([&](func::FuncOp fn) {
      if (fn->hasAttr("chipyard.dpi_capture_anchor"))
        anchors.push_back(fn);
    });
    for (auto fn : anchors) {
      auto uses = SymbolTable::getSymbolUses(fn, getOperation());
      if (!uses)
        return fn.emitError("unknown export capture anchor use");
      SmallVector<func::CallOp> calls;
      for (auto use : *uses) {
        auto call = dyn_cast<func::CallOp>(use.getUser());
        if (!call || call.getNumOperands() != 1 || call.getNumResults() != 0)
          return fn.emitError("invalid export capture anchor");
        auto signal = call.getOperand(0).getDefiningOp<llhd::SignalOp>();
        if (!signal || !signal->hasAttr("chipyard.dpi_captures"))
          return call.emitError("export capture is not module-owned storage");
        calls.push_back(call);
      }
      for (auto call : calls)
        call.erase();
      fn.erase();
    }
    return success();
  }

  void runOnOperation() override {
    if (exports && failed(removeExportAnchors())) {
      signalPassFailure();
      return;
    }
    if (exports && failed(prepareExports())) {
      signalPassFailure();
      return;
    }
    if (!capturesOnly)
      materializeGlobals();
    getOperation().walk([&](sim::PrintFormattedProcOp print) {
      if (print.getStream() &&
          print.getStream().getDefiningOp<sim::SVChannelToOutputStreamOp>())
        print->setAttr("chipyard.file_output", UnitAttr::get(&getContext()));
    });
    SmallVector<llhd::SignalOp> signals;
    getOperation().walk([&](llhd::SignalOp op) {
      if ((isa<hw::HWModuleOp>(op->getParentOp()) &&
           (!capturesOnly || op->hasAttr("chipyard.dpi_captures"))) ||
          argumentReferences.contains(op.getOperation())) {
        signals.push_back(op);
        if (op->hasAttr("chipyard.dpi_captures"))
          globalRoots.push_back(op);
      }
    });
    for (auto signal : signals)
      if (!consumed.contains(signal.getOperation()) && !lower(signal))
        (void)lowerCFG(signal);
    llvm::append_range(globalRoots, argumentRoots);
    for (auto root : globalRoots)
      if (!consumed.contains(root.getOperation())) {
        // lower() erases successful roots too; consumed tracks both paths.
        root.emitError("cannot bind native global reference component or "
                       "exported reference; "
                       "temporary DPI formals require blocking assignments");
        signalPassFailure();
        return;
      }
    // SV array reads outside their declared bounds produce X, represented as
    // zero by this native two-state backend. Guard the index as well as the
    // result so a later load never receives an invalid pointer, even if the
    // array read survives instead of becoming a mux tree.
    SmallVector<hw::ArrayGetOp> gets;
    getOperation().walk([&](hw::ArrayGetOp get) { gets.push_back(get); });
    for (auto get : gets) {
      if (get->hasAttr("chipyard.array_bounds_checked"))
        continue;
      auto array = cast<hw::ArrayType>(get.getInput().getType());
      auto size = array.getNumElements();
      unsigned indexWidth = storageWidth(get.getIndex().getType());
      if (indexWidth < 64 && (size >> indexWidth))
        continue;
      auto width = storageWidth(get.getType());
      if (width <= 0)
        continue;
      OpBuilder b(get);
      auto loc = get.getLoc();
      Value zero = hw::ConstantOp::create(b, loc, APInt(width, 0));
      if (zero.getType() != get.getType())
        zero = bitcast(b, loc, get.getType(), zero);
      if (!size) {
        get.replaceAllUsesWith(zero);
        get.erase();
        continue;
      }
      auto valid = indexValid(b, loc, get.getIndex(), size);
      auto first = hw::ConstantOp::create(b, loc, APInt(indexWidth, 0));
      auto safe = comb::MuxOp::create(b, loc, valid, get.getIndex(), first);
      get.getIndexMutable().assign(safe);
      get->setAttr("chipyard.array_bounds_checked", b.getUnitAttr());
      b.setInsertionPointAfter(get);
      auto checked = comb::MuxOp::create(b, loc, valid, get.getResult(), zero);
      get.getResult().replaceAllUsesExcept(checked.getResult(), checked);
    }
    if (count)
      llvm::errs() << "Bound " << count
                   << " native procedural signal memories\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardSignalMemory", "1",
          [] { PassRegistration<SignalMemory>(); }};
}
