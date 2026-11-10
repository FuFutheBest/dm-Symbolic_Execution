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

using minic::Stmt;

// =====================================================================
// base Handbook 3.2. The two z3 queries; with them the tool handles
// straight-line programs.

// Can some input reach state `st`, i.e. is st.pc satisfiable?
// Return false only when z3 answers unsat. When z3 answers unknown (for
// instance after a timeout) the path may be real: keep it, return true.
bool Engine::feasible (const State &st) {
  (void) st;
  throw Unsupported ("TODO: feasible");
}

// Can the program fail at this point? `fail` is the failure condition here
// (for assert(c) it is !c). Ask z3 whether st.pc && fail is satisfiable:
//   sat     -> some input fails: store s.get_model() in model_
//   unsat   -> no input fails here
//   unknown -> undecided: set incomplete_ = true, so the answer cannot be
//   "safe"
void Engine::check (const State &st, const z3::expr &fail) {
  (void) st;
  (void) fail;
  throw Unsupported ("TODO: check");
}

// =======================================================================
// if Handbook 3.5.

// if (c) { then_block } else { else_block }
// The condition can call functions, so evaluate it with eval(*s.cond, st)
// and handle every Result. For each one, make a state where the condition
// holds and one where it does not, drop those that are not feasible, and
// run the branches with exec(). s.else_block is null when there is no else.
// Return all states that come out, through merge().
std::vector<State> Engine::exec_if (const Stmt &s, State st) {
  (void) s;
  (void) st;
  throw Unsupported ("TODO: exec_if");
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
std::vector<State> Engine::exec_for (const Stmt &s, State st) {
  (void) s;
  (void) st;
  throw Unsupported ("TODO: exec_for");
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
Results Engine::call (const minic::Func &f, const std::vector<Value> &args,
                      const State &caller) {
  (void) f;
  (void) args;
  (void) caller;
  throw Unsupported ("TODO: call");
}

// ======================================================================
// ops Handbook 3.3: division, remainder, shifts, and when they trap.

// a op b for every binary operator except && and ||, which the framework
// handles. Operands are int (32-bit bit-vectors), or bool for == and !=.
z3::expr Engine::binary (const std::string &op, const z3::expr &a,
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
  // TODO: / % << >>
  // Careful: not every C++ operator on z3::expr means what MiniC means, and
  // a << b on z3::expr even compiles but is NOT a shift. See section 3.3.
  throw Unsupported ("TODO: operator " + op);
}

// When does a / b (or a % b) trap? Return the condition as a z3 boolean.
z3::expr Engine::div_failure (const z3::expr &a, const z3::expr &b) {
  (void) a;
  (void) b;
  throw Unsupported ("TODO: div_failure");
}

// When does a shift by `amount` trap?
z3::expr Engine::shift_failure (const z3::expr &amount) {
  (void) amount;
  throw Unsupported ("TODO: shift_failure");
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
z3::expr Engine::index_failure (const z3::expr &i, int64_t n) {
  (void) i;
  (void) n;
  // TODO (carr): a constant index -- the answer is ctx_.bool_val(true) or
  // (false)
  // TODO (sarr): an index that is not a constant
  throw Unsupported ("TODO: index_failure");
}

// The element arr[i].
z3::expr Engine::read (const Value &arr, const z3::expr &i) {
  (void) arr;
  (void) i;
  // TODO (carr): a constant index
  // TODO (sarr): an index that is not a constant
  throw Unsupported ("TODO: read");
}

// arr[i] = v.
void Engine::write (Value &arr, const z3::expr &i, const z3::expr &v) {
  (void) arr;
  (void) i;
  (void) v;
  // TODO (carr): a constant index
  // TODO (sarr): an index that is not a constant
  throw Unsupported ("TODO: write");
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
std::vector<State> Engine::merge (std::vector<State> states) {
  return states;
}
