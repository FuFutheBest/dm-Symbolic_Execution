// Static checks for MiniC: scoping, typing, literal ranges, control flow,
// recursion, and a static bound on the number of execution steps.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "internal.h"

namespace minic {

const char *type_name (Type t) {
  switch (t) {
  case Type::Int:
    return "int";
  case Type::Bool:
    return "bool";
  default:
    return "void";
  }
}

const Func *Program::find (const std::string &name) const {
  for (const auto &f : funcs)
    if (f->name == name)
      return f.get ();
  return nullptr;
}

namespace {

const int64_t MAX_ARRAY = 4096;
const int64_t MAX_LOOP_HI = 1000000;
const int64_t MAX_STEPS = 5000000;
const int MAX_AST_DEPTH = 150;
const size_t MAX_FUNCS = 64;
// A counterexample must fit in the answer (judge.h MAX_OUTPUT_BYTES, 1
// MiB): the entry's inputs, printed as JSON with every int at its longest,
// may take at most this many bytes.
const int64_t MAX_INPUT_JSON = 1000000;
const int MAX_CALL_DEPTH = 16;

// Step counts can be astronomically large before they are rejected;
// saturate.
const int64_t COST_CAP = int64_t (1) << 62;
int64_t sat_add (int64_t a, int64_t b) {
  return std::min (COST_CAP, a + b);
}
int64_t sat_mul (int64_t a, int64_t b) {
  if (a == 0 || b == 0)
    return 0;
  return a > COST_CAP / b ? COST_CAP : std::min (COST_CAP, a * b);
}

std::string str (Type t) { return type_name (t); }

void check_literal (const Expr &lit) {
  int64_t v = lit.value;
  if (v >= -(int64_t (1) << 31) && v <= (int64_t (1) << 31) - 1)
    return;
  char buf[64];
  if (lit.is_hex)
    snprintf (buf, sizeof buf, "%s0x%llx", v < 0 ? "-" : "",
              (unsigned long long) (v < 0 ? -(uint64_t) v : (uint64_t) v));
  else
    snprintf (buf, sizeof buf, "%lld", (long long) v);
  throw MiniCError (lit.line, std::string ("literal ") + buf +
                                  " does not fit in int");
}

struct VarInfo {
  Type ty;
  bool is_array;
  int64_t size;
  bool readonly;
};

// ------------------------------------------------------------------
// helpers

bool returns (const Stmt *s) {
  if (s->kind == Stmt::Return)
    return true;
  if (s->kind == Stmt::Block) {
    for (const auto &x : s->stmts)
      if (returns (x.get ()))
        return true;
    return false;
  }
  if (s->kind == Stmt::If)
    return s->else_block && returns (s->then_block.get ()) &&
           returns (s->else_block.get ());
  return false;
}

// A node of either kind, for walks over the whole tree.
struct Node {
  const Stmt *s = nullptr;
  const Expr *e = nullptr;
  int line () const { return s ? s->line : e->line; }
};

void children (const Node &n, std::vector<Node> &out) {
  auto add_e = [&] (const ExprPtr &e) {
    if (e)
      out.push_back ({nullptr, e.get ()});
  };
  auto add_s = [&] (const StmtPtr &s) {
    if (s)
      out.push_back ({s.get (), nullptr});
  };
  if (n.s) {
    const Stmt *s = n.s;
    switch (s->kind) {
    case Stmt::Block:
      for (const auto &x : s->stmts)
        add_s (x);
      break;
    case Stmt::If:
      add_e (s->cond);
      add_s (s->then_block);
      add_s (s->else_block);
      break;
    case Stmt::For:
      add_s (s->body);
      break;
    case Stmt::VarDecl:
    case Stmt::Assign:
    case Stmt::Return:
      add_e (s->value);
      break;
    case Stmt::IndexAssign:
      add_e (s->index);
      add_e (s->value);
      break;
    case Stmt::Assert:
    case Stmt::Assume:
      add_e (s->cond);
      break;
    case Stmt::ExprStmt:
      add_e (s->call);
      break;
    default:
      break;
    }
  } else {
    const Expr *e = n.e;
    switch (e->kind) {
    case Expr::Index:
      add_e (e->index);
      break;
    case Expr::Unary:
      add_e (e->operand);
      break;
    case Expr::Binary:
      add_e (e->left);
      add_e (e->right);
      break;
    case Expr::Call:
      for (const auto &a : e->args)
        add_e (a);
      break;
    default:
      break;
    }
  }
}

void check_depth (const Stmt *root) {
  std::vector<std::pair<Node, int>> stack = {{{root, nullptr}, 1}};
  std::vector<Node> kids;
  while (!stack.empty ()) {
    auto [n, d] = stack.back ();
    stack.pop_back ();
    if (d > MAX_AST_DEPTH)
      throw MiniCError (n.line (), "program is nested too deeply");
    kids.clear ();
    children (n, kids);
    for (const auto &k : kids)
      stack.push_back ({k, d + 1});
  }
}

// Upper bounds on interpreter steps; must match the interpreter's step
// counting.
int64_t calls_cost (Node n, const std::map<std::string, int64_t> &cost) {
  int64_t total = 0;
  std::vector<Node> stack = {n}, kids;
  while (!stack.empty ()) {
    Node x = stack.back ();
    stack.pop_back ();
    if (x.e && x.e->kind == Expr::Call)
      total = sat_add (total, sat_add (1, cost.at (x.e->name)));
    bool compound =
        x.s && (x.s->kind == Stmt::Block || x.s->kind == Stmt::If ||
                x.s->kind == Stmt::For);
    if (!compound) {
      kids.clear ();
      children (x, kids);
      stack.insert (stack.end (), kids.begin (), kids.end ());
    }
  }
  return total;
}

int64_t stmt_cost (const Stmt *s,
                   const std::map<std::string, int64_t> &cost);

int64_t block_cost (const Stmt *b,
                    const std::map<std::string, int64_t> &cost) {
  int64_t total = 0;
  for (const auto &s : b->stmts)
    total = sat_add (total, stmt_cost (s.get (), cost));
  return total;
}

int64_t stmt_cost (const Stmt *s,
                   const std::map<std::string, int64_t> &cost) {
  if (s->kind == Stmt::Block)
    return block_cost (s, cost);
  if (s->kind == Stmt::If) {
    int64_t then_cost = block_cost (s->then_block.get (), cost);
    int64_t else_cost =
        s->else_block ? block_cost (s->else_block.get (), cost) : 0;
    return sat_add (1,
                    sat_add (calls_cost ({nullptr, s->cond.get ()}, cost),
                             std::max (then_cost, else_cost)));
  }
  if (s->kind == Stmt::For)
    return sat_add (
        1, sat_mul (s->hi - s->lo,
                    sat_add (1, block_cost (s->body.get (), cost))));
  return sat_add (1, calls_cost ({s, nullptr}, cost));
}

// ------------------------------------------------------------------
// checker

class Checker {
public:
  explicit Checker (Program &prog) : prog_ (prog) {}

  void run () {
    const Func *entry = prog_.find (ENTRY);
    if (!entry)
      throw MiniCError (1,
                        std::string ("no entry function '") + ENTRY + "'");
    if (prog_.funcs.size () > MAX_FUNCS)
      throw MiniCError (1, "at most " + std::to_string (MAX_FUNCS) +
                               " functions are allowed");
    if (entry->ret != Type::Void)
      throw MiniCError (entry->line,
                        std::string ("'") + ENTRY + "' must return void");
    int64_t json = 2; // {}
    for (const auto &p :
         entry->params) // "name": value,  (an int is at most 11 characters)
      json += (int64_t) p.name.size () + 6 +
              (p.is_array ? 2 + 13 * p.size : 11);
    if (json > MAX_INPUT_JSON)
      throw MiniCError (entry->line,
                        std::string ("the inputs of '") + ENTRY +
                            "' are too large for a counterexample");
    for (const auto &f : prog_.funcs) {
      check_depth (f->body.get ());
      func (*f);
    }
    check_recursion ();
    prog_.entry = entry;
    std::map<std::string, int64_t> cost;
    std::map<std::string, int> depth;
    for (const std::string &name : topo_order ()) {
      const Func *f = prog_.find (name);
      cost[name] = block_cost (f->body.get (), cost);
      int d = 0;
      for (const auto &c : calls_[name])
        d = std::max (d, depth[c]);
      depth[name] = 1 + d;
      if (depth[name] > MAX_CALL_DEPTH)
        throw MiniCError (f->line, "call chains may be at most " +
                                       std::to_string (MAX_CALL_DEPTH) +
                                       " functions deep");
    }
    prog_.step_bound = sat_add (cost[ENTRY], 1);
    if (prog_.step_bound > MAX_STEPS)
      throw MiniCError (entry->line, "program may run more than " +
                                         std::to_string (MAX_STEPS) +
                                         " steps");
  }

private:
  Program &prog_;
  std::map<std::string, std::set<std::string>>
      calls_; // callees of each function
  const Func *cur_ = nullptr;
  std::vector<std::map<std::string, VarInfo>> scopes_;
  int loop_depth_ = 0;

  void check_recursion () {
    std::map<std::string, int> state; // 1 = active, 2 = done
    std::vector<std::string> stack;
    std::function<void (const std::string &)> visit =
        [&] (const std::string &name) {
          state[name] = 1;
          for (const auto &callee : calls_[name]) {
            if (state.count (callee) && state[callee] == 1) {
              std::string path;
              for (const auto &s : stack)
                path += s + " -> ";
              throw MiniCError (prog_.find (callee)->line,
                                "recursion is not allowed (" + path +
                                    callee + ")");
            }
            if (!state.count (callee)) {
              stack.push_back (callee);
              visit (callee);
              stack.pop_back ();
            }
          }
          state[name] = 2;
        };
    for (const auto &f : prog_.funcs) {
      if (!state.count (f->name)) {
        stack = {f->name};
        visit (f->name);
      }
    }
  }

  std::vector<std::string> topo_order () {
    std::vector<std::string> order;
    std::set<std::string> seen;
    std::function<void (const std::string &)> visit =
        [&] (const std::string &name) {
          if (seen.count (name))
            return;
          seen.insert (name);
          for (const auto &c : calls_[name])
            visit (c);
          order.push_back (name);
        };
    for (const auto &f : prog_.funcs)
      visit (f->name);
    return order;
  }

  // -------------------------------------------------------------- function
  void func (const Func &f) {
    cur_ = &f;
    calls_[f.name];
    scopes_ = {{}};
    loop_depth_ = 0;
    for (const auto &p : f.params) {
      if (p.is_array && !(1 <= p.size && p.size <= MAX_ARRAY))
        throw MiniCError (p.line, "array length must be between 1 and " +
                                      std::to_string (MAX_ARRAY));
      declare (p.line, p.name, {p.ty, p.is_array, p.size, false});
    }
    block (f.body.get (), false);
    if (f.ret != Type::Void && !returns (f.body.get ()))
      throw MiniCError (
          f.line, "function '" + f.name +
                      "' may reach its end without returning a value");
  }

  void declare (int line, const std::string &name, VarInfo info) {
    for (const auto &s : scopes_)
      if (s.count (name))
        throw MiniCError (
            line,
            "'" + name + "' is already declared in an enclosing scope");
    scopes_.back ()[name] = info;
  }

  const VarInfo &lookup (int line, const std::string &name) {
    for (auto it = scopes_.rbegin (); it != scopes_.rend (); ++it) {
      auto f = it->find (name);
      if (f != it->end ())
        return f->second;
    }
    throw MiniCError (line, "undeclared variable '" + name + "'");
  }

  const VarInfo &scalar (int line, const std::string &name) {
    const VarInfo &v = lookup (line, name);
    if (v.is_array)
      throw MiniCError (line, "'" + name +
                                  "' is an array; arrays can only be "
                                  "indexed or passed to functions");
    return v;
  }

  const VarInfo &array (int line, const std::string &name) {
    const VarInfo &v = lookup (line, name);
    if (!v.is_array)
      throw MiniCError (line, "'" + name + "' is not an array");
    return v;
  }

  // ------------------------------------------------------------ statements
  void block (const Stmt *b, bool new_scope = true) {
    if (new_scope)
      scopes_.emplace_back ();
    for (const auto &s : b->stmts)
      stmt (s.get ());
    if (new_scope)
      scopes_.pop_back ();
  }

  void stmt (const Stmt *s) {
    switch (s->kind) {
    case Stmt::VarDecl:
      expect (*s->value, s->ty);
      declare (s->line, s->name, {s->ty, false, 0, false});
      break;
    case Stmt::ArrayDecl:
      if (!(1 <= s->size && s->size <= MAX_ARRAY))
        throw MiniCError (s->line, "array length must be between 1 and " +
                                       std::to_string (MAX_ARRAY));
      declare (s->line, s->name, {s->ty, true, s->size, false});
      break;
    case Stmt::Assign: {
      VarInfo v = scalar (s->line, s->name);
      if (v.readonly)
        throw MiniCError (s->line, "loop variable '" + s->name +
                                       "' cannot be assigned");
      expect (*s->value, v.ty);
      break;
    }
    case Stmt::IndexAssign: {
      VarInfo v = array (s->line, s->name);
      index (*s->index);
      expect (*s->value, v.ty);
      break;
    }
    case Stmt::If:
      expect (*s->cond, Type::Bool);
      block (s->then_block.get ());
      if (s->else_block)
        block (s->else_block.get ());
      break;
    case Stmt::For:
      if (!(0 <= s->lo && s->lo <= s->hi && s->hi <= MAX_LOOP_HI))
        throw MiniCError (s->line,
                          "loop bounds must satisfy 0 <= lo <= hi <= " +
                              std::to_string (MAX_LOOP_HI));
      scopes_.emplace_back ();
      declare (s->line, s->name, {Type::Int, false, 0, true});
      loop_depth_++;
      block (s->body.get ());
      loop_depth_--;
      scopes_.pop_back ();
      break;
    case Stmt::Break:
    case Stmt::Continue:
      if (loop_depth_ == 0)
        throw MiniCError (s->line, "break/continue outside of a loop");
      break;
    case Stmt::Return:
      if (cur_->ret == Type::Void) {
        if (s->value)
          throw MiniCError (s->line, "void function cannot return a value");
      } else {
        if (!s->value)
          throw MiniCError (s->line,
                            "function must return a " + str (cur_->ret));
        expect (*s->value, cur_->ret);
      }
      break;
    case Stmt::Assert:
    case Stmt::Assume:
      expect (*s->cond, Type::Bool);
      break;
    case Stmt::ExprStmt:
      call (*s->call, true);
      break;
    case Stmt::Block:
      block (s);
      break;
    }
  }

  // ----------------------------------------------------------- expressions
  void expect (Expr &e, Type ty) {
    Type t = infer (e);
    if (t != ty)
      throw MiniCError (e.line,
                        "expected " + str (ty) + ", found " + str (t));
  }

  void index (Expr &e) {
    Type t = infer (e);
    if (t != Type::Int)
      throw MiniCError (e.line,
                        "array index must be an int, found " + str (t));
  }

  // Type-check `e`, record its type in e.ty and return it.
  Type infer (Expr &e) {
    switch (e.kind) {
    case Expr::IntLit:
      check_literal (e);
      e.ty = Type::Int;
      break;
    case Expr::BoolLit:
      e.ty = Type::Bool;
      break;
    case Expr::Var:
      e.ty = scalar (e.line, e.name).ty;
      break;
    case Expr::Index:
      e.ty = array (e.line, e.name).ty;
      index (*e.index);
      break;
    case Expr::Unary:
      if (e.op == "!") {
        expect (*e.operand, Type::Bool);
        e.ty = Type::Bool;
      } else {
        Type t = infer (*e.operand);
        if (t != Type::Int)
          throw MiniCError (e.line, "operator '" + e.op +
                                        "' needs an int operand, found " +
                                        str (t));
        e.ty = Type::Int;
      }
      break;
    case Expr::Binary:
      e.ty = binary (e);
      break;
    case Expr::Call:
      call (e, false);
      break;
    }
    return e.ty;
  }

  Type binary (Expr &e) {
    const std::string &op = e.op;
    if (op == "&&" || op == "||") {
      expect (*e.left, Type::Bool);
      expect (*e.right, Type::Bool);
      return Type::Bool;
    }
    Type lt = infer (*e.left);
    Type rt = infer (*e.right);
    if (lt != rt)
      throw MiniCError (e.line, "operands of '" + op +
                                    "' have different types (" + str (lt) +
                                    " and " + str (rt) + ")");
    if (op == "==" || op == "!=")
      return Type::Bool;
    if (lt != Type::Int)
      throw MiniCError (e.line, "operator '" + op +
                                    "' needs int operands, found " +
                                    str (lt));
    if (op == "<" || op == "<=" || op == ">" || op == ">=")
      return Type::Bool;
    return Type::Int;
  }

  Type call (Expr &e, bool allow_void) {
    const Func *f = prog_.find (e.name);
    if (!f)
      throw MiniCError (e.line, "unknown function '" + e.name + "'");
    if (f->name == ENTRY)
      throw MiniCError (e.line,
                        std::string ("'") + ENTRY + "' cannot be called");
    calls_[cur_->name].insert (f->name);
    if (e.args.size () != f->params.size ())
      throw MiniCError (e.line, "'" + f->name + "' takes " +
                                    std::to_string (f->params.size ()) +
                                    " arguments, got " +
                                    std::to_string (e.args.size ()));
    for (size_t k = 0; k < e.args.size (); k++) {
      Expr &a = *e.args[k];
      const Param &p = f->params[k];
      if (!p.is_array) {
        expect (a, p.ty);
      } else {
        if (a.kind != Expr::Var)
          throw MiniCError (a.line, "argument '" + p.name +
                                        "' must be an array variable");
        const VarInfo &v = array (a.line, a.name);
        if (v.ty != p.ty || v.size != p.size)
          throw MiniCError (
              a.line, "argument '" + p.name + "' must be " + str (p.ty) +
                          "[" + std::to_string (p.size) + "], found " +
                          str (v.ty) + "[" + std::to_string (v.size) + "]");
        a.ty = v.ty;
      }
    }
    if (f->ret == Type::Void && !allow_void)
      throw MiniCError (e.line,
                        "void function '" + f->name + "' used as a value");
    e.ty = f->ret;
    return f->ret;
  }
};

} // namespace

void check (Program &prog) { Checker (prog).run (); }

std::unique_ptr<Program> load_source (const std::string &src) {
  auto prog = parse (src);
  check (*prog);
  return prog;
}

std::unique_ptr<Program> load (const std::string &path) {
  std::ifstream f (path, std::ios::binary);
  if (!f)
    throw MiniCError (0, "cannot open " + path);
  std::stringstream ss;
  ss << f.rdbuf ();
  return load_source (ss.str ());
}

} // namespace minic
