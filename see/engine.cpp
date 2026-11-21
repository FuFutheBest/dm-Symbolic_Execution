// The framework of the symbolic executor: everything that follows from the
// shape of the syntax tree. Your part is in core.cpp.
#include "engine.h"

using minic::Expr;
using minic::Stmt;
using minic::Type;

Engine::Engine(const minic::Program &prog, EngineOptions options)
    : prog_(prog), options_(options), solver_(ctx_) {}

// ----------------------------------------------------------------------
// API

std::string Engine::run() {
  // One z3 constant per input value. Array elements are named like
  // "key[0]", which no MiniC variable can be called, so names never clash.
  State init(ctx_);
  for (const auto &p : prog_.entry->params) {
    Value v;
    int64_t n = p.is_array ? p.size : 1;
    for (int64_t k = 0; k < n; k++) {
      std::string name =
          p.is_array ? p.name + "[" + std::to_string(k) + "]" : p.name;
      v.push_back(p.ty == Type::Int ? ctx_.bv_const(name.c_str(), 32)
                                    : ctx_.bool_const(name.c_str()));
    }
    inputs_.insert_or_assign(p.name, v);
    init.store.insert_or_assign(p.name, v);
  }
  exec(*prog_.entry->body, init);
  flush_failures();
  if (model_)
    return "fail";
  return incomplete_ ? "unknown" : "safe";
}

minic::Inputs Engine::counterexample() const {
  minic::Inputs in;
  for (const auto &[name, consts] : inputs_) {
    std::vector<int64_t> vals;
    for (const z3::expr &c : consts) {
      z3::expr v =
          model_->eval(c, true); // true: also give a value to unused inputs
      if (c.is_bool()) {
        vals.push_back(v.is_true() ? 1 : 0);
      } else {
        // get_numeral_uint64 gives the bit pattern; int is signed.
        int64_t n = int64_t(v.get_numeral_uint64());
        vals.push_back(n >= (int64_t(1) << 31) ? n - (int64_t(1) << 32) : n);
      }
    }
    in[name] = vals;
  }
  return in;
}

// ------------------------------------------------------------------
// helpers

bool Engine::guard(State &st, const z3::expr &fail) {
  // Mandatory trap simplification also protects concrete array accesses.
  z3::expr f = options_.simplification ? simplified(fail) : fail.simplify();
  if (f.is_false())
    return true; // cannot fail here: nothing to ask
  check(st, f);
  if (f.is_true())
    return false;                  // fails on every input that gets here
  st.pc = simplified(st.pc && !f); // Continue only with non-failing inputs.
  return !st.pc.is_false() && !model_;
}

z3::expr Engine::zero(Type ty) {
  return ty == Type::Int ? ctx_.bv_val(0, 32) : ctx_.bool_val(false);
}

// ---------------------------------------------------------------
// statements

std::vector<State> Engine::exec(const Stmt &s, State st) {
  if (model_ || st.pc.is_false())
    return {}; // A failure is found, or this path is blocked.
  if (st.flow != State::Normal)
    return {st}; // passed on until a loop or call handles it

  std::vector<State> out;
  switch (s.kind) {
  case Stmt::Block: {
    std::vector<State> states = {st};
    std::vector<std::string> declared;
    for (const auto &x : s.stmts) {
      if (x->kind == Stmt::VarDecl || x->kind == Stmt::ArrayDecl)
        declared.push_back(x->name);
      std::vector<State> next;
      for (State &y : states)
        for (State &z : exec(*x, y))
          next.push_back(z);
      states = next;
      if (states.empty())
        break;
    }
    // Variables declared in this block go out of scope.
    for (State &y : states)
      for (const std::string &name : declared)
        y.store.erase(name);
    return states;
  }
  case Stmt::VarDecl:
  case Stmt::Assign:
    for (Result &r : eval(*s.value, st)) {
      r.state.store.insert_or_assign(s.name, Value{r.value});
      out.push_back(r.state);
    }
    return out;
  case Stmt::ArrayDecl:
    st.store.insert_or_assign(s.name, Value(size_t(s.size), zero(s.ty)));
    return {st};
  case Stmt::IndexAssign:
    // a[i] = e: evaluate i, then e, and only then check the index.
    for (Result &ri : eval(*s.index, st))
      for (Result &rv : eval(*s.value, ri.state)) {
        State &y = rv.state;
        int64_t n = int64_t(y.store.at(s.name).size());
        if (!guard(y, index_failure(ri.value, n)))
          continue;
        write(y.store.at(s.name), ri.value, rv.value);
        out.push_back(y);
      }
    return out;
  case Stmt::If:
    return exec_if(s, st);
  case Stmt::For:
    return exec_for(s, st);
  case Stmt::Break:
    st.flow = State::Break;
    return {st};
  case Stmt::Continue:
    st.flow = State::Continue;
    return {st};
  case Stmt::Return:
    if (!s.value) {
      st.flow = State::Return;
      return {st};
    }
    for (Result &r : eval(*s.value, st)) {
      r.state.flow = State::Return;
      r.state.ret = r.value;
      out.push_back(r.state);
    }
    return out;
  case Stmt::Assert:
    for (Result &r : eval(*s.cond, st))
      if (guard(r.state, !r.value))
        out.push_back(r.state);
    return out;
  case Stmt::Assume:
    for (Result &r : eval(*s.cond, st)) {
      r.state.pc = simplified(r.state.pc && r.value);
      if (!r.state.pc.is_false())
        out.push_back(r.state);
    }
    return out;
  case Stmt::ExprStmt:
    for (Result &r : eval(*s.call, st))
      out.push_back(r.state);
    return out;
  }
  throw Unsupported("statement");
}

// --------------------------------------------------------------
// expressions

Results Engine::eval(const Expr &e, State st) {
  Results out;
  if (model_ || st.pc.is_false())
    return out;
  switch (e.kind) {
  case Expr::IntLit:
    return {{st, ctx_.bv_val(e.value, 32)}};
  case Expr::BoolLit:
    return {{st, ctx_.bool_val(e.value != 0)}};
  case Expr::Var: {
    z3::expr v = st.store.at(e.name)[0];
    return {{st, v}};
  }
  case Expr::Index:
    for (Result &r : eval(*e.index, st)) {
      int64_t n = int64_t(r.state.store.at(e.name).size());
      if (!guard(r.state, index_failure(r.value, n)))
        continue;
      z3::expr v = read(r.state.store.at(e.name), r.value);
      out.push_back({r.state, v});
    }
    return out;
  case Expr::Unary:
    for (Result &r : eval(*e.operand, st)) {
      z3::expr v = unary(e.op, r.value);
      out.push_back({r.state, v});
    }
    return out;
  case Expr::Binary:
    if (e.op == "&&" || e.op == "||") {
      // The right operand runs only when the left one did not decide the
      // result. Failures and calls in it happen only under that condition.
      bool is_and = e.op == "&&";
      for (Result &ra : eval(*e.left, st)) {
        z3::expr a = simplified(ra.value);
        z3::expr enter = simplified(is_and ? a : !a);
        if (enter.is_false()) {
          out.push_back({ra.state, ctx_.bool_val(!is_and)});
          continue;
        }
        State inner = ra.state;
        inner.pc = simplified(inner.pc && enter);
        Results rb = eval(*e.right, inner);
        if (rb.size() == 1) {
          // One state: keep it as one, valid whether or not the right side
          // ran.
          State y = rb[0].state;
          y.pc = z3::eq(y.pc, inner.pc) ? ra.state.pc
                                        : (y.pc || (ra.state.pc && !enter));
          out.push_back({y, simplified(is_and ? (a && rb[0].value)
                                              : (a || rb[0].value))});
        } else {
          // The right side split (or always failed): the skipped case is a
          // state of its own.
          State skip = ra.state;
          skip.pc = skip.pc && !enter;
          out.push_back({skip, ctx_.bool_val(!is_and)});
          for (Result &r : rb)
            out.push_back(r);
        }
      }
      return out;
    }
    // Left operand first, then the right one, then the operation itself.
    for (Result &ra : eval(*e.left, st))
      for (Result &rb : eval(*e.right, ra.state)) {
        State &y = rb.state;
        if (e.op == "/" || e.op == "%") {
          if (!guard(y, div_failure(ra.value, rb.value)))
            continue;
        } else if (e.op == "<<" || e.op == ">>") {
          if (!guard(y, shift_failure(rb.value)))
            continue;
        }
        out.push_back({y, simplified(binary(e.op, ra.value, rb.value))});
      }
    return out;
  case Expr::Call: {
    const minic::Func &f = *prog_.find(e.name);
    // Arguments from left to right; each one can split the states again.
    std::vector<std::pair<State, std::vector<Value>>> partial = {{st, {}}};
    for (size_t k = 0; k < e.args.size(); k++) {
      std::vector<std::pair<State, std::vector<Value>>> next;
      for (auto &[y, vals] : partial) {
        if (f.params[k].is_array) {
          // Arrays are passed by value: the callee gets a copy.
          std::vector<Value> more = vals;
          more.push_back(y.store.at(e.args[k]->name));
          next.push_back({y, more});
          continue;
        }
        for (Result &r : eval(*e.args[k], y)) {
          std::vector<Value> more = vals;
          more.push_back(Value{r.value});
          next.push_back({r.state, more});
        }
      }
      partial = next;
    }
    for (auto &[y, vals] : partial)
      for (Result &r : call(f, vals, y))
        out.push_back(r);
    return out;
  }
  }
  throw Unsupported("expression");
}
