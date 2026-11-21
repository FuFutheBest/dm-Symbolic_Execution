// Your part of the symbolic executor: fill in the functions marked TODO.
//
// The functions are grouped in feature blocks (see the handbook,
// "实验内容", and tasks/blocks.json). Each block is graded on its own
// tasks, and `make grade` shows the score of every block. Until a function
// is done it throws Unsupported, and the answer is "unknown" -- never a
// wrong "safe". Keep that property in whatever you write.
//
// Useful from the framework (engine.cpp):
//   exec(stmt, st)   all states after running a statement (a Block, a
//   branch, a body) eval(expr, st)   value of an expression, as Results
//   (one per resulting state) ctx_             the z3::context;
//   ctx_.bv_val(5, 32), ctx_.bool_val(true) model_           set it to the
//   model when a failure is found incomplete_      set it when z3 cannot
//   decide a query
#include "engine.h"
#include "z3++.h"
#include <optional>

using minic::Stmt;

// =====================================================================
// base Handbook 3.2. The two z3 queries; with them the tool handles
// straight-line programs.

// Can some input reach state `st`, i.e. is st.pc satisfiable?
// Return false only when z3 answers unsat. When z3 answers unknown (for
// instance after a timeout) the path may be real: keep it, return true.
bool Engine::feasible(const State &st) {
  z3::solver solver(ctx_);
  solver.add(st.pc);
  auto result = solver.check();
  if (result == z3::unsat) {
    return false;
  }
  return true;
}

// Can the program fail at this point? `fail` is the failure condition here
// (for assert(c) it is !c). Ask z3 whether st.pc && fail is satisfiable:
//   sat     -> some input fails: store s.get_model() in model_
//   unsat   -> no input fails here
//   unknown -> undecided: set incomplete_ = true, so the answer cannot be
//   "safe"
void Engine::check(const State &st, const z3::expr &fail) {
  z3::solver solver(ctx_);

  solver.add(st.pc);
  solver.add(fail);

  auto result = solver.check();

  if (result == z3::sat) {
    model_ = solver.get_model();
  } else if (result == z3::unknown) {
    incomplete_ = true;
  }
}

// =======================================================================
// if Handbook 3.5.

// if (c) { then_block } else { else_block }
// The condition can call functions, so evaluate it with eval(*s.cond, st)
// and handle every Result. For each one, make a state where the condition
// holds and one where it does not, drop those that are not feasible, and
// run the branches with exec(). s.else_block is null when there is no else.
// Return all states that come out, through merge().
std::vector<State> Engine::exec_if(const Stmt &s, State st) {
  auto c = eval(*s.cond, st);
  bool no_else = (s.else_block == nullptr);
  std::vector<State> res;

  for (const auto &r : c) {
    auto true_cond = r.state;
    true_cond.pc = true_cond.pc & r.value;
    if (feasible(true_cond)) {
      auto after = exec(*s.then_block, true_cond);
      res.reserve(res.size() + after.size());
      res.insert(res.end(), after.begin(), after.end());
    }

    auto false_cond = r.state;
    false_cond.pc = false_cond.pc & !r.value;
    if (feasible(false_cond)) {
      auto after = no_else ? std::vector<State>{false_cond}
                           : exec(*s.else_block, false_cond);
      res.reserve(res.size() + after.size());
      res.insert(res.end(), after.begin(), after.end());
    }
  }

  return merge(res);
}

// =====================================================================
// loop Handbook 3.5.

// for i in LO..HI { body }      (s.name is i, s.lo and s.hi the bounds)
// Unroll the loop: for i = s.lo, ..., s.hi - 1, store the constant i as the
// loop variable in every running state and run exec(*s.body, ...). Then
// sort the states by their flow:
//   Normal, Continue  run the next iteration (set flow back to Normal)
//   Break             leave the loop: keep it for after the loop (flow
//   Normal) Return            leave the loop and the function: keep it as
//   it is
// Stop early when no state runs the next iteration. At the end, remove the
// loop variable from every state's store and return them all, through
// merge().
std::vector<State> Engine::exec_for(const Stmt &s, State st) {
  std::vector<State> running = {st};
  std::vector<State> finished;

  for (int64_t i = s.lo; i < s.hi && !running.empty(); ++i) {
    std::vector<State> next;
    for (State &state : running) {
      state.store.insert_or_assign(s.name, Value{ctx_.bv_val(i, 32)});
      for (State result : exec(*s.body, state)) {
        switch (result.flow) {
        case State::Normal:
        case State::Continue:
          result.flow = State::Normal;
          next.push_back(result);
          break;
        case State::Break:
          result.flow = State::Normal;
          finished.push_back(result);
          break;
        case State::Return:
          finished.push_back(result);
          break;
        }
      }
    }
    running = merge(next);
  }

  finished.insert(finished.end(), running.begin(), running.end());
  for (State &state : finished)
    state.store.erase(s.name);
  return merge(finished);
}

// =====================================================================
// call Handbook 3.6.

// A call of f with argument values `args` (in parameter order; arrays are
// already copied, since MiniC passes them by value). Run f.body in a new
// state that sees only the parameters and starts with the caller's path
// condition. Every state that comes out of the body is one way the call can
// end: flow Return, or Normal when a void function reaches its end. For
// each, the result is the caller's state with the new path condition,
// together with the returned value (a void function's value is never used;
// any expression, e.g. ctx_.bool_val(true), will do). The callee's
// variables are gone after the call. Pass the states that come out of the
// body through merge() before building the Results.
Results Engine::call(const minic::Func &f, const std::vector<Value> &args,
                     const State &caller) {
  State callee(ctx_);
  callee.pc = caller.pc;
  for (size_t k = 0; k < f.params.size(); ++k) {
    callee.store.insert_or_assign(f.params[k].name, args[k]);
  }
  auto exec_res = exec(*f.body, callee);
  auto return_res = merge(exec_res);
  Results rt;
  for (const auto &r : return_res) {
    auto copy = caller;
    copy.pc = r.pc;
    rt.push_back(
        {copy, r.ret.has_value() ? r.ret.value() : ctx_.bool_val(true)});
  }
  return rt;
}

// ======================================================================
// ops Handbook 3.3: division, remainder, shifts, and when they trap.

// a op b for every binary operator except && and ||, which the framework
// handles. Operands are int (32-bit bit-vectors), or bool for == and !=.
z3::expr Engine::binary(const std::string &op, const z3::expr &a,
                        const z3::expr &b) {
  if (op == "+")
    return a + b;
  if (op == "-")
    return a - b;
  if (op == "*")
    return a * b;
  if (op == "&")
    return a & b;
  if (op == "|")
    return a | b;
  if (op == "^")
    return a ^ b;
  if (op == "==")
    return a == b;
  if (op == "!=")
    return a != b;
  if (op == "<")
    return a < b; // z3's < on bit-vectors is signed, like MiniC's int
  if (op == "<=")
    return a <= b;
  if (op == ">")
    return a > b;
  if (op == ">=")
    return a >= b;
  if (op == "<<")
    return z3::shl(a, b);
  if (op == ">>")
    return z3::ashr(a, b);
  if (op == "/")
    return a / b;
  if (op == "%")
    return z3::srem(a, b);
  throw Unsupported("TODO: operator " + op);
}

// When does a / b (or a % b) trap? Return the condition as a z3 boolean.
z3::expr Engine::div_failure(const z3::expr &a, const z3::expr &b) {
  z3::expr min_int = ctx_.bv_val("-2147483648", 32);
  return (b == 0) || (a == min_int && b == -1);
}

// When does a shift by `amount` trap?
z3::expr Engine::shift_failure(const z3::expr &amount) {
  return (amount < 0) || (amount >= 32);
}

// =============================================================== carr,
// sarr Handbook 3.4. Each of the three array functions has two parts: a
// constant index (carr: the index is a loop variable or otherwise the same
// on every input) and a symbolic index (sarr: the index depends on the
// input). To tell them apart: z3::expr k = i.simplify(); k.is_numeral()
// says whether the index is a constant, and k.get_numeral_uint64() gives
// its 32 bits. The framework calls index_failure before read and write, so
// an index that reaches them is in range on every input that gets there.

// When does indexing an array of length n with i trap?
z3::expr Engine::index_failure(const z3::expr &i, int64_t n) {
  z3::expr index = z3::bv2int(i, true);
  return (index < 0) || (index >= ctx_.int_val(n));
}

// The element arr[i].
z3::expr Engine::read(const Value &arr, const z3::expr &i) {
  z3::expr index = i.simplify();
  if (index.is_numeral()) {
    uint64_t k = index.get_numeral_uint64();
    return arr[static_cast<size_t>(k)];
  }

  z3::expr value = arr.back();
  for (size_t k = arr.size() - 1; k > 0; --k) {
    z3::expr at = ctx_.bv_val(static_cast<uint64_t>(k - 1), 32);
    value = z3::ite(index == at, arr[k - 1], value);
  }
  return value;
}

// arr[i] = v.
void Engine::write(Value &arr, const z3::expr &i, const z3::expr &v) {
  z3::expr index = i.simplify();
  if (index.is_numeral()) {
    uint64_t k = index.get_numeral_uint64();
    arr[static_cast<size_t>(k)] = v;
    return;
  }

  for (size_t k = 0; k < arr.size(); ++k) {
    z3::expr at = ctx_.bv_val(static_cast<uint64_t>(k), 32);
    arr[k] = z3::ite(index == at, v, arr[k]);
  }
}

// ====================================================================
// merge Handbook 3.7.1.

// exec_if, exec_for and call pass their resulting states through here.
// Merging states that reached the same point into one keeps their number
// from doubling at every branch. As given, it merges nothing, which is
// correct but slow: the merge tasks time out without it.
// TODO (merge): group the states by flow (Normal, Break, Continue, Return)
// and merge each group into one state: pc is the disjunction of the pcs,
// every variable becomes ite(pc_1, v_1, ite(pc_2, v_2, ...)), and so does
// the returned value `ret` of Return states. States at the same point hold
// the same variables. Note that after a merge a variable can hold an
// ite(...) expression, so an array index that used to be a constant may
// become symbolic: merging needs the sarr block.
std::vector<State> Engine::merge(std::vector<State> states) { return states; }
