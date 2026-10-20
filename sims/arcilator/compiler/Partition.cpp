// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Bound CIRCT's per-module analyses without changing the native boundary.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/PortImplementation.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SMTAPI.h"
#include <functional>
#include <map>
#include <numeric>
#include <optional>

using namespace mlir;
using namespace circt;

namespace {
struct Group {
  SmallVector<Operation *> operations;
  llvm::SetVector<Value> inputs, outputs;
  llvm::SetVector<Operation *> constants;
  hw::HWModuleOp module;
  hw::InstanceOp instance;
};

bool isHardwareValue(Type type) {
  return hw::isHWValueType(type) || isa<seq::ClockType>(type);
}

// Immutable literals can be copied into each partition. Hardware constants
// must remain visible to local canonicalization too: treating Boolean true or
// a decoder mask as a dynamic module input can hide complemented predicates
// and preserve a false combinational dependency cycle. Parameter references
// retain their original scope and are not rematerialized as HW literals.
bool canRematerialize(Operation *op) {
  return op &&
         (op->hasTrait<OpTrait::ConstantLike>() ||
          op->getName().getStringRef() == "sim.stdout_stream" ||
          op->getName().getStringRef() == "sim.stderr_stream") &&
         op->getNumOperands() == 0 && op->getNumRegions() == 0 &&
         (isa<hw::ConstantOp, hw::AggregateConstantOp>(op) ||
          op->getName().getStringRef() == "arith.constant" ||
          llvm::any_of(op->getResultTypes(),
                       [](Type type) { return !isHardwareValue(type); }));
}

// An array local may retain unwritten lanes through an aggregate feedback
// value even though every observed lane is assigned independently. Resolve
// constant element reads through only those cyclic mux/injection cones. Never
// replace unresolved elements with a default or expand dynamic/OOB accesses.
struct ArrayFeedback {
  Block *block;
  DenseMap<Value, bool> cyclic;
  using Key = std::pair<Value, uint64_t>;
  DenseMap<Key, Value> elements;
  OpBuilder builder;
  ArrayFeedback(Block *block)
      : block(block), builder(block->getParentOp()->getContext()) {}
  std::optional<uint64_t> constantIndex(Value value, uint64_t size) {
    auto constant = value.getDefiningOp<hw::ConstantOp>();
    if (!constant || constant.getValue().getActiveBits() > 64)
      return std::nullopt;
    auto index = constant.getValue().getZExtValue();
    return index < size ? std::optional<uint64_t>(index) : std::nullopt;
  }
  SmallVector<Value, 2> inputs(Value value) {
    auto *op = value.getDefiningOp();
    if (!op || op->getBlock() != block)
      return {};
    if (auto mux = dyn_cast<comb::MuxOp>(op))
      return {mux.getTrueValue(), mux.getFalseValue()};
    if (auto inject = dyn_cast<hw::ArrayInjectOp>(op)) {
      auto type = hw::type_cast<hw::ArrayType>(value.getType());
      if (constantIndex(inject.getIndex(), type.getNumElements()))
        return {inject.getInput()};
    }
    return {};
  }
  bool hasCycle(Value root) {
    if (auto found = cyclic.find(root); found != cyclic.end())
      return found->second;
    DenseMap<Value, unsigned> status;
    SmallVector<std::pair<Value, bool>> work{{root, false}};
    while (!work.empty()) {
      auto [value, expanded] = work.pop_back_val();
      if (expanded) {
        status[value] = 2;
        continue;
      }
      if (status.lookup(value) == 1) {
        cyclic[root] = true;
        return true;
      }
      if (status.lookup(value) == 2)
        continue;
      status[value] = 1;
      work.emplace_back(value, true);
      for (auto input : inputs(value))
        work.emplace_back(input, false);
    }
    for (auto [value, state] : status)
      cyclic[value] = false;
    return false;
  }
  Value resolve(Value root, Value indexValue, uint64_t index) {
    Key rootKey{root, index};
    SmallVector<std::pair<Key, bool>> work{{rootKey, false}};
    while (!work.empty()) {
      auto [key, expanded] = work.pop_back_val();
      auto [array, element] = key;
      auto *op = array.getDefiningOp();
      if (!expanded) {
        if (elements.count(key))
          continue;
        // A null pending entry stops recursion through an unwritten lane.
        elements[key] = {};
        auto parents = inputs(array);
        if (!parents.empty()) {
          if (auto inject = dyn_cast<hw::ArrayInjectOp>(op)) {
            auto type = hw::type_cast<hw::ArrayType>(array.getType());
            if (*constantIndex(inject.getIndex(), type.getNumElements()) ==
                element) {
              elements[key] = inject.getElement();
              continue;
            }
          }
          work.emplace_back(key, true);
          for (auto parent : parents)
            work.push_back({{parent, element}, false});
        } else if (auto create = dyn_cast_or_null<hw::ArrayCreateOp>(op)) {
          elements[key] =
              create.getInputs()[create.getInputs().size() - 1 - element];
        } else {
          // An ordinary array input or state is a leaf. Its read is pure and
          // has the same constant index and type as the original read.
          auto indexType = hw::type_cast<IntegerType>(indexValue.getType());
          auto constant = hw::ConstantOp::create(
              builder, root.getLoc(), APInt(indexType.getWidth(), element));
          elements[key] =
              hw::ArrayGetOp::create(builder, root.getLoc(), array, constant);
        }
        continue;
      }
      auto parents = inputs(array);
      auto first = elements.lookup({parents[0], element});
      if (!first)
        continue;
      if (auto mux = dyn_cast<comb::MuxOp>(op)) {
        auto second = elements.lookup({parents[1], element});
        if (second)
          elements[key] =
              comb::MuxOp::create(builder, mux.getLoc(), mux.getCond(), first,
                                  second, mux.getTwoState());
      } else
        elements[key] = first;
    }
    return elements.lookup(rootKey);
  }
  void pruneDeadCones(ArrayRef<Operation *> roots) {
    DenseSet<Operation *> candidates;
    SmallVector<Operation *> work(roots);
    while (!work.empty()) {
      auto *op = work.pop_back_val();
      if (!op || op->getBlock() != block || op->getNumRegions() ||
          !op->getNumResults() || op->hasAttr("inner_sym") ||
          isa<hw::InstanceOp>(op) || !isMemoryEffectFree(op))
        continue;
      auto dialect = op->getName().getDialectNamespace();
      if (dialect != "comb" && dialect != "hw")
        continue;
      if (candidates.insert(op).second)
        for (auto operand : op->getOperands())
          work.push_back(operand.getDefiningOp());
    }
    DenseSet<Operation *> live;
    for (auto *op : candidates)
      if (llvm::any_of(op->getUsers(), [&](Operation *user) {
            return !candidates.count(user);
          }))
        work.push_back(op);
    while (!work.empty()) {
      auto *op = work.pop_back_val();
      if (!live.insert(op).second)
        continue;
      for (auto operand : op->getOperands())
        if (auto *producer = operand.getDefiningOp();
            candidates.count(producer))
          work.push_back(producer);
    }
    SmallVector<Operation *> dead;
    for (auto *op : candidates)
      if (!live.count(op))
        dead.push_back(op);
    // Ordinary local DCE cannot remove a mutually dependent dead SCC. Every
    // removed operation is pure and has no users outside this dead set.
    for (auto *op : dead)
      op->dropAllReferences();
    for (auto *op : dead)
      op->erase();
  }
  unsigned run() {
    SmallVector<Operation *> reads, replaced;
    for (auto &op : *block)
      if (isa<hw::ArrayGetOp, hw::ArraySliceOp>(op))
        reads.push_back(&op);
    unsigned count = 0;
    for (auto *op : reads) {
      auto array = op->getOperand(0), indexValue = op->getOperand(1);
      auto type = hw::type_cast<hw::ArrayType>(array.getType());
      auto index = constantIndex(indexValue, type.getNumElements());
      if (!index || !hasCycle(array))
        continue;
      builder.setInsertionPoint(op);
      Value result;
      if (isa<hw::ArrayGetOp>(op))
        result = resolve(array, indexValue, *index);
      else {
        auto sliceType =
            hw::type_cast<hw::ArrayType>(op->getResult(0).getType());
        auto size = sliceType.getNumElements();
        if (!size || size > type.getNumElements() - *index)
          continue;
        SmallVector<Value> lanes;
        for (uint64_t i = size; i > 0; --i) {
          auto lane = resolve(array, indexValue, *index + i - 1);
          if (!lane)
            break;
          lanes.push_back(lane);
        }
        if (lanes.size() == size) {
          result = hw::ArrayCreateOp::create(builder, op->getLoc(), lanes);
          // Keep alias types outside array_create: the SDK's mux-of-elements
          // canonicalization does not adapt alias result types consistently.
          auto expected = op->getResult(0).getType();
          if (result.getType() != expected)
            result =
                hw::BitcastOp::create(builder, op->getLoc(), expected, result);
        }
      }
      if (result) {
        if (result == op->getResult(0))
          continue;
        op->getResult(0).replaceAllUsesWith(result);
        // Keep the original graph alive until all cached cones are resolved.
        // Remove dead aggregate SCCs before outlining can make their internal
        // edges look like independently observable module outputs.
        replaced.push_back(op);
        ++count;
      }
    }
    pruneDeadCones(replaced);
    return count;
  }
};

// x = shift(x, k) OP a has a unique, finite prefix solution when k > 0 and
// the vacated bits contain OP's identity. Express that solution with a parallel
// scan. Unlike arbitrary combinational feedback, each dependency moves strictly
// toward a seeded boundary. No iteration state or guessed initial value is
// used.
struct PrefixFeedback {
  Block *block;
  struct Shift {
    uint64_t amount;
    bool left;
  };
  std::optional<Shift> matchShift(Value value, Value root, unsigned width,
                                  bool identity) {
    auto *op = value.getDefiningOp();
    if (!op || op->getBlock() != block)
      return std::nullopt;
    if (!identity && isa<comb::ShlOp, comb::ShrUOp>(op) &&
        op->getOperand(0) == root) {
      if (auto amount = op->getOperand(1).getDefiningOp<hw::ConstantOp>()) {
        auto distance = amount.getValue().getLimitedValue(width);
        if (distance)
          return Shift{distance, isa<comb::ShlOp>(op)};
      }
    }
    auto concat = dyn_cast<comb::ConcatOp>(op);
    if (!concat || concat.getInputs().size() != 2)
      return std::nullopt;
    for (bool left : {true, false}) {
      auto kept =
          concat.getInputs()[left ? 0 : 1].getDefiningOp<comb::ExtractOp>();
      auto padding =
          concat.getInputs()[left ? 1 : 0].getDefiningOp<hw::ConstantOp>();
      if (!kept || !padding || kept.getInput() != root ||
          (identity ? !padding.getValue().isAllOnes()
                    : !padding.getValue().isZero()))
        continue;
      auto distance = padding.getValue().getBitWidth();
      auto keptType = hw::type_dyn_cast<IntegerType>(kept.getType());
      if (distance && distance <= width && keptType &&
          keptType.getWidth() == width - distance &&
          kept.getLowBit() == (left ? 0 : distance))
        return Shift{distance, left};
    }
    return std::nullopt;
  }
  bool independent(Value value, Value root) {
    DenseSet<Value> visited;
    SmallVector<Value> work{value};
    while (!work.empty()) {
      auto current = work.pop_back_val();
      if (current == root)
        return false;
      if (!visited.insert(current).second)
        continue;
      auto *op = current.getDefiningOp();
      if (!op || isa<seq::FirRegOp, seq::CompRegOp>(op))
        continue;
      if (op->getBlock() != block || op->getNumRegions())
        return false;
      for (auto input : op->getOperands())
        work.push_back(input);
    }
    return true;
  }
  unsigned run() {
    SmallVector<Operation *> candidates, replaced;
    for (auto &op : *block)
      if (isa<comb::AndOp, comb::OrOp, comb::XorOp>(op))
        candidates.push_back(&op);
    for (auto *op : candidates) {
      auto root = op->getResult(0);
      auto type = hw::type_dyn_cast<IntegerType>(root.getType());
      if (!type || !type.getWidth() || op->hasAttr("inner_sym") ||
          op->getNumOperands() < 2)
        continue;
      bool identity = isa<comb::AndOp>(op);
      for (auto [index, input] : llvm::enumerate(op->getOperands())) {
        auto shift = matchShift(input, root, type.getWidth(), identity);
        if (!shift)
          continue;
        SmallVector<Value> bases;
        bool complete = true;
        for (auto [otherIndex, other] : llvm::enumerate(op->getOperands()))
          if (otherIndex != index) {
            if (!independent(other, root)) {
              complete = false;
              break;
            }
            bases.push_back(other);
          }
        if (!complete)
          continue;
        OpBuilder builder(op);
        auto combine = [&](ValueRange values) -> Value {
          if (values.size() == 1)
            return values.front();
          if (auto bitwise = dyn_cast<comb::AndOp>(op))
            return comb::AndOp::create(builder, op->getLoc(), values,
                                       bitwise.getTwoState());
          if (auto bitwise = dyn_cast<comb::OrOp>(op))
            return comb::OrOp::create(builder, op->getLoc(), values,
                                      bitwise.getTwoState());
          return comb::XorOp::create(builder, op->getLoc(), values,
                                     cast<comb::XorOp>(op).getTwoState());
        };
        Value result = combine(bases);
        bool created = bases.size() > 1;
        for (uint64_t distance = shift->amount; distance < type.getWidth();
             distance *= 2) {
          auto fill =
              hw::ConstantOp::create(builder, op->getLoc(),
                                     identity ? APInt::getAllOnes(distance)
                                              : APInt::getZero(distance));
          auto kept = comb::ExtractOp::create(builder, op->getLoc(), result,
                                              shift->left ? 0 : distance,
                                              type.getWidth() - distance);
          Value next =
              shift->left
                  ? comb::ConcatOp::create(builder, op->getLoc(), kept, fill)
                  : comb::ConcatOp::create(builder, op->getLoc(), fill, kept);
          if (next.getType() != root.getType())
            next = hw::BitcastOp::create(builder, op->getLoc(), root.getType(),
                                         next);
          result = combine(ValueRange{result, next});
          created = true;
        }
        if (created)
          result.getDefiningOp()->setDiscardableAttrs(
              op->getDiscardableAttrDictionary());
        root.replaceAllUsesWith(result);
        replaced.push_back(op);
        break;
      }
    }
    auto count = replaced.size();
    ArrayFeedback(block).pruneDeadCones(replaced);
    return count;
  }
};

// Resolve pure combinational SCCs only when their cut values forget every
// initial value and reach a fixed point for every external input. Unsupported
// operators, unknown solver results, retained state and oscillations remain
// for the normal Arc diagnostics. No callback or sequential operation is
// cloned.
struct ProvenFeedback {
  using Expr = llvm::SMTExprRef;
  struct Node {
    Operation *op;
    SmallVector<unsigned, 3> inputs;
  };
  Block *block;
  bool verbose = false;
  bool nativeFeedback = false;
  unsigned expressionCount = 0, inputCount = 0;
  bool failed = false;
  llvm::SMTSolverRef solver;
  DenseSet<Operation *> members;
  DenseMap<Value, unsigned> cutIndex;
  DenseMap<Value, Expr> outside, round;
  SmallVector<Value> cuts;

  bool supported(Operation *op) {
    return isa<comb::AndOp, comb::OrOp, comb::XorOp, comb::AddOp, comb::SubOp,
               comb::MulOp, comb::MuxOp, comb::ICmpOp, comb::ConcatOp,
               comb::ExtractOp, comb::ReplicateOp, comb::ShlOp, comb::ShrUOp,
               comb::ShrSOp>(op);
  }
  unsigned width(Value value) {
    auto type = dyn_cast<IntegerType>(value.getType());
    return type && type.getWidth() <= 65536 ? type.getWidth() : 0;
  }
  Expr constant(unsigned bits, uint64_t value) {
    return solver->mkBitvector(llvm::APSInt(APInt(bits, value), true), bits);
  }
  Expr boolean(Expr value) {
    return solver->mkIte(value, constant(1, 1), constant(1, 0));
  }
  Expr encode(Operation *op, ArrayRef<Expr> state, unsigned depth) {
    if (++expressionCount > 200000 || depth > 2048) {
      failed = true;
      return {};
    }
    SmallVector<Expr> args;
    for (auto value : op->getOperands()) {
      auto arg = get(value, state, depth + 1);
      if (!arg)
        return {};
      args.push_back(arg);
    }
    auto bits = width(op->getResult(0));
    if (!bits)
      return {};
    if (isa<comb::AndOp, comb::OrOp, comb::XorOp, comb::AddOp, comb::SubOp,
            comb::MulOp>(op)) {
      auto result = args.front();
      for (auto arg : llvm::drop_begin(args)) {
        if (isa<comb::AndOp>(op))
          result = solver->mkBVAnd(result, arg);
        else if (isa<comb::OrOp>(op))
          result = solver->mkBVOr(result, arg);
        else if (isa<comb::XorOp>(op))
          result = solver->mkBVXor(result, arg);
        else if (isa<comb::AddOp>(op))
          result = solver->mkBVAdd(result, arg);
        else if (isa<comb::SubOp>(op))
          result = solver->mkBVSub(result, arg);
        else
          result = solver->mkBVMul(result, arg);
      }
      return result;
    }
    if (isa<comb::MuxOp>(op))
      return solver->mkIte(
          solver->mkNot(solver->mkEqual(args[0], constant(1, 0))), args[1],
          args[2]);
    if (auto cmp = dyn_cast<comb::ICmpOp>(op)) {
      Expr result;
      switch (cmp.getPredicate()) {
      case comb::ICmpPredicate::eq:
      case comb::ICmpPredicate::ceq:
      case comb::ICmpPredicate::weq:
        result = solver->mkEqual(args[0], args[1]);
        break;
      case comb::ICmpPredicate::ne:
      case comb::ICmpPredicate::cne:
      case comb::ICmpPredicate::wne:
        result = solver->mkNot(solver->mkEqual(args[0], args[1]));
        break;
      case comb::ICmpPredicate::ult:
        result = solver->mkBVUlt(args[0], args[1]);
        break;
      case comb::ICmpPredicate::ule:
        result = solver->mkBVUle(args[0], args[1]);
        break;
      case comb::ICmpPredicate::ugt:
        result = solver->mkBVUgt(args[0], args[1]);
        break;
      case comb::ICmpPredicate::uge:
        result = solver->mkBVUge(args[0], args[1]);
        break;
      case comb::ICmpPredicate::slt:
        result = solver->mkBVSlt(args[0], args[1]);
        break;
      case comb::ICmpPredicate::sle:
        result = solver->mkBVSle(args[0], args[1]);
        break;
      case comb::ICmpPredicate::sgt:
        result = solver->mkBVSgt(args[0], args[1]);
        break;
      case comb::ICmpPredicate::sge:
        result = solver->mkBVSge(args[0], args[1]);
        break;
      }
      return boolean(result);
    }
    if (auto extract = dyn_cast<comb::ExtractOp>(op))
      return solver->mkBVExtract(extract.getLowBit() + bits - 1,
                                 extract.getLowBit(), args[0]);
    if (isa<comb::ConcatOp>(op)) {
      auto result = args.front();
      for (auto arg : llvm::drop_begin(args))
        result = solver->mkBVConcat(result, arg);
      return result;
    }
    if (isa<comb::ReplicateOp>(op)) {
      auto result = args[0];
      unsigned inputBits = width(op->getOperand(0));
      for (unsigned i = inputBits; i < bits; i += inputBits)
        result = solver->mkBVConcat(result, args[0]);
      return result;
    }
    if (isa<comb::ShlOp>(op))
      return solver->mkBVShl(args[0], args[1]);
    if (isa<comb::ShrUOp>(op))
      return solver->mkBVLshr(args[0], args[1]);
    if (isa<comb::ShrSOp>(op))
      return solver->mkBVAshr(args[0], args[1]);
    return {};
  }
  Expr get(Value value, ArrayRef<Expr> state, unsigned depth = 0) {
    if (auto found = cutIndex.find(value); found != cutIndex.end())
      return state[found->second];
    bool local = members.contains(value.getDefiningOp());
    auto &memo = local ? round : outside;
    if (auto found = memo.find(value); found != memo.end())
      return found->second;
    auto bits = width(value);
    if (!bits || depth > 2048) {
      if (verbose)
        llvm::errs() << "Feedback encoding limit: type " << value.getType()
                     << ", depth " << depth << "\n";
      failed = true;
      return {};
    }
    Expr result;
    Attribute attr;
    if (matchPattern(value, m_Constant(&attr)) && isa<IntegerAttr>(attr))
      result = solver->mkBitvector(
          llvm::APSInt(cast<IntegerAttr>(attr).getValue(), true), bits);
    else if (auto *op = value.getDefiningOp();
             op && op->getBlock() == block && supported(op) &&
             (bits <= 256 ||
              isa<comb::ConcatOp, comb::ExtractOp, comb::ReplicateOp>(op)))
      result = encode(op, state, depth + 1);
    else if (!local) {
      auto name = "arc_feedback_input_" + std::to_string(inputCount++);
      result = solver->mkSymbol(name.c_str(), solver->getBitvectorSort(bits));
    }
    if (!result)
      failed = true;
    memo[value] = result;
    return result;
  }
  SmallVector<Expr> evaluate(ArrayRef<Expr> state) {
    round.clear();
    SmallVector<Expr> result;
    for (auto value : cuts) {
      auto expr = encode(value.getDefiningOp(), state, 0);
      if (!expr) {
        failed = true;
        return {};
      }
      result.push_back(expr);
    }
    return result;
  }
  bool equivalent(ArrayRef<Expr> left, ArrayRef<Expr> right) {
    auto difference = solver->mkBoolean(false);
    for (auto [a, b] : llvm::zip(left, right))
      difference =
          solver->mkOr(difference, solver->mkNot(solver->mkEqual(a, b)));
    // Each query is independent. Reset assertions so the solver can perform
    // its full bit-vector preprocessing on the new equivalence problem.
    solver->reset();
    solver->addConstraint(difference);
    auto result = solver->check();
    if (verbose)
      llvm::errs() << "Feedback equivalence query: "
                   << (!result   ? "unknown"
                       : *result ? "sat"
                                 : "unsat")
                   << "\n";
    return result && !*result;
  }
  unsigned prove(ArrayRef<Operation *> component) {
    members.clear();
    members.insert(component.begin(), component.end());
    cuts.clear();
    cutIndex.clear();
    outside.clear();
    round.clear();
    expressionCount = inputCount = 0;
    failed = false;
    if (component.size() > 1024)
      return 0;
    for (auto *op : component)
      if (!supported(op) || !width(op->getResult(0)) ||
          width(op->getResult(0)) > 256 || op->hasAttr("inner_sym")) {
        if (verbose)
          llvm::errs() << "Feedback unsupported operation: " << op->getName()
                       << " " << op->getResult(0).getType() << "\n";
        return 0;
      }
    DenseMap<Operation *, unsigned> status;
    std::function<void(Operation *)> dfs = [&](Operation *op) {
      if (status.lookup(op) == 1) {
        auto value = op->getResult(0);
        if (!cutIndex.contains(value)) {
          cutIndex[value] = cuts.size();
          cuts.push_back(value);
        }
        return;
      }
      if (status.lookup(op) == 2)
        return;
      status[op] = 1;
      for (auto input : op->getOperands())
        if (auto *def = input.getDefiningOp(); members.contains(def))
          dfs(def);
      status[op] = 2;
    };
    for (auto *op : component)
      dfs(op);
    unsigned cutBits = 0;
    for (auto value : cuts)
      cutBits += width(value);
    if (verbose)
      llvm::errs() << "Feedback cut set: " << cuts.size() << " values, "
                   << cutBits << " bits\n";
    if (cuts.empty() || cuts.size() > 16 || cutBits > 256)
      return 0;
    solver = llvm::CreateZ3Solver();
    if (!solver)
      return 0;
    solver->setUnsignedParam("timeout", 5000);
    SmallVector<Expr> unknown, zero;
    for (auto [i, value] : llvm::enumerate(cuts)) {
      auto name = "arc_feedback_seed_" + std::to_string(i);
      unknown.push_back(solver->mkSymbol(
          name.c_str(), solver->getBitvectorSort(width(value))));
      zero.push_back(constant(width(value), 0));
    }
    for (unsigned iteration = 1; iteration <= 16; ++iteration) {
      if (verbose)
        llvm::errs() << "Feedback proof round " << iteration << "\n";
      unknown = evaluate(unknown);
      if (failed)
        return 0;
      zero = evaluate(zero);
      if (failed)
        return 0;
      if (!equivalent(unknown, zero))
        continue;
      auto next = evaluate(zero);
      if (failed || !equivalent(next, zero))
        return 0;
      return iteration;
    }
    return 0;
  }
  void rewrite(ArrayRef<Operation *> component, unsigned iterations) {
    OpBuilder builder(block, block->begin());
    DenseMap<Value, Value> previous;
    for (auto value : cuts)
      previous[value] = hw::ConstantOp::create(builder, value.getLoc(),
                                               APInt(width(value), 0));
    DenseMap<Value, Value> memo;
    std::function<Value(Value)> materialize;
    auto clone = [&](Operation *op) -> Value {
      auto value = op->getResult(0);
      if (auto found = memo.find(value); found != memo.end())
        return found->second;
      IRMapping mapping;
      for (auto input : op->getOperands())
        mapping.map(input, materialize(input));
      auto *copy = builder.clone(*op, mapping);
      copy->removeAttr("sv.namehint");
      auto result = copy->getResult(0);
      memo[value] = result;
      return result;
    };
    materialize = [&](Value value) -> Value {
      if (auto found = previous.find(value); found != previous.end())
        return found->second;
      auto *op = value.getDefiningOp();
      return members.contains(op) ? clone(op) : value;
    };
    for (unsigned i = 0; i < iterations; ++i) {
      memo.clear();
      DenseMap<Value, Value> next;
      for (auto value : cuts)
        next[value] = clone(value.getDefiningOp());
      previous = std::move(next);
    }
    memo.clear();
    SmallVector<Value> replacements;
    for (auto *op : component)
      replacements.push_back(materialize(op->getResult(0)));
    for (auto [op, replacement] : llvm::zip(component, replacements)) {
      if (auto name = op->getAttr("sv.namehint"))
        replacement.getDefiningOp()->setAttr("sv.namehint", name);
      op->getResult(0).replaceAllUsesWith(replacement);
    }
    for (auto *op : component)
      op->erase();
  }
  bool bind(ArrayRef<Operation *> component) {
    // LLHD initializers are preserved by SignalState before signal promotion.
    // Remaining native two-state wire cycles start at zero and retain their
    // previous settled value between evaluations. Expose DFS back edges to the
    // host's combinational iteration without turning them into clocked state.
    if (!nativeFeedback)
      return false;
    for (auto *op : component)
      if (!isa<IntegerType>(op->getResult(0).getType()) ||
          op->getResult(0).getType().getIntOrFloatBitWidth() > 65536)
        return false;
    DenseSet<Operation *> local(component.begin(), component.end());
    DenseMap<Operation *, unsigned> status;
    llvm::SetVector<Value> edges;
    struct Frame {
      Operation *op;
      unsigned operand;
    };
    SmallVector<Frame> stack;
    for (auto *start : component) {
      if (status.lookup(start))
        continue;
      status[start] = 1;
      stack.push_back({start, 0});
      while (!stack.empty()) {
        auto &frame = stack.back();
        if (frame.operand == frame.op->getNumOperands()) {
          status[frame.op] = 2;
          stack.pop_back();
          continue;
        }
        Value value = frame.op->getOperand(frame.operand++);
        auto *def = value.getDefiningOp();
        if (!local.contains(def))
          continue;
        if (status.lookup(def) == 1)
          edges.insert(value);
        else if (!status.lookup(def)) {
          status[def] = 1;
          stack.push_back({def, 0});
        }
      }
    }
    auto root = cast<hw::HWModuleOp>(block->getParentOp());
    auto top = root->getParentOfType<ModuleOp>();
    OpBuilder builder(top.getContext());
    SmallVector<Attribute> wires;
    if (auto existing =
            top->getAttrOfType<ArrayAttr>("chipyard.feedback_wires"))
      llvm::append_range(wires, existing);
    for (auto value : edges) {
      unsigned index = wires.size();
      auto [name, input] =
          root.appendInput("feedback_q_" + Twine(index), value.getType());
      value.replaceAllUsesWith(input);
      root.appendOutput("feedback_next_" + Twine(index), value);
      wires.push_back(builder.getDictionaryAttr(
          {builder.getNamedAttr("model", root.getSymNameAttr()),
           builder.getNamedAttr("input", name),
           builder.getNamedAttr(
               "output", root.getOutputNameAttr(root.getNumOutputPorts() - 1)),
           builder.getNamedAttr(
               "bits", builder.getI64IntegerAttr(
                           cast<IntegerType>(value.getType()).getWidth()))}));
    }
    top->setAttr("chipyard.feedback_wires", builder.getArrayAttr(wires));
    llvm::errs() << "Bound combinational SCC of " << component.size()
                 << " operations through " << edges.size()
                 << " native feedback wires\n";
    return true;
  }
  unsigned run() {
    SmallVector<Node> nodes;
    DenseMap<Operation *, unsigned> ids;
    for (auto &op : block->without_terminator()) {
      auto dialect = op.getName().getDialectNamespace();
      // Declarative register ops may be side-effect-free in MLIR, but their
      // operands describe a next-state function. They are state boundaries,
      // not combinational edges in this graph.
      bool combinational = dialect == "comb" || dialect == "arith" ||
                           (dialect == "hw" && !isa<hw::InstanceOp>(op));
      if (combinational && op.getNumResults() == 1 && !op.getNumRegions() &&
          isPure(&op)) {
        ids[&op] = nodes.size();
        nodes.push_back({&op, {}});
      }
    }
    for (auto &node : nodes)
      for (auto input : node.op->getOperands())
        if (auto found = ids.find(input.getDefiningOp()); found != ids.end())
          node.inputs.push_back(found->second);
    unsigned unseen = ~0U, next = 0;
    SmallVector<unsigned> indices(nodes.size(), unseen), low(nodes.size()),
        stack;
    SmallVector<bool> active(nodes.size(), false);
    struct Frame {
      unsigned node, next;
    };
    SmallVector<Frame> work;
    SmallVector<SmallVector<Operation *>> components;
    auto enter = [&](unsigned node) {
      indices[node] = low[node] = next++;
      active[node] = true;
      stack.push_back(node);
      work.push_back({node, 0});
    };
    for (unsigned root = 0; root < nodes.size(); ++root) {
      if (indices[root] != unseen)
        continue;
      enter(root);
      while (!work.empty()) {
        auto &frame = work.back();
        unsigned node = frame.node;
        if (frame.next < nodes[node].inputs.size()) {
          unsigned input = nodes[node].inputs[frame.next++];
          if (indices[input] == unseen)
            enter(input);
          else if (active[input])
            low[node] = std::min(low[node], indices[input]);
          continue;
        }
        if (indices[node] == low[node]) {
          SmallVector<Operation *> component;
          unsigned item;
          do {
            item = stack.pop_back_val();
            active[item] = false;
            component.push_back(nodes[item].op);
          } while (item != node);
          if (component.size() > 1 ||
              llvm::is_contained(nodes[node].inputs, node))
            components.push_back(std::move(component));
        }
        work.pop_back();
        if (!work.empty())
          low[work.back().node] = std::min(low[work.back().node], low[node]);
      }
    }
    unsigned resolved = 0;
    for (auto &component : components)
      if (auto iterations = prove(component)) {
        rewrite(component, iterations);
        ++resolved;
        llvm::errs() << "Proved combinational SCC of " << component.size()
                     << " operations with " << cuts.size()
                     << " cut values stable after " << iterations
                     << " rounds\n";
      } else if (!bind(component)) {
        llvm::errs() << "Retained unproven combinational SCC of "
                     << component.size() << " operations\n";
      }
    return resolved;
  }
};

struct ResolveFeedback : PassWrapper<ResolveFeedback, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ResolveFeedback)
  ResolveFeedback() = default;
  ResolveFeedback(const ResolveFeedback &other) : PassWrapper(other) {}
  Option<bool> verbose{*this, "verbose", llvm::cl::init(false)};
  StringRef getArgument() const override { return "chipyard-resolve-feedback"; }
  void runOnOperation() override {
    for (auto module : getOperation().getOps<hw::HWModuleOp>())
      ProvenFeedback{module.getBodyBlock(), verbose}.run();
  }
};

struct Partition : PassWrapper<Partition, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(Partition)
  Partition() = default;
  Partition(const Partition &other) : PassWrapper(other) {}

  Option<unsigned> maxOps{*this, "max-ops",
                          llvm::cl::desc("Maximum initial chunk size"),
                          llvm::cl::init(4096)};
  Option<unsigned> maxPorts{
      *this, "max-ports",
      llvm::cl::desc("Maximum combined boundary port count"),
      llvm::cl::init(16384)};

  Option<bool> resolveFeedback{*this, "resolve-feedback", llvm::cl::init(true)};
  Option<bool> nativeFeedback{*this, "native-feedback", llvm::cl::init(false)};
  StringRef getArgument() const override { return "chipyard-partition"; }
  StringRef getDescription() const override {
    return "Bound native HW compiler working sets while retaining boundary "
           "ports";
  }

  void runOnOperation() override {
    auto top = getOperation();
    auto root = top.lookupSymbol<hw::HWModuleOp>("ArcModel");
    if (!root)
      return;
    if (auto count = ArrayFeedback(root.getBodyBlock()).run())
      llvm::errs() << "Resolved " << count
                   << " cyclic aggregate element reads\n";
    if (auto count = PrefixFeedback{root.getBodyBlock()}.run())
      llvm::errs() << "Resolved " << count
                   << " finite shifted prefix recurrences\n";
    if (resolveFeedback)
      ProvenFeedback{root.getBodyBlock(), false, nativeFeedback}.run();
    if (maxOps == 0)
      return;

    // CIRCT's remove-i0 pass unwraps singleton arrays, but does not adapt
    // array_concat operands. Express concatenation through element reads and
    // array_create so singleton removal stays valid. Retain arrays for Arc's
    // memory-oriented lowering instead of creating giant LLVM integers.
    SmallVector<hw::ArrayConcatOp> concats;
    root.walk([&](hw::ArrayConcatOp op) { concats.push_back(op); });
    for (auto concat : concats) {
      OpBuilder builder(concat);
      SmallVector<Value> elements;
      for (Value array : concat.getInputs()) {
        auto type = cast<hw::ArrayType>(array.getType());
        unsigned width =
            std::max(1U, llvm::Log2_64_Ceil(type.getNumElements()));
        for (uint64_t i = type.getNumElements(); i > 0; --i) {
          auto index = hw::ConstantOp::create(
              builder, concat.getLoc(), builder.getIntegerType(width), i - 1);
          elements.push_back(
              hw::ArrayGetOp::create(builder, concat.getLoc(), array, index));
        }
      }
      auto create =
          hw::ArrayCreateOp::create(builder, concat.getLoc(), elements);
      create->setDiscardableAttrs(concat->getDiscardableAttrDictionary());
      concat.replaceAllUsesWith(create.getResult());
      concat.erase();
    }

    SmallVector<Operation *> ops;
    // Keep original literals in the root for direct output uses. Each
    // partition rematerializes the literals it reads rather than capturing
    // them as dynamic ports or moving the root's output definition away.
    for (auto &op : root.getBodyBlock()->without_terminator())
      if (!canRematerialize(&op))
        ops.push_back(&op);
    uint64_t portEstimate = 0;
    for (auto *op : ops)
      op->walk([&](Operation *nested) {
        portEstimate += nested->getNumOperands() + nested->getNumResults();
      });
    if (ops.size() <= maxOps && portEstimate <= maxPorts)
      return;

    unsigned count = ops.size();
    SmallVector<unsigned> parents(count);
    std::iota(parents.begin(), parents.end(), 0);
    auto find = [&](unsigned i) {
      while (parents[i] != i) {
        parents[i] = parents[parents[i]];
        i = parents[i];
      }
      return i;
    };
    DenseMap<Operation *, unsigned> ids;
    for (auto [i, op] : llvm::enumerate(ops))
      ids[op] = i;

    auto topOwner = [&](Operation *op) -> Operation * {
      while (op && op->getParentOp() != root.getOperation())
        op = op->getParentOp();
      return op;
    };
    auto owner = [&](Value value) { return topOwner(value.getDefiningOp()); };

    // Only hardware values and clocks may cross the new module boundaries.
    // Keep each memory handle (or other opaque state) with all of its ports.
    // Do this per operation before packing: joining preexisting chunks would
    // transitively merge unrelated memories into a giant module.
    for (auto *op : ops) {
      if (canRematerialize(op))
        continue;
      for (auto result : op->getResults()) {
        if (isHardwareValue(result.getType()))
          continue;
        for (auto *user : result.getUsers()) {
          auto *parent = topOwner(user);
          if (parent && ids.contains(parent))
            parents[find(ids[op])] = find(ids[parent]);
        }
      }
    }

    SmallVector<unsigned> sizes(count, 0), ports(count, 0);
    SmallVector<unsigned> bundleGroup(count, ~0U);
    for (unsigned i = 0; i < count; ++i) {
      unsigned bundle = find(i);
      ++sizes[bundle];
      ops[i]->walk([&](Operation *op) {
        ports[bundle] += op->getNumOperands() + op->getNumResults();
      });
    }
    // The operand/result total above is only a conservative packing estimate.
    // Large procedural regions can have many internal CFG values but very few
    // hardware boundary ports. Before rejecting an indivisible bundle, count
    // only its distinct captures and escaping results. Keep the established
    // packing of bundles that already fit unchanged.
    DenseMap<unsigned, unsigned> exactPorts;
    DenseSet<std::pair<unsigned, Value>> seenPorts;
    for (unsigned i = 0; i < count; ++i) {
      auto bundle = find(i);
      if (ports[bundle] <= maxPorts)
        continue;
      exactPorts.try_emplace(bundle, 0);
      auto addPort = [&](Value value) {
        if (seenPorts.insert({bundle, value}).second)
          ++exactPorts[bundle];
      };
      ops[i]->walk([&](Operation *nested) {
        for (auto value : nested->getOperands()) {
          auto *def = owner(value);
          bool external =
              def ? !ids.contains(def) || find(ids.lookup(def)) != bundle
                  : value.getParentBlock() == root.getBodyBlock();
          if (external && !canRematerialize(def))
            addPort(value);
        }
      });
      for (auto value : ops[i]->getResults())
        for (auto *user : value.getUsers()) {
          auto *parent = topOwner(user);
          if (parent &&
              (!ids.contains(parent) || find(ids.lookup(parent)) != bundle)) {
            addPort(value);
            break;
          }
        }
    }
    for (auto [bundle, exact] : exactPorts)
      ports[bundle] = exact;

    unsigned nextGroup = 0, groupSize = 0, groupPorts = 0;
    for (unsigned i = 0; i < count; ++i) {
      unsigned bundle = find(i);
      if (bundleGroup[bundle] != ~0U)
        continue;
      if (ports[bundle] > maxPorts) {
        ops[i]->emitError(
            "a memory/handle bundle exceeds the compiler port budget");
        return signalPassFailure();
      }
      if (groupSize && (groupSize + sizes[bundle] > maxOps ||
                        groupPorts + ports[bundle] > maxPorts)) {
        ++nextGroup;
        groupSize = groupPorts = 0;
      }
      bundleGroup[bundle] = nextGroup;
      groupSize += sizes[bundle];
      groupPorts += ports[bundle];
    }

    std::map<unsigned, Group> groups;
    for (auto *op : ops) {
      ids[op] = bundleGroup[find(ids[op])];
      groups[ids[op]].operations.push_back(op);
    }
    for (auto &[id, group] : groups) {
      for (auto *op : group.operations) {
        // Region captures count as inputs too. Region-local block arguments
        // stay local and must not become hardware ports.
        op->walk([&](Operation *nested) {
          for (auto value : nested->getOperands()) {
            auto *def = owner(value);
            bool external = def ? !ids.contains(def) || ids.lookup(def) != id
                                : value.getParentBlock() == root.getBodyBlock();
            if (!external)
              continue;
            if (canRematerialize(def))
              group.constants.insert(def);
            else
              group.inputs.insert(value);
          }
        });
        if (canRematerialize(op))
          continue;
        for (auto result : op->getResults()) {
          for (auto *user : result.getUsers()) {
            auto *parent = topOwner(user);
            if (parent && (!ids.contains(parent) || ids.lookup(parent) != id)) {
              group.outputs.insert(result);
              break;
            }
          }
        }
      }
    }

    OpBuilder builder(root);
    for (auto &[id, group] : groups) {
      SmallVector<hw::PortInfo> modulePorts;
      for (auto [i, value] : llvm::enumerate(group.inputs))
        modulePorts.push_back({{builder.getStringAttr("in" + std::to_string(i)),
                                value.getType(), hw::ModulePort::Input},
                               i});
      for (auto [i, value] : llvm::enumerate(group.outputs))
        modulePorts.push_back(
            {{builder.getStringAttr("out" + std::to_string(i)), value.getType(),
              hw::ModulePort::Output},
             i});
      std::string name = "ArcModel_part_" + std::to_string(id);
      while (top.lookupSymbol(name))
        name += "_";
      group.module = hw::HWModuleOp::create(
          builder, root.getLoc(), builder.getStringAttr(name), modulePorts);
      SymbolTable::setSymbolVisibility(group.module,
                                       SymbolTable::Visibility::Private);
      auto *block = group.module.getBodyBlock();
      auto *end = block->getTerminator();
      DenseMap<Value, Value> mapping;
      for (auto [i, value] : llvm::enumerate(group.inputs))
        mapping[value] = block->getArgument(i);
      OpBuilder local(block, block->begin());
      for (auto *constant : group.constants) {
        auto *copy = local.clone(*constant);
        for (auto [original, replacement] :
             llvm::zip(constant->getResults(), copy->getResults()))
          mapping[original] = replacement;
      }
      // Move complete operations, including procedural regions. Forward SSA
      // references within a group remain valid; only external captures change.
      for (auto *op : group.operations)
        op->moveBefore(end);
      for (auto *op : group.operations) {
        op->walk([&](Operation *nested) {
          for (auto &operand : nested->getOpOperands())
            if (auto it = mapping.find(operand.get()); it != mapping.end())
              operand.set(it->second);
        });
      }
      end->setOperands(group.outputs.getArrayRef());
    }

    // Build every instance before replacing uses: state feedback can cross
    // group boundaries, and HW graph regions permit those forward references.
    builder.setInsertionPoint(root.getBodyBlock()->getTerminator());
    for (auto &[id, group] : groups)
      group.instance = hw::InstanceOp::create(
          builder, root.getLoc(), group.module, "part_" + std::to_string(id),
          group.inputs.getArrayRef());
    for (auto &[id, group] : groups) {
      for (auto [i, original] : llvm::enumerate(group.outputs)) {
        Value value = original;
        value.replaceUsesWithIf(
            group.instance.getResult(i), [&](OpOperand &use) {
              return use.getOwner()->getParentOfType<hw::HWModuleOp>() == root;
            });
      }
    }
    llvm::errs() << "Partitioned " << ops.size() << " operations into "
                 << groups.size() << " HW modules\n";
  }
};
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardHWPartition", "1", [] {
            PassRegistration<Partition>();
            PassRegistration<ResolveFeedback>();
          }};
}
