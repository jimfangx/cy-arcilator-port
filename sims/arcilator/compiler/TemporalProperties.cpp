// Compile finite sequence antecedents into sampled native assertion monitors.
// Keep unsupported temporal operators intact so the consumer diagnoses them.
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/LTL/LTLOps.h"
#include "circt/Dialect/Seq/SeqOps.h"
#include "circt/Dialect/Verif/VerifOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
#include <map>
#include <memory>

using namespace mlir;
using namespace circt;
namespace {
// Expressions describe samples relative to the start of a sequence. A shift
// moves the entire expression in time without expanding or copying its DAG.
struct Expr {
  enum Kind { Atom, Constant, And, Or, Not, Shift } kind;
  Value atom;
  int argument;
  Expr *left, *right;
};
using Matches = std::map<unsigned, Expr *>;
struct Monitor {
  static constexpr unsigned maxDelay = 4096;
  SmallVector<std::unique_ptr<Expr>> arena;
  DenseMap<Value, Matches> sequences;
  DenseMap<Value, Expr *> pointwise;
  bool failed = false;
  Expr *make(Expr::Kind kind, Expr *a = nullptr, Expr *b = nullptr,
             int argument = 0, Value atom = {}) {
    if (arena.size() >= 100000) {
      failed = true;
      return nullptr;
    }
    arena.push_back(std::make_unique<Expr>(Expr{kind, atom, argument, a, b}));
    return arena.back().get();
  }
  Expr *boolean(Value v) {
    if (pointwise.count(v))
      return pointwise.lookup(v);
    pointwise[v] = nullptr;
    Expr *result = nullptr;
    if (v.getType().isInteger(1))
      result = make(Expr::Atom, nullptr, nullptr, 0, v);
    else if (auto *op = v.getDefiningOp()) {
      if (auto c = dyn_cast<ltl::BooleanConstantOp>(op))
        result = make(Expr::Constant, nullptr, nullptr, c.getValue());
      else if (isa<ltl::NotOp>(op)) {
        if (auto a = boolean(op->getOperand(0)))
          result = make(Expr::Not, a);
      } else if (isa<ltl::ImplicationOp>(op)) {
        auto a = boolean(op->getOperand(0));
        auto b = boolean(op->getOperand(1));
        if (a && b)
          result = make(Expr::Or, make(Expr::Not, a), b);
      } else if (isa<ltl::AndOp, ltl::OrOp, ltl::IntersectOp, ltl::ConcatOp>(
                     op) &&
                 op->getNumOperands()) {
        bool isOr = isa<ltl::OrOp>(op);
        result = boolean(op->getOperand(0));
        for (Value input : op->getOperands().drop_front()) {
          auto next = boolean(input);
          if (!result || !next) {
            result = nullptr;
            break;
          }
          result = make(isOr ? Expr::Or : Expr::And, result, next);
        }
      } else if (auto delay = dyn_cast<ltl::DelayOp>(op)) {
        if (delay.getDelay() == 0 && delay.getLength() == 0)
          result = boolean(delay.getInput());
      } else if (auto repeat = dyn_cast<ltl::RepeatOp>(op)) {
        if (repeat.getBase() == 1 && repeat.getMore() == 0)
          result = boolean(repeat.getInput());
      }
    }
    pointwise[v] = result;
    return result;
  }
  void add(Matches &m, unsigned end, Expr *expr) {
    if (end > maxDelay || !expr) {
      failed = true;
      return;
    }
    auto [it, inserted] = m.emplace(end, expr);
    if (!inserted)
      it->second = make(Expr::Or, it->second, expr);
  }
  Matches join(const Matches &a, const Matches &b, unsigned gap = 0) {
    Matches out;
    for (auto [endA, exprA] : a)
      for (auto [endB, exprB] : b)
        add(out, endA + gap + endB,
            make(Expr::And, exprA,
                 make(Expr::Shift, exprB, nullptr, endA + gap)));
    return out;
  }
  Matches sequence(Value v) {
    if (sequences.count(v))
      return sequences.lookup(v);
    sequences[v] = {};
    Matches out;
    if (v.getType().isInteger(1))
      out[0] = boolean(v);
    else if (auto *op = v.getDefiningOp()) {
      if (auto delay = dyn_cast<ltl::DelayOp>(op)) {
        if (auto length = delay.getLength();
            length && delay.getDelay() <= maxDelay &&
            *length <= maxDelay - delay.getDelay()) {
          auto input = sequence(delay.getInput());
          for (unsigned shift = delay.getDelay();
               shift <= delay.getDelay() + *length; ++shift)
            for (auto [end, expr] : input)
              add(out, end + shift, make(Expr::Shift, expr, nullptr, shift));
        }
      } else if (auto concat = dyn_cast<ltl::ConcatOp>(op)) {
        if (concat.getNumOperands()) {
          out = sequence(concat.getOperand(0));
          for (Value input : concat.getOperands().drop_front())
            out = join(out, sequence(input));
        }
      } else if (auto repeat = dyn_cast<ltl::RepeatOp>(op)) {
        if (auto more = repeat.getMore();
            more && repeat.getBase() >= 1 && repeat.getBase() <= maxDelay &&
            *more <= maxDelay - repeat.getBase()) {
          auto input = sequence(repeat.getInput());
          auto copies = input;
          for (unsigned n = 1; n <= repeat.getBase() + *more; ++n) {
            if (n >= repeat.getBase())
              for (auto [end, expr] : copies)
                add(out, end, expr);
            if (n < repeat.getBase() + *more)
              copies = join(copies, input, 1);
          }
        }
      } else if (isa<ltl::OrOp, ltl::AndOp, ltl::IntersectOp>(op) &&
                 op->getNumOperands()) {
        out = sequence(op->getOperand(0));
        for (Value input : op->getOperands().drop_front()) {
          Matches next = sequence(input), combined;
          if (isa<ltl::OrOp>(op)) {
            combined = out;
            for (auto [end, expr] : next)
              add(combined, end, expr);
          } else {
            for (auto [endA, exprA] : out)
              for (auto [endB, exprB] : next)
                if (!isa<ltl::IntersectOp>(op) || endA == endB)
                  add(combined, std::max(endA, endB),
                      make(Expr::And, exprA, exprB));
          }
          out = std::move(combined);
        }
      }
    }
    if (out.empty())
      failed = true;
    sequences[v] = out;
    return out;
  }
  // Each obligation checks its consequent at the endpoint of a matching
  // antecedent. Nested implications concatenate their activation conditions;
  // conjunction creates independent obligations, preserving their deadlines.
  SmallVector<std::pair<unsigned, Expr *>> obligations;
  bool property(Value v, const Matches &activation) {
    if (auto condition = boolean(v)) {
      for (auto [end, enabled] : activation)
        obligations.emplace_back(
            end, make(Expr::Or, make(Expr::Not, enabled),
                      make(Expr::Shift, condition, nullptr, end)));
      return !failed;
    }
    auto *op = v.getDefiningOp();
    if (!op)
      return false;
    if (auto implication = dyn_cast<ltl::ImplicationOp>(op)) {
      auto antecedent = sequence(implication.getAntecedent());
      if (antecedent.empty())
        return false;
      return property(implication.getConsequent(),
                      join(activation, antecedent));
    }
    if (isa<ltl::AndOp>(op) && op->getNumOperands()) {
      for (Value input : op->getOperands())
        if (!property(input, activation))
          return false;
      return true;
    }
    return false;
  }
};
struct TemporalProperties
    : PassWrapper<TemporalProperties, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TemporalProperties)
  StringRef getArgument() const override {
    return "chipyard-lower-temporal-properties";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<comb::CombDialect, hw::HWDialect, seq::SeqDialect>();
  }
  void runOnOperation() override {
    // ltl.past carries its own clock and has no assertion-disable reset. A past
    // sample remains available even if checks were disabled on that clock tick.
    SmallVector<ltl::PastOp> pasts;
    getOperation().walk([&](ltl::PastOp op) { pasts.push_back(op); });
    for (auto past : pasts) {
      if (past.getDelay() < 0 || past.getDelay() > Monitor::maxDelay)
        continue;
      OpBuilder b(past);
      Value result = past.getInput();
      auto clock = seq::ToClockOp::create(b, past.getLoc(), past.getClk());
      for (int64_t i = 0; i < past.getDelay(); ++i)
        result = seq::FirRegOp::create(b, past.getLoc(), result, clock,
                                       b.getStringAttr("arc_past"));
      past.getResult().replaceAllUsesWith(result);
      past.erase();
    }
    SmallVector<Operation *> checks;
    getOperation().walk([&](Operation *op) {
      if (isa<verif::ClockedAssertOp, verif::ClockedAssumeOp>(op))
        checks.push_back(op);
    });
    unsigned count = 0;
    for (auto *check : checks) {
      if (check->getOperand(0).getType().isInteger(1))
        continue;
      auto edge = check->getAttrOfType<verif::ClockEdgeAttr>("edge").getValue();
      if (edge == verif::ClockEdge::Both)
        continue;
      Monitor monitor;
      auto yes = monitor.make(Expr::Constant, nullptr, nullptr, 1);
      if (!monitor.property(check->getOperand(0), {{0, yes}}) || monitor.failed)
        continue;
      OpBuilder b(check);
      auto loc = check->getLoc();
      Value one = hw::ConstantOp::create(b, loc, APInt(1, 1));
      Value zero = hw::ConstantOp::create(b, loc, APInt(1, 0));
      // ODS order: property, clock, optional enable (edge is an attribute).
      Value clock = check->getOperand(1);
      if (edge == verif::ClockEdge::Neg)
        clock = comb::XorOp::create(b, loc, clock, one);
      clock = seq::ToClockOp::create(b, loc, clock);
      Value enable = check->getNumOperands() == 3 ? check->getOperand(2) : one;
      Value disable = comb::XorOp::create(b, loc, enable, one);
      SmallVector<Value> valid{one};
      DenseMap<Value, SmallVector<Value>> history;
      auto validAt = [&](unsigned delay) {
        while (valid.size() <= delay)
          valid.push_back(
              seq::FirRegOp::create(b, loc, valid.back(), clock,
                                    b.getStringAttr("arc_property_valid"),
                                    disable, zero, hw::InnerSymAttr{}, true));
        return valid[delay];
      };
      auto sample = [&](Value atom, unsigned delay) {
        auto &samples = history[atom];
        if (samples.empty())
          samples.push_back(atom);
        while (samples.size() <= delay)
          samples.push_back(
              seq::FirRegOp::create(b, loc, samples.back(), clock,
                                    b.getStringAttr("arc_property_sample")));
        return samples[delay];
      };
      std::map<std::pair<Expr *, unsigned>, Value> evaluated;
      std::function<Value(Expr *, unsigned)> evaluate =
          [&](Expr *expr, unsigned end) -> Value {
        auto key = std::make_pair(expr, end);
        if (evaluated.count(key))
          return evaluated[key];
        Value result;
        switch (expr->kind) {
        case Expr::Atom:
          result = sample(expr->atom, end);
          break;
        case Expr::Constant:
          result = expr->argument ? one : zero;
          break;
        case Expr::Not:
          result = comb::XorOp::create(b, loc, evaluate(expr->left, end), one);
          break;
        case Expr::Shift:
          assert(end >= unsigned(expr->argument));
          result = evaluate(expr->left, end - expr->argument);
          break;
        case Expr::And:
        case Expr::Or: {
          auto a = evaluate(expr->left, end), c = evaluate(expr->right, end);
          result = expr->kind == Expr::And
                       ? Value(comb::AndOp::create(b, loc, a, c))
                       : Value(comb::OrOp::create(b, loc, a, c));
          break;
        }
        }
        evaluated[key] = result;
        return result;
      };
      SmallVector<Value> predicates;
      for (auto [end, expr] : monitor.obligations) {
        auto value = evaluate(expr, end);
        auto invalid = comb::XorOp::create(b, loc, validAt(end), one);
        predicates.push_back(comb::OrOp::create(b, loc, invalid, value));
      }
      Value predicate = one;
      if (!predicates.empty())
        predicate = comb::AndOp::create(b, loc, predicates, false);
      check->setOperand(0, predicate);
      ++count;
    }
    if (count)
      llvm::errs() << "Lowered " << count << " finite temporal monitors\n";
  }
};
} // namespace
extern "C" LLVM_ATTRIBUTE_WEAK mlir::PassPluginLibraryInfo
mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "ChipyardTemporalProperties", "1",
          [] { PassRegistration<TemporalProperties>(); }};
}
