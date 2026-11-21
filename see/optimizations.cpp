// Optimizations from lab-manual sections 3.7.1, 3.7.2 and appendix B.
#include "engine.h"
#include <algorithm>

namespace {
int64_t signed_value(const z3::expr &e) {
  uint64_t bits = e.get_numeral_uint64();
  return bits >= (uint64_t(1) << 31)
             ? static_cast<int64_t>(bits) - (int64_t(1) << 32)
             : static_cast<int64_t>(bits);
}
} // namespace

z3::expr Engine::simplified(const z3::expr &e) {
  if (!options_.simplification || e.is_const())
    return e;
  auto it = simplify_cache_.find(e.id());
  if (it != simplify_cache_.end())
    return it->second.second;
  z3::expr result = e.simplify();
  // Retain the source expression too: AST IDs can be reused after deletion.
  if (simplify_cache_.size() >= 4096)
    simplify_cache_.clear();
  simplify_cache_.emplace(e.id(), std::make_pair(e, result));
  return result;
}

z3::expr Engine::choose(const z3::expr &cond, const z3::expr &yes,
                        const z3::expr &no) {
  if (z3::eq(yes, no))
    return yes;
  if (cond.is_true())
    return yes;
  if (cond.is_false())
    return no;
  return simplified(z3::ite(cond, yes, no));
}

void Engine::prepare_path(const z3::expr &pc) {
  // Flatten conjunctions without recursion, preserving their order. Keep
  // the longest common prefix asserted across consecutive queries.
  std::vector<z3::expr> path, todo = {pc};
  while (!todo.empty()) {
    z3::expr e = todo.back();
    todo.pop_back();
    if (e.is_and()) {
      for (unsigned k = e.num_args(); k > 0; --k)
        todo.push_back(e.arg(k - 1));
    } else if (!e.is_true()) {
      path.push_back(e);
    }
  }
  size_t common = 0;
  while (common < path.size() && common < solver_path_.size() &&
         z3::eq(path[common], solver_path_[common]))
    ++common;
  if (solver_path_.size() > common)
    solver_.pop(static_cast<unsigned>(solver_path_.size() - common));
  solver_path_.erase(solver_path_.begin() + common, solver_path_.end());
  for (size_t k = common; k < path.size(); ++k) {
    solver_.push();
    solver_.add(path[k]);
    solver_path_.push_back(path[k]);
  }
}

z3::check_result Engine::query(const z3::expr &pc, const z3::expr &condition,
                               bool save_model) {
  using namespace std::chrono;
  auto used = duration_cast<milliseconds>(solver_time_).count();
  if (options_.solver_budget_ms && used >= options_.solver_budget_ms)
    return z3::unknown;
  unsigned timeout = options_.timeout_ms;
  if (options_.solver_budget_ms) {
    unsigned left = options_.solver_budget_ms - static_cast<unsigned>(used);
    timeout = timeout ? std::min(timeout, left) : left;
  }
  z3::params params(ctx_);
  params.set("timeout", timeout);
  params.set("rlimit", options_.rlimit);
  auto solve = [&](z3::solver &s) {
    s.set(params);
    auto start = steady_clock::now();
    z3::check_result result = s.check();
    solver_time_ += steady_clock::now() - start;
    if (save_model && result == z3::sat)
      model_ = s.get_model();
    return result;
  };
  if (!options_.incremental) {
    z3::solver fresh(ctx_);
    fresh.add(pc);
    fresh.add(condition);
    return solve(fresh);
  }
  prepare_path(pc);
  solver_.push();
  solver_.add(condition);
  z3::check_result result = solve(solver_);
  solver_.pop();
  return result;
}

void Engine::flush_failures() {
  if (pending_failures_.empty() || model_)
    return;
  std::vector<z3::expr> pending;
  pending.swap(pending_failures_);
  // Batch unknown is not evidence of safety. Split it into smaller queries;
  // a leaf that remains unknown permanently prevents a safe verdict.
  std::vector<std::pair<size_t, size_t>> ranges = {{0, pending.size()}};
  while (!ranges.empty() && !model_) {
    auto [lo, hi] = ranges.back();
    ranges.pop_back();
    z3::expr_vector terms(ctx_);
    for (size_t k = lo; k < hi; ++k)
      terms.push_back(pending[k]);
    z3::expr failure = simplified(z3::mk_or(terms));
    auto result = query(ctx_.bool_val(true), failure, true);
    if (result == z3::unknown) {
      auto used =
          std::chrono::duration_cast<std::chrono::milliseconds>(solver_time_)
              .count();
      if (hi - lo == 1 ||
          (options_.solver_budget_ms && used >= options_.solver_budget_ms)) {
        incomplete_ = true;
      } else {
        size_t mid = lo + (hi - lo) / 2;
        ranges.push_back({mid, hi});
        ranges.push_back({lo, mid});
      }
    }
  }
}

std::optional<z3::expr> Engine::fold_binary(const std::string &op,
                                            const z3::expr &a,
                                            const z3::expr &b) {
  if (!options_.constant_folding)
    return std::nullopt;
  if ((a.is_true() || a.is_false()) && (b.is_true() || b.is_false())) {
    if (op == "==")
      return ctx_.bool_val(a.is_true() == b.is_true());
    if (op == "!=")
      return ctx_.bool_val(a.is_true() != b.is_true());
    return std::nullopt;
  }
  if (!a.is_numeral() || !b.is_numeral())
    return std::nullopt;
  int64_t x = signed_value(a), y = signed_value(b);
  auto value = [&](int64_t v) { return ctx_.bv_val(minic::wrap(v), 32); };
  if (op == "+")
    return value(x + y);
  if (op == "-")
    return value(x - y);
  if (op == "*")
    return value(x * y); // 32-bit operands fit in int64_t.
  if (op == "==")
    return ctx_.bool_val(x == y);
  if (op == "!=")
    return ctx_.bool_val(x != y);
  if (op == "<")
    return ctx_.bool_val(x < y);
  if (op == "<=")
    return ctx_.bool_val(x <= y);
  if (op == ">")
    return ctx_.bool_val(x > y);
  if (op == ">=")
    return ctx_.bool_val(x >= y);
  uint64_t ux = a.get_numeral_uint64(), uy = b.get_numeral_uint64();
  if (op == "&")
    return value(static_cast<int64_t>(ux & uy));
  if (op == "|")
    return value(static_cast<int64_t>(ux | uy));
  if (op == "^")
    return value(static_cast<int64_t>(ux ^ uy));
  if (op == "/" || op == "%") {
    if (y == 0 || (x == -(int64_t(1) << 31) && y == -1))
      return std::nullopt; // Never evaluate a trap with host arithmetic.
    return value(op == "/" ? x / y : x % y);
  }
  if (op == "<<" || op == ">>") {
    if (y < 0 || y >= 32)
      return std::nullopt;
    if (op == "<<")
      return value(static_cast<int64_t>((ux << y) & 0xffffffffULL));
    // Portable arithmetic right shift; no signed-negative C++ shift.
    int64_t divisor = int64_t(1) << y;
    return value(x >= 0 ? x / divisor : -1 - ((-1 - x) / divisor));
  }
  return std::nullopt;
}

z3::expr Engine::unary(const std::string &op, const z3::expr &a) {
  if (options_.constant_folding) {
    if (op == "!" && (a.is_true() || a.is_false()))
      return ctx_.bool_val(a.is_false());
    if (a.is_numeral()) {
      int64_t x = signed_value(a);
      if (op == "-")
        return ctx_.bv_val(minic::wrap(-x), 32);
      if (op == "~")
        return ctx_.bv_val(minic::wrap(-1 - x), 32);
    }
  }
  return simplified(op == "!" ? !a : op == "-" ? -a : ~a);
}

z3::expr Engine::as_array(const Value &arr) {
  if (arr.empty())
    throw Unsupported("empty array");
  z3::expr result = z3::const_array(ctx_.bv_sort(32), arr.back());
  for (size_t k = 0; k + 1 < arr.size(); ++k)
    result =
        z3::store(result, ctx_.bv_val(static_cast<uint64_t>(k), 32), arr[k]);
  return result;
}
