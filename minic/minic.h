// MiniC: the language used in Lab 2.
//
//     auto prog = minic::load("tasks/public/p02_warmup_xor.mc");   // parse
//     + check minic::Inputs in = {{"x", {1}}, {"y", {2}}}; minic::Outcome
//     out = minic::run_input(*prog, in);              // run it if
//     (out.is_failure()) ...
//
// The interpreter is the definition of the language's semantics: an input
// is a counterexample only if run_input says the program fails on it.
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ast.h"

namespace minic {

// A static error: lexing, parsing or type checking. what() is "line N:
// message".
struct MiniCError : std::runtime_error {
  int line;
  MiniCError (int line, const std::string &msg);
};

// An input that does not fit the parameters of `check`.
struct InputError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// An internal invariant of the interpreter was violated. Never a valid
// failure.
struct InterpreterBug : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Parse and type-check a program. Throws MiniCError.
std::unique_ptr<Program> load (const std::string &path);
std::unique_ptr<Program> load_source (const std::string &src);

// Values for the parameters of `check`: name -> values. A scalar has
// exactly one value, an array has one value per element. int values are
// -2^31 .. 2^31-1; bool values are 0 (false) or 1 (true).
using Inputs = std::map<std::string, std::vector<int64_t>>;

struct Outcome {
  enum Kind { Ok, AssertFail, Trap, Blocked };
  Kind kind = Ok;
  int line = 0; // where the run stopped (0 for Ok)
  std::string detail;

  bool is_failure () const { return kind == AssertFail || kind == Trap; }
  std::string str () const; // e.g. "trap at line 4: division by zero"
};

void validate (const Program &prog, const Inputs &in); // throws InputError
Outcome run_input (const Program &prog,
                   const Inputs &in);                // validate, then run
Outcome run (const Program &prog, const Inputs &in); // `in` must be valid

// Inputs as a JSON object, in the format of the answer protocol.
std::string to_json (const Program &prog, const Inputs &in);
// Parse a JSON object strictly and validate it. Throws InputError.
Inputs parse_inputs (const Program &prog, const std::string &json);

// Reduce an integer to the range of int, wrapping around like 32-bit two's
// complement.
int64_t wrap (int64_t v);

} // namespace minic
