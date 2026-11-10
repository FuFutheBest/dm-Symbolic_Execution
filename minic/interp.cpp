// Reference interpreter for MiniC. It is the definition of the language's
// semantics: a counterexample counts only if this interpreter says it
// fails.
#include <unordered_map>

#include "internal.h"

namespace minic {

int64_t wrap (int64_t v) {
  return int64_t (int32_t (uint32_t (uint64_t (v))));
}

std::string Outcome::str () const {
  static const char *const names[] = {"ok", "assert_fail", "trap",
                                      "blocked"};
  std::string s = names[kind];
  if (line)
    s += " at line " + std::to_string (line);
  if (!detail.empty ())
    s += ": " + detail;
  return s;
}

namespace {

const int64_t INT_MIN32 = -(int64_t (1) << 31);

struct Slot {
  int64_t value = 0;          // scalars
  std::vector<int64_t> elems; // arrays
};
using Frame = std::unordered_map<std::string, Slot>;

struct Stop {
  Outcome outcome;
};

enum class Flow { Normal, Break, Continue, Return };

class Interpreter {
public:
  explicit Interpreter (const Program &prog) : prog_ (prog) {}

  Outcome run (const Inputs &in) {
    const Func &entry = *prog_.entry;
    std::vector<Slot> args;
    for (const auto &p : entry.params) {
      const auto &v = in.at (p.name);
      Slot s;
      if (p.is_array)
        s.elems = v;
      else
        s.value = v.at (0);
      args.push_back (std::move (s));
    }
    try {
      call (entry, std::move (args));
    } catch (const Stop &s) {
      return s.outcome;
    }
    return Outcome{};
  }

private:
  const Program &prog_;
  int64_t steps_ = 0;
  int64_t ret_ = 0; // value of the last `return e`

  void tick () {
    if (++steps_ > prog_.step_bound)
      throw InterpreterBug ("static step bound exceeded");
  }

  [[noreturn]] void stop (Outcome::Kind kind, int line,
                          const std::string &detail) {
    throw Stop{Outcome{kind, line, detail}};
  }

  static Slot &slot (Frame &frame, const std::string &name) {
    auto it = frame.find (name);
    if (it == frame.end ())
      throw InterpreterBug ("unknown variable " + name);
    return it->second;
  }

  // ------------------------------------------------------------- functions
  int64_t call (const Func &f, std::vector<Slot> args) {
    tick ();
    Frame frame;
    for (size_t k = 0; k < f.params.size (); k++)
      frame[f.params[k].name] = std::move (args[k]);
    if (block (*f.body, frame) == Flow::Return)
      return ret_;
    if (f.ret != Type::Void)
      throw InterpreterBug ("function " + f.name + " fell off its end");
    return 0;
  }

  // ------------------------------------------------------------ statements
  Flow block (const Stmt &b, Frame &frame) {
    for (const auto &s : b.stmts) {
      Flow fl = stmt (*s, frame);
      if (fl != Flow::Normal)
        return fl;
    }
    return Flow::Normal;
  }

  Flow stmt (const Stmt &s, Frame &frame) {
    if (s.kind == Stmt::Block)
      return block (s, frame);
    tick ();
    switch (s.kind) {
    case Stmt::VarDecl:
      frame[s.name].value = expr (*s.value, frame);
      return Flow::Normal;
    case Stmt::ArrayDecl:
      frame[s.name].elems.assign (size_t (s.size), 0);
      return Flow::Normal;
    case Stmt::Assign: {
      int64_t v = expr (*s.value, frame);
      slot (frame, s.name).value = v;
      return Flow::Normal;
    }
    case Stmt::IndexAssign: {
      // index, then value, then the bounds check
      int64_t i = expr (*s.index, frame);
      int64_t v = expr (*s.value, frame);
      std::vector<int64_t> &arr = slot (frame, s.name).elems;
      bounds (s.line, i, int64_t (arr.size ()));
      arr[size_t (i)] = v;
      return Flow::Normal;
    }
    case Stmt::If:
      if (expr (*s.cond, frame))
        return block (*s.then_block, frame);
      if (s.else_block)
        return block (*s.else_block, frame);
      return Flow::Normal;
    case Stmt::For:
      for (int64_t i = s.lo; i < s.hi; i++) {
        tick ();
        frame[s.name].value = i;
        Flow fl = block (*s.body, frame);
        if (fl == Flow::Break)
          break;
        if (fl == Flow::Return)
          return fl;
      }
      return Flow::Normal;
    case Stmt::Break:
      return Flow::Break;
    case Stmt::Continue:
      return Flow::Continue;
    case Stmt::Return:
      ret_ = s.value ? expr (*s.value, frame) : 0;
      return Flow::Return;
    case Stmt::Assert:
      if (!expr (*s.cond, frame))
        stop (Outcome::AssertFail, s.line, "assertion violated");
      return Flow::Normal;
    case Stmt::Assume:
      if (!expr (*s.cond, frame))
        stop (Outcome::Blocked, s.line, "assumption violated");
      return Flow::Normal;
    case Stmt::ExprStmt:
      expr (*s.call, frame);
      return Flow::Normal;
    default:
      throw InterpreterBug ("unknown statement");
    }
  }

  void bounds (int line, int64_t i, int64_t n) {
    if (!(0 <= i && i < n))
      stop (Outcome::Trap, line,
            "index " + std::to_string (i) + " out of bounds [0, " +
                std::to_string (n) + ")");
  }

  // ----------------------------------------------------------- expressions
  int64_t expr (const Expr &e, Frame &frame) {
    switch (e.kind) {
    case Expr::IntLit:
      return e.value; // in range by the checker
    case Expr::BoolLit:
      return e.value;
    case Expr::Var:
      return slot (frame, e.name).value;
    case Expr::Index: {
      int64_t i = expr (*e.index, frame);
      const std::vector<int64_t> &arr = slot (frame, e.name).elems;
      bounds (e.line, i, int64_t (arr.size ()));
      return arr[size_t (i)];
    }
    case Expr::Unary: {
      int64_t v = expr (*e.operand, frame);
      uint64_t u = uint64_t (v);
      if (e.op == "!")
        return !v;
      if (e.op == "-")
        return wrap (int64_t (0 - u));
      if (e.op == "~")
        return wrap (int64_t (~u));
      throw InterpreterBug ("unknown unary operator " + e.op);
    }
    case Expr::Binary:
      return binary (e, frame);
    case Expr::Call: {
      const Func *f = prog_.find (e.name);
      if (!f)
        throw InterpreterBug ("unknown function " + e.name);
      std::vector<Slot> args;
      for (size_t k = 0; k < e.args.size (); k++) {
        Slot s;
        if (f->params[k].is_array)
          s.elems = slot (frame, e.args[k]->name).elems; // by value
        else
          s.value = expr (*e.args[k], frame);
        args.push_back (std::move (s));
      }
      return call (*f, std::move (args));
    }
    }
    throw InterpreterBug ("unknown expression");
  }

  int64_t binary (const Expr &e, Frame &frame) {
    const std::string &op = e.op;
    if (op == "&&")
      return expr (*e.left, frame) && expr (*e.right, frame);
    if (op == "||")
      return expr (*e.left, frame) || expr (*e.right, frame);
    int64_t a = expr (*e.left, frame);
    int64_t b = expr (*e.right, frame);
    if (op == "==")
      return a == b;
    if (op == "!=")
      return a != b;
    if (op == "<")
      return a < b;
    if (op == "<=")
      return a <= b;
    if (op == ">")
      return a > b;
    if (op == ">=")
      return a >= b;
    uint64_t ua = uint64_t (a),
             ub = uint64_t (b); // wrap-around arithmetic, no overflow UB
    if (op == "+")
      return wrap (int64_t (ua + ub));
    if (op == "-")
      return wrap (int64_t (ua - ub));
    if (op == "*")
      return wrap (int64_t (ua * ub));
    if (op == "&")
      return a & b;
    if (op == "|")
      return a | b;
    if (op == "^")
      return a ^ b;
    if (op == "/" || op == "%") {
      if (b == 0)
        stop (Outcome::Trap, e.line, "division by zero");
      if (a == INT_MIN32 && b == -1)
        stop (Outcome::Trap, e.line, "signed division overflow");
      // C++ division truncates toward zero, like MiniC
      return op == "/" ? a / b : a % b;
    }
    if (op == "<<" || op == ">>") {
      if (b < 0 || b > 31)
        stop (Outcome::Trap, e.line,
              "shift amount " + std::to_string (b) +
                  " out of range [0, 31]");
      if (op == "<<")
        return wrap (int64_t (ua << b));
      return a >> b; // arithmetic: a is signed
    }
    throw InterpreterBug ("unknown operator " + op);
  }
};

} // namespace

Outcome run (const Program &prog, const Inputs &in) {
  return Interpreter (prog).run (in);
}

Outcome run_input (const Program &prog, const Inputs &in) {
  validate (prog, in);
  return run (prog, in);
}

} // namespace minic
