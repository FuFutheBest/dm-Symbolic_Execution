// A symbolic executor for MiniC, built on z3.
//
// The work is split in two files:
//   engine.cpp  the framework: walks the syntax tree, evaluates expressions
//   in
//               the right order (including && and ||), passes
//               break/continue/ return on to the enclosing loop or
//               function, and cleans up variables that go out of scope. You
//               do not need to change it.
//   core.cpp    your part: the functions marked TODO, grouped in feature
//               blocks. See the handbook, section "实验内容".
//
// A construct that is not handled yet throws Unsupported, and the answer
// becomes "unknown". Keep it that way: never answer "safe" about code you
// did not handle.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <z3++.h>

#include "minic.h"

// Thrown for any construct the engine does not handle (yet).
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The value of a variable: one z3 expression per element. A scalar variable
// has exactly one element (store.at("x")[0]); an array of length N has N
// elements. int values are 32-bit bit-vectors, bool values are z3 booleans.
using Value = std::vector<z3::expr>;

// One symbolic state: where a path is, and what it knows.
struct State {
  z3::expr pc; // path condition: inputs reach this point iff pc holds
  std::map<std::string, Value> store; // variables in scope -> their values

  // How the last statement ended. The framework runs a statement only in
  // Normal states; a Break/Continue/Return state is passed on unchanged
  // until a loop (exec_for) or a function call (call) takes care of it.
  enum Flow { Normal, Break, Continue, Return } flow = Normal;
  std::optional<z3::expr>
      ret; // Return: the returned value (none for `return;`)

  explicit State (z3::context &ctx) : pc (ctx.bool_val (true)) {}
};

// Evaluating an expression can split a state: a function called in it may
// branch. So evaluation returns every resulting state together with the
// value on it.
struct Result {
  State state;
  z3::expr value;
};
using Results = std::vector<Result>;

class Engine {
public:
  explicit Engine (const minic::Program &prog);

  // Runs `check` symbolically. Returns "fail" if some input makes the
  // program fail, "safe" if none does, "unknown" if the engine could not
  // decide. Throws Unsupported when the program uses something not handled
  // yet.
  std::string run ();

  // After run() returned "fail": the failing input, read from the z3 model.
  minic::Inputs counterexample () const;

private:
  // ------------------------------------------------- framework
  // (engine.cpp)
  const minic::Program &prog_;
  z3::context ctx_;
  std::map<std::string, Value>
      inputs_;                     // parameter of check -> its z3 constants
  std::optional<z3::model> model_; // set by check() when a failure is found
  bool incomplete_ = false;        // set when some query was undecided

  // All states after running statement `s` on state `st`.
  std::vector<State> exec (const minic::Stmt &s, State st);
  // Value of expression `e` on state `st`, for every resulting state.
  Results eval (const minic::Expr &e, State st);
  // Checks the failure condition `fail` with check(), then continues the
  // state only for inputs that do not fail there. Returns false if no input
  // can continue (the condition always holds), so the state is dropped.
  bool guard (State &st, const z3::expr &fail);
  z3::expr zero (minic::Type ty); // 0 or false

  // --------------------------------------------------------- yours
  // (core.cpp) base: the two z3 queries
  bool feasible (const State &st);
  void check (const State &st, const z3::expr &fail);
  // if, loop, call: control structures
  std::vector<State> exec_if (const minic::Stmt &s, State st);
  std::vector<State> exec_for (const minic::Stmt &s, State st);
  Results call (const minic::Func &f, const std::vector<Value> &args,
                const State &caller);
  // ops: operators and their traps
  z3::expr binary (const std::string &op, const z3::expr &a,
                   const z3::expr &b);
  z3::expr div_failure (const z3::expr &a, const z3::expr &b);
  z3::expr shift_failure (const z3::expr &amount);
  // carr, sarr: arrays with a constant or a symbolic index
  z3::expr index_failure (const z3::expr &i, int64_t n);
  z3::expr read (const Value &arr, const z3::expr &i);
  void write (Value &arr, const z3::expr &i, const z3::expr &v);
  // merge: state merging
  std::vector<State> merge (std::vector<State> states);
};
