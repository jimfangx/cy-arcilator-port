//===- LowerArrays.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Based on CIRCT firtool-1.162.0 (fa4f662cbe710906347fac795c581a06c6b99757).
// Preserve array values across function CFG edges and derive return buffers
// from function result types, including functions with multiple return blocks.

#include <cassert>
#include <utility>

#include "circt/Dialect/Arc/ArcOps.h"
#include "circt/Dialect/Arc/ArcPasses.h"
#include "circt/Dialect/Arc/ArcTypes.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/HWTypes.h"
#include "circt/Support/Namespace.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/FuncConversions.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define DEBUG_TYPE "arc-lower-arrays"

namespace circt {
namespace arc {
#define GEN_PASS_DEF_LOWERARRAYS
#include "circt/Dialect/Arc/ArcPasses.h.inc"
} // namespace arc
} // namespace circt

using namespace mlir;
using namespace circt;
using namespace arc;
using namespace hw;
using ::llvm::enumerate;

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {
using ReturnBuffers = DenseMap<Operation *, SmallVector<unsigned>>;
ArrayRef<unsigned> bufferResults(const ReturnBuffers &buffers,
                                 Operation *function) {
  auto found = buffers.find(function);
  return found == buffers.end() ? ArrayRef<unsigned>{} : found->second;
}

struct LowerArraysPass : public arc::impl::LowerArraysBase<LowerArraysPass> {
  LowerArraysPass() = default;
  LowerArraysPass(const LowerArraysPass &pass) : LowerArraysPass() {}

  StringRef getArgument() const override { return "chipyard-lower-arrays"; }
  void runOnOperation() override;
};

Value asIndex(Value value, OpBuilder &builder) {
  Location loc = builder.getUnknownLoc();
  if (Operation *parent = value.getDefiningOp()) {
    loc = parent->getLoc();
  }
  return arith::IndexCastUIOp::create(builder, loc, builder.getIndexType(),
                                      value);
}

Value cloneArrayRef(Value value, OpBuilder &builder, Location loc) {
  Value newAlloc = ArrayRefAllocOp::create(builder, loc, value.getType(), {});
  return ArrayRefCopyOp::create(builder, loc, newAlloc, value);
}

struct ConvertFunc : public OpConversionPattern<func::FuncOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(func::FuncOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    const TypeConverter &converter = *getTypeConverter();
    TypeConverter::SignatureConversion conversion(op.getNumArguments());

    SmallVector<Type> newArgTypes;
    SmallVector<Type> newResultTypes;
    auto originalAttrs = op.getArgAttrsAttr();
    SmallVector<Attribute> argumentAttrs;
    // Result buffers follow the function signature, independent of its CFG.
    for (Type result : op.getResultTypes()) {
      if (isa<ArrayType>(result)) {
        // Nested arrays can remain value types. Only actual buffer results
        // introduce an sret argument; calls must use this same classification.
        if (auto buffer =
                dyn_cast<ArrayRefType>(converter.convertType(result))) {
          conversion.addInputs(buffer);
          newArgTypes.push_back(buffer);
          if (originalAttrs)
            argumentAttrs.push_back(DictionaryAttr::get(getContext()));
        }
      }
    }

    if (failed(converter.convertTypes(op.getArgumentTypes(), newArgTypes)) ||
        failed(converter.convertTypes(op.getResultTypes(), newResultTypes))) {
      return failure();
    }

    if (failed(converter.convertSignatureArgs(op.getArgumentTypes(),
                                              conversion)) ||
        (!op.isExternal() && failed(rewriter.convertRegionTypes(
                                 &op.getBody(), converter, &conversion)))) {
      return failure();
    }

    rewriter.modifyOpInPlace(op, [&] {
      op.setType(FunctionType::get(getContext(), newArgTypes, newResultTypes));
      if (originalAttrs) {
        llvm::append_range(argumentAttrs, originalAttrs);
        op.setArgAttrsAttr(ArrayAttr::get(getContext(), argumentAttrs));
      }
    });

    return success();
  }
};

struct ConvertReturn : public OpConversionPattern<func::ReturnOp> {
  const ReturnBuffers &buffers;
  ConvertReturn(TypeConverter &converter, MLIRContext *context,
                const ReturnBuffers &buffers)
      : OpConversionPattern(converter, context), buffers(buffers) {}

  LogicalResult
  matchAndRewrite(func::ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    func::FuncOp func = op->getParentOfType<func::FuncOp>();
    auto results = bufferResults(buffers, func);
    unsigned sretIndex = 0;
    SmallVector<Value> newOperands;
    for (auto [index, operand] : llvm::enumerate(adaptor.getOperands())) {
      if (sretIndex < results.size() && results[sretIndex] == index) {
        Value arg = func.getArgument(sretIndex++);
        Value copy =
            ArrayRefCopyOp::create(rewriter, op.getLoc(), arg, operand);
        newOperands.push_back(copy);
      } else {
        newOperands.push_back(operand);
      }
    }
    rewriter.modifyOpInPlace(op, [&] { op->setOperands(newOperands); });
    return success();
  }
};

struct ConvertCall : public OpConversionPattern<func::CallOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(func::CallOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    SmallVector<Value> newOperands;
    for (Type resultType : op.getResultTypes()) {
      auto arrayType = dyn_cast<ArrayType>(resultType);
      if (!arrayType)
        continue;
      auto arrayRefType =
          dyn_cast<ArrayRefType>(getTypeConverter()->convertType(resultType));
      if (!arrayRefType)
        continue;
      Value alloc =
          ArrayRefAllocOp::create(rewriter, op.getLoc(), arrayRefType, {});
      newOperands.push_back(alloc);
    }
    for (Value operand : adaptor.getOperands()) {
      newOperands.push_back(operand);
    }

    SmallVector<Type> resultTypes;
    if (failed(getTypeConverter()->convertTypes(op.getResultTypes(),
                                                resultTypes))) {
      return failure();
    }

    auto newCall = func::CallOp::create(rewriter, op.getLoc(), op.getCallee(),
                                        resultTypes, newOperands);
    newCall->setDiscardableAttrs(op->getDiscardableAttrDictionary());

    rewriter.replaceOp(op, newCall);
    return success();
  }
};

struct ConvertAggregateConstant
    : public OpConversionPattern<AggregateConstantOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(AggregateConstantOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto newType =
        cast<ArrayRefType>(getTypeConverter()->convertType(op.getType()));
    // The SDK's allocation initializer emits LLVM integer constants directly.
    // Aggregate elements must instead pass through HW-to-LLVM conversion so
    // their field types, nested arrays and target layout are respected.
    if (!isa<IntegerType>(newType.getElementType())) {
      SmallVector<Value> elements;
      for (Attribute field : op.getFieldsAttr())
        elements.push_back(AggregateConstantOp::create(rewriter, op.getLoc(),
                                                       newType.getElementType(),
                                                       cast<ArrayAttr>(field)));
      Value alloc = ArrayRefAllocOp::create(rewriter, op.getLoc(), newType, {});
      Value initialized = ArrayRefCreateOp::create(rewriter, op.getLoc(),
                                                   newType, alloc, elements);
      rewriter.replaceOp(op, initialized);
      return success();
    }
    Value newOp = ArrayRefAllocOp::create(rewriter, op.getLoc(), newType,
                                          op.getFieldsAttr());
    rewriter.replaceOp(op, newOp);
    return success();
  }
};

struct ConvertArrayGet : public OpConversionPattern<ArrayGetOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(hw::ArrayGetOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type resultType = getTypeConverter()->convertType(op.getType());
    Value index = asIndex(op.getIndex(), rewriter);
    Value newOp = ArrayRefGetOp::create(rewriter, op.getLoc(), resultType,
                                        adaptor.getInput(), index);
    rewriter.replaceOp(op, newOp);
    return success();
  }
};

struct ConvertArrayInject : public OpConversionPattern<ArrayInjectOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ArrayInjectOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value index = asIndex(op.getIndex(), rewriter);
    Value dest = cloneArrayRef(adaptor.getInput(), rewriter, op.getLoc());
    Value newOp =
        ArrayRefInjectOp::create(rewriter, op.getLoc(), dest.getType(), dest,
                                 index, adaptor.getElement());
    rewriter.replaceOp(op, newOp);
    return success();
  }
};

struct ConvertArraySlice : public OpConversionPattern<ArraySliceOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ArraySliceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Because we're converting from value semantics we can assume that the
    // input buffer is immutable, so we don't need to copy it.
    Value index = asIndex(op.getLowIndex(), rewriter);
    Type destType = getTypeConverter()->convertType(op.getType());
    Value newOp = ArrayRefSliceOp::create(rewriter, op.getLoc(), destType,
                                          adaptor.getInput(), index);
    rewriter.replaceOp(op, newOp);
    return success();
  }
};

struct ConvertArrayConcat : public OpConversionPattern<ArrayConcatOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ArrayConcatOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type destType = getTypeConverter()->convertType(op.getType());
    Value dest = ArrayRefAllocOp::create(rewriter, op.getLoc(), destType, {});

    // ArrayConcatOp's operands are ordered from most significant to least
    // significant.
    int offset = cast<ArrayRefType>(destType).getNumElements();
    for (Value operand : adaptor.getOperands()) {
      offset -= cast<ArrayRefType>(operand.getType()).getNumElements();
      Value index =
          arith::ConstantIndexOp::create(rewriter, op.getLoc(), offset);
      Value destSlice = ArrayRefSliceOp::create(rewriter, op.getLoc(),
                                                operand.getType(), dest, index);
      ArrayRefCopyOp::create(rewriter, op.getLoc(), destSlice, operand);
    }
    assert(offset == 0);
    rewriter.replaceOp(op, dest);
    return success();
  }
};

struct ConvertArrayCreate : public OpConversionPattern<ArrayCreateOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(ArrayCreateOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type newType = getTypeConverter()->convertType(op.getType());
    Value alloc = ArrayRefAllocOp::create(rewriter, op.getLoc(), newType, {});
    Value create = ArrayRefCreateOp::create(rewriter, op.getLoc(), newType,
                                            alloc, adaptor.getInputs());
    rewriter.replaceOp(op, create);
    return success();
  }
};

struct ConvertStorageGet : public OpConversionPattern<StorageGetOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(StorageGetOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto result = convertOpResultTypes(op, adaptor.getOperands(),
                                       *getTypeConverter(), rewriter);
    if (failed(result))
      return failure();

    rewriter.replaceOp(op, *result);
    return success();
  }
};

struct ConvertMux : public OpConversionPattern<comb::MuxOp> {
  using OpConversionPattern::OpConversionPattern;

  LogicalResult
  matchAndRewrite(comb::MuxOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Type newType = getTypeConverter()->convertType(op.getType());
    Value newOp = arith::SelectOp::create(
        rewriter, op.getLoc(), newType, adaptor.getCond(),
        adaptor.getTrueValue(), adaptor.getFalseValue());
    rewriter.replaceOp(op, newOp);
    return success();
  }
};

// Identifies a return of an ArrayRef that is defined by an ArrayRefAllocOp,
// and replaces the alloc with the sret buffer. Also removes the copy.
struct OptimizeReturnOfAlloc : public OpRewritePattern<func::ReturnOp> {
  const ReturnBuffers &buffers;
  OptimizeReturnOfAlloc(MLIRContext *context, const ReturnBuffers &buffers)
      : OpRewritePattern(context), buffers(buffers) {}

  LogicalResult matchAndRewrite(func::ReturnOp op,
                                PatternRewriter &rewriter) const override {
    auto funcOp = op->getParentOfType<func::FuncOp>();
    bool changed = false;

    // Buffer positions come from the original signature. Scalar/nested-value
    // results and preexisting borrowed references do not consume sret slots.
    for (auto [bufferIndex, resultIndex] :
         llvm::enumerate(bufferResults(buffers, funcOp))) {
      auto arg = funcOp.getArgument(bufferIndex);
      auto &result = op->getOpOperand(resultIndex);
      Value resultValue = result.get();
      auto copy = resultValue.getDefiningOp<ArrayRefCopyOp>();
      if (!copy || copy.getInput() != arg)
        continue;
      ArrayRefAllocOp alloc = getUltimatelyDefiningAlloc(copy.getSource());
      if (!alloc || alloc.getInit())
        continue;
      // Note that `result` is a structured binding, which cannot be implicitly
      // captured until C++20.
      rewriter.modifyOpInPlace(
          op, [&, &result = result] { result.set(copy.getSource()); });
      rewriter.replaceAllUsesWith(alloc, arg);
      rewriter.eraseOp(alloc);
      rewriter.eraseOp(copy);
      changed = true;
    }
    return success(changed);
  }

  ArrayRefAllocOp getUltimatelyDefiningAlloc(Value value) const {
    if (!isa<ArrayRefType>(value.getType()))
      return nullptr;
    while (value) {
      Operation *op = value.getDefiningOp();
      if (!op)
        return nullptr;

      if (auto alloc = dyn_cast<ArrayRefAllocOp>(op))
        return alloc;

      value =
          TypeSwitch<Operation *, Value>(op)
              .Case<ArrayRefCopyOp>([&](auto copy) { return copy.getInput(); })
              .Case<ArrayRefInjectOp>(
                  [&](auto inject) { return inject.getInput(); })
              .Case<ArrayRefFromArrayOp>(
                  [&](auto fromArray) { return fromArray.getInput(); })
              .Case<func::CallOp>([&](auto call) {
                OpResult result = cast<OpResult>(value);
                auto callee =
                    SymbolTable::lookupNearestSymbolFrom<func::FuncOp>(
                        call, call.getCalleeAttr());
                auto results = bufferResults(buffers, callee);
                auto found = llvm::find(results, result.getResultNumber());
                if (found == results.end())
                  return Value{};
                return call.getOperand(std::distance(results.begin(), found));
              })
              .Default([&](Operation *) { return nullptr; });
    }
    return nullptr;
  }
};

struct ConvertStateRead : public OpConversionPattern<StateReadOp> {
  using OpConversionPattern<StateReadOp>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(StateReadOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto result = convertOpResultTypes(op, adaptor.getOperands(),
                                       *this->getTypeConverter(), rewriter);
    if (failed(result))
      return failure();

    // For correctness we need to copy the read state into a local array.
    Value resultValue = result.value()->getResult(0);
    rewriter.replaceOp(op, cloneArrayRef(resultValue, rewriter, op.getLoc()));
    return success();
  }
};

template <typename Op>
struct ConvertTrivially : public OpConversionPattern<Op> {
  using OpConversionPattern<Op>::OpConversionPattern;

  LogicalResult
  matchAndRewrite(Op op, typename Op::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto result = convertOpResultTypes(op, adaptor.getOperands(),
                                       *this->getTypeConverter(), rewriter);
    if (failed(result))
      return failure();
    rewriter.replaceOp(op, *result);
    return success();
  }
};

using ConvertAllocState = ConvertTrivially<arc::AllocStateOp>;
using ConvertStateWrite = ConvertTrivially<arc::StateWriteOp>;
using ConvertRootInput = ConvertTrivially<arc::RootInputOp>;
using ConvertRootOutput = ConvertTrivially<arc::RootOutputOp>;
using ConvertUnrealizedConversionCast =
    ConvertTrivially<UnrealizedConversionCastOp>;

} // namespace

void LowerArraysPass::runOnOperation() {
  // Execute regions can contain arbitrary CFGs and array block arguments.
  // Give each isolated region a function signature before converting arrays,
  // so its entry arguments, branch edges and multiple exits use the same
  // buffer convention as ordinary functions. Process nested regions first.
  SmallVector<arc::ExecuteOp> executes;
  getOperation().walk<WalkOrder::PostOrder>(
      [&](arc::ExecuteOp op) { executes.push_back(op); });
  circt::Namespace names;
  names.add(getOperation());
  for (auto execute : executes) {
    getContext().getOrLoadDialect<LLVM::LLVMDialect>();
    OpBuilder builder(getOperation().getBodyRegion());
    builder.setInsertionPointToEnd(getOperation().getBody());
    auto helper = func::FuncOp::create(
        builder, execute.getLoc(), names.newName("__arc_execute"),
        builder.getFunctionType(execute.getOperandTypes(),
                                execute.getResultTypes()));
    helper.setPrivate();
    helper->setAttr(
        "llvm.linkage",
        LLVM::LinkageAttr::get(&getContext(), LLVM::Linkage::Internal));
    helper.getBody().takeBody(execute.getBody());
    for (auto &block : helper.getBody()) {
      if (auto output = dyn_cast<arc::OutputOp>(block.getTerminator())) {
        builder.setInsertionPoint(output);
        func::ReturnOp::create(builder, output.getLoc(), output.getOutputs());
        output.erase();
      }
    }
    builder.setInsertionPoint(execute);
    auto call = func::CallOp::create(builder, execute.getLoc(), helper,
                                     execute.getOperands());
    call->setDiscardableAttrs(execute->getDiscardableAttrDictionary());
    execute.replaceAllUsesWith(call.getResults());
    execute.erase();
  }
  TypeConverter converter;
  ConversionTarget target(getContext());
  RewritePatternSet patterns(&getContext());

  converter.addConversion([](Type type) { return type; });
  converter.addConversion([&converter](ArrayType type) -> Type {
    Type newElem = converter.convertType(type.getElementType());
    // Don't convert nested array types.
    if (newElem != type.getElementType())
      return type;
    return ArrayRefType::get(newElem, type.getNumElements());
  });
  converter.addConversion([&converter](StateType type) {
    return StateType::get(converter.convertType(type.getType()));
  });

  ReturnBuffers returnBuffers;
  getOperation().walk([&](func::FuncOp function) {
    auto &results = returnBuffers[function];
    for (auto [index, type] : llvm::enumerate(function.getResultTypes()))
      if (isa<ArrayType>(type) &&
          isa<ArrayRefType>(converter.convertType(type)))
        results.push_back(index);
  });

  target.addLegalOp<ArrayRefFromArrayOp, ArrayRefToArrayOp>();
  // Arrays within structs or unions always use !hw.array.
  target.addLegalOp<StructCreateOp, StructInjectOp, StructExtractOp,
                    StructExplodeOp, UnionCreateOp, UnionExtractOp>();
  target.markUnknownOpDynamicallyLegal(
      [&](Operation *op) { return converter.isLegal(op); });
  target.addDynamicallyLegalOp<func::FuncOp>([&](func::FuncOp func) {
    FunctionType fty = func.getFunctionType();
    if (!converter.isLegal(fty.getInputs()) ||
        !converter.isLegal(fty.getResults()))
      return false;
    return llvm::all_of(func.getBody(), [&](Block &block) {
      return converter.isLegal(block.getArgumentTypes());
    });
  });
  // An ArrayGetOp may legally return an array if the input type was a nested
  // array. Similarly for ArrayCreateOp and ArrayInjectOp.
  target.addDynamicallyLegalOp<ArrayGetOp>([&](ArrayGetOp op) {
    return converter.isLegal(op.getInput().getType());
  });
  target.addDynamicallyLegalOp<ArrayCreateOp, ArrayInjectOp>(
      [&](Operation *op) {
        return converter.isLegal(op->getResult(0).getType());
      });

  // Produces an ArrayRefType from an ArrayType.
  converter.addTargetMaterialization([&](OpBuilder &b, ArrayRefType type,
                                         ValueRange values, Location loc,
                                         Type fromType) -> Value {
    assert(isa<ArrayType>(fromType));
    Value alloc = ArrayRefAllocOp::create(b, loc, type, {});
    return ArrayRefFromArrayOp::create(b, loc, type, alloc, values.front());
  });

  // Produces an ArrayType from an ArrayRefType.
  converter.addSourceMaterialization([&](OpBuilder &b, ArrayType type,
                                         ValueRange values,
                                         Location loc) -> Value {
    assert(isa<ArrayRefType>(values.front().getType()));
    return ArrayRefToArrayOp::create(b, loc, type, values.front());
  });

  patterns
      .add<ConvertFunc, ConvertCall, ConvertAggregateConstant, ConvertArrayGet,
           ConvertArrayInject, ConvertArraySlice, ConvertArrayConcat,
           ConvertArrayCreate, ConvertStorageGet, ConvertMux, ConvertAllocState,
           ConvertStateRead, ConvertStateWrite, ConvertRootInput,
           ConvertRootOutput, ConvertUnrealizedConversionCast>(converter,
                                                               &getContext());
  patterns.add<ConvertReturn>(converter, &getContext(), returnBuffers);

  populateBranchOpInterfaceTypeConversionPattern(patterns, converter);

  ConversionConfig config;
  config.allowPatternRollback = false;
  if (failed(applyPartialConversion(getOperation(), target, std::move(patterns),
                                    config))) {
    return signalPassFailure();
  }

  // Apply some cleanup patterns to optimize away ArrayRefAllocOps.
  RewritePatternSet cleanupPatterns(&getContext());
  cleanupPatterns.add<OptimizeReturnOfAlloc>(&getContext(), returnBuffers);
  if (failed(
          applyPatternsGreedily(getOperation(), std::move(cleanupPatterns)))) {
    return signalPassFailure();
  }
}

extern "C" LLVM_ATTRIBUTE_WEAK ::mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardCfgArrays", "1.0", []() {
            mlir::registerPass([]() -> std::unique_ptr<mlir::Pass> {
              return std::make_unique<LowerArraysPass>();
            });
          }};
}
