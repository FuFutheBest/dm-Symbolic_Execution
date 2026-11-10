// Syntax tree of MiniC. The checker fills in `ty` on every expression.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minic {

// int is a 32-bit two's complement integer; arrays have int or bool
// elements.
enum class Type { Int, Bool, Void };

const char *type_name (Type t); // "int", "bool" or "void"

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// One expression node. Which fields are used depends on `kind`.
struct Expr {
  enum Kind { IntLit, BoolLit, Var, Index, Unary, Binary, Call };

  Kind kind;
  int line = 0;
  Type ty = Type::Void; // type of the value, set by the checker

  int64_t value = 0; // IntLit: the value, -2^31 .. 2^31-1 (a leading '-' is
                     //   folded into the literal); BoolLit: 0 or 1
  bool is_hex = false; // IntLit: written in hexadecimal
  std::string name;    // Var, Index (the array), Call (the function)
  std::string op;      // Unary: "-" "~" "!"; Binary: "+" "<<" "&&" "==" ...
  ExprPtr operand;     // Unary
  ExprPtr left, right; // Binary
  ExprPtr index;       // Index
  std::vector<ExprPtr> args; // Call
};

// One statement node. Which fields are used depends on `kind`.
struct Stmt {
  enum Kind {
    VarDecl,
    ArrayDecl,
    Assign,
    IndexAssign,
    If,
    For,
    Break,
    Continue,
    Return,
    Assert,
    Assume,
    ExprStmt,
    Block
  };

  Kind kind;
  int line = 0;
  Type ty = Type::Void; // VarDecl: variable type; ArrayDecl: element type
  std::string
      name; // VarDecl, ArrayDecl, Assign, IndexAssign: the variable;
            //   For: the loop variable
  int64_t size = 0;       // ArrayDecl: number of elements
  int64_t lo = 0, hi = 0; // For: the loop runs i = lo, lo+1, ..., hi-1
  ExprPtr value; // VarDecl: initial value; Assign, IndexAssign: new value;
                 //   Return: returned value (null for `return;`)
  ExprPtr index; // IndexAssign
  ExprPtr cond;  // If, Assert, Assume
  ExprPtr call;  // ExprStmt: always a Call expression
  StmtPtr then_block; // If
  StmtPtr
      else_block; // If: null when there is no else; `else if` is a Block
                  //   holding one If statement
  StmtPtr body;   // For: a Block
  std::vector<StmtPtr> stmts; // Block
};

struct Param {
  int line = 0;
  Type ty = Type::Void; // element type for arrays
  std::string name;
  bool is_array = false;
  int64_t size = 0; // number of elements when is_array
};

struct Func {
  int line = 0;
  Type ret = Type::Void;
  std::string name;
  std::vector<Param> params;
  StmtPtr body; // a Block
};

struct Program {
  std::vector<std::unique_ptr<Func>> funcs; // in source order
  const Func *entry = nullptr;              // the function `check`
  int64_t step_bound = 0; // static bound on interpreter steps

  const Func *
  find (const std::string &name) const; // nullptr if there is none
};

} // namespace minic
