// Lexer and recursive-descent parser for MiniC.
#include <cstdio>

#include "internal.h"

namespace minic {

MiniCError::MiniCError (int line, const std::string &msg)
    : std::runtime_error ("line " + std::to_string (line) + ": " + msg),
      line (line) {}

namespace {

const size_t MAX_SOURCE_BYTES = 1 << 20;
const size_t MAX_LITERAL_DIGITS = 40;
const int MAX_NESTING = 60; // parenthesis/unary/block nesting
// The checker rejects syntax trees deeper than this. A chain like a+a+a+...
// is rejected here already: a very deep tree would overflow the stack on
// destruction.
const int MAX_AST_DEPTH = 150;

// Literals larger than this are kept at this value; no type can hold them
// anyway.
const int64_t LITERAL_CAP = INT64_MAX;

const char *const KEYWORDS[] = {
    "int",    "bool",  "void",     "if",     "else",   "for",  "in",
    "return", "break", "continue", "assert", "assume", "true", "false",
};
// Longest first, so that "<<" wins over "<".
const char *const PUNCT[] = {
    "..", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "(",
    ")",  "{",  "}",  "[",  "]",  ",",  ";",  "=",  "+",  "-",
    "*",  "/",  "%",  "&",  "|",  "^",  "~",  "!",  "<",  ">",
};

enum class Tok { Int, Ident, Kw, Punct, Eof };

struct Token {
  Tok kind;
  std::string text;
  int64_t value = 0;   // Int
  bool is_hex = false; // Int
  int line;
};

bool is_type_keyword (const std::string &s) {
  return s == "int" || s == "bool";
}

Type type_of (const std::string &s) {
  if (s == "int")
    return Type::Int;
  if (s == "bool")
    return Type::Bool;
  return Type::Void;
}

bool is_digit (char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit (char c) {
  return is_digit (c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_ident_start (char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_ident_char (char c) { return is_ident_start (c) || is_digit (c); }
int hex_value (char c) {
  if (is_digit (c))
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

int64_t parse_number (const std::string &digits, int base) {
  int64_t v = 0;
  for (char c : digits) {
    int d = hex_value (c);
    if (v > (LITERAL_CAP - d) / base)
      return LITERAL_CAP;
    v = v * base + d;
  }
  return v;
}

std::string describe_char (unsigned char c) {
  char buf[16];
  if (c >= 0x20 && c < 0x7f)
    snprintf (buf, sizeof buf, "'%c'", c);
  else
    snprintf (buf, sizeof buf, "'\\x%02x'", c);
  return buf;
}

// Characters allowed unescaped in a char literal: printable ASCII except '
// and \.
bool plain_char (char c) {
  return c >= ' ' && c <= '~' && c != '\'' && c != '\\';
}

std::vector<Token> tokenize (const std::string &src) {
  if (src.size () > MAX_SOURCE_BYTES)
    throw MiniCError (1, "source file too large");
  std::vector<Token> toks;
  size_t pos = 0, n = src.size ();
  int line = 1;
  while (pos < n) {
    char c = src[pos];
    if (c == ' ' || c == '\t' || c == '\r') {
      pos++;
      continue;
    }
    if (c == '\n') {
      line++;
      pos++;
      continue;
    }
    if (src.compare (pos, 2, "//") == 0) {
      while (pos < n && src[pos] != '\n')
        pos++;
      continue;
    }
    if (src.compare (pos, 2, "/*") == 0) {
      size_t end = src.find ("*/", pos + 2);
      if (end != std::string::npos) {
        for (size_t k = pos; k < end; k++)
          line += src[k] == '\n';
        pos = end + 2;
        continue;
      }
      // An unterminated comment is not a comment; '/' is lexed as an
      // operator.
    }
    if (c == '0' && pos + 2 < n &&
        (src[pos + 1] == 'x' || src[pos + 1] == 'X') &&
        is_hex_digit (src[pos + 2])) {
      size_t end = pos + 2;
      while (end < n && is_hex_digit (src[end]))
        end++;
      std::string text = src.substr (pos, end - pos);
      if (text.size () > MAX_LITERAL_DIGITS)
        throw MiniCError (line, "integer literal too long");
      toks.push_back (
          {Tok::Int, text, parse_number (text.substr (2), 16), true, line});
      pos = end;
      continue;
    }
    if (is_digit (c)) {
      size_t end = pos;
      while (end < n && is_digit (src[end]))
        end++;
      std::string text = src.substr (pos, end - pos);
      if (text.size () > MAX_LITERAL_DIGITS)
        throw MiniCError (line, "integer literal too long");
      if (text.size () > 1 && text[0] == '0')
        throw MiniCError (line,
                          "decimal literal with leading zero: " + text);
      toks.push_back (
          {Tok::Int, text, parse_number (text, 10), false, line});
      pos = end;
      continue;
    }
    if (c == '\'') {
      // '\n' '\t' '\r' '\0' '\\' '\'' '\xNN' or one plain character
      size_t len = 0;
      int64_t v = -1;
      if (pos + 1 < n && src[pos + 1] == '\\') {
        if (pos + 3 < n && src[pos + 3] == '\'') {
          switch (src[pos + 2]) {
          case 'n':
            v = 10;
            break;
          case 't':
            v = 9;
            break;
          case 'r':
            v = 13;
            break;
          case '0':
            v = 0;
            break;
          case '\\':
            v = 92;
            break;
          case '\'':
            v = 39;
            break;
          }
          len = 4;
        }
        if (v < 0 && pos + 5 < n && src[pos + 2] == 'x' &&
            is_hex_digit (src[pos + 3]) && is_hex_digit (src[pos + 4]) &&
            src[pos + 5] == '\'') {
          v = hex_value (src[pos + 3]) * 16 + hex_value (src[pos + 4]);
          len = 6;
        }
      } else if (pos + 2 < n && plain_char (src[pos + 1]) &&
                 src[pos + 2] == '\'') {
        v = (unsigned char) src[pos + 1];
        len = 3;
      }
      if (v < 0)
        throw MiniCError (line,
                          "unexpected character " + describe_char (c));
      toks.push_back ({Tok::Int, src.substr (pos, len), v, false, line});
      pos += len;
      continue;
    }
    if (is_ident_start (c)) {
      size_t end = pos;
      while (end < n && is_ident_char (src[end]))
        end++;
      std::string text = src.substr (pos, end - pos);
      bool kw = false;
      for (const char *k : KEYWORDS)
        kw = kw || text == k;
      toks.push_back ({kw ? Tok::Kw : Tok::Ident, text, 0, false, line});
      pos = end;
      continue;
    }
    bool matched = false;
    for (const char *p : PUNCT) {
      std::string ps (p);
      if (src.compare (pos, ps.size (), ps) == 0) {
        toks.push_back ({Tok::Punct, ps, 0, false, line});
        pos += ps.size ();
        matched = true;
        break;
      }
    }
    if (!matched)
      throw MiniCError (line, "unexpected character " + describe_char (c));
  }
  toks.push_back ({Tok::Eof, "", 0, false, line});
  return toks;
}

// Binary operators by precedence level, lowest first (C precedence).
const std::vector<std::vector<std::string>> BINARY_LEVELS = {
    {"||"},
    {"&&"},
    {"|"},
    {"^"},
    {"&"},
    {"==", "!="},
    {"<", "<=", ">", ">="},
    {"<<", ">>"},
    {"+", "-"},
    {"*", "/", "%"},
};

class Parser {
public:
  explicit Parser (const std::string &src) : toks_ (tokenize (src)) {}

  std::unique_ptr<Program> program () {
    auto prog = std::make_unique<Program> ();
    while (tok ().kind != Tok::Eof) {
      auto f = func ();
      if (prog->find (f->name))
        throw MiniCError (f->line,
                          "function '" + f->name + "' defined twice");
      prog->funcs.push_back (std::move (f));
    }
    return prog;
  }

private:
  std::vector<Token> toks_;
  size_t i_ = 0;
  int depth_ = 0;

  const Token &tok () const { return toks_[i_]; }
  const Token &peek (size_t k = 1) const {
    return toks_[std::min (i_ + k, toks_.size () - 1)];
  }
  std::string shown () const {
    return tok ().text.empty () ? "end of file" : tok ().text;
  }

  bool at (const std::string &text) const {
    return (tok ().kind == Tok::Punct || tok ().kind == Tok::Kw) &&
           tok ().text == text;
  }
  bool accept (const std::string &text) {
    if (!at (text))
      return false;
    i_++;
    return true;
  }
  void expect (const std::string &text) {
    if (!at (text))
      throw MiniCError (tok ().line, "expected '" + text + "', found '" +
                                         shown () + "'");
    i_++;
  }
  std::string ident () {
    if (tok ().kind != Tok::Ident)
      throw MiniCError (tok ().line,
                        "expected identifier, found '" + shown () + "'");
    return toks_[i_++].text;
  }
  int64_t int_literal () {
    const Token &t = tok ();
    bool decimal = t.kind == Tok::Int && !t.text.empty ();
    for (char c : t.text)
      decimal = decimal && is_digit (c);
    if (!decimal)
      throw MiniCError (t.line, "expected a decimal integer literal");
    i_++;
    return t.value;
  }
  void nest () {
    if (++depth_ > MAX_NESTING)
      throw MiniCError (tok ().line, "nesting too deep");
  }
  void unnest () { depth_--; }

  Type scalar_type () {
    if (tok ().kind == Tok::Kw && is_type_keyword (tok ().text))
      return type_of (toks_[i_++].text);
    throw MiniCError (tok ().line,
                      "expected a type, found '" + tok ().text + "'");
  }

  std::unique_ptr<Func> func () {
    auto f = std::make_unique<Func> ();
    f->line = tok ().line;
    if (tok ().kind == Tok::Kw &&
        (tok ().text == "void" || is_type_keyword (tok ().text))) {
      f->ret = tok ().text == "void" ? Type::Void : type_of (tok ().text);
      i_++;
    } else {
      throw MiniCError (f->line, "expected a function definition, found '" +
                                     tok ().text + "'");
    }
    f->name = ident ();
    expect ("(");
    if (!at (")")) {
      while (true) {
        f->params.push_back (param ());
        if (!accept (","))
          break;
      }
    }
    expect (")");
    f->body = block ();
    return f;
  }

  Param param () {
    Param p;
    p.line = tok ().line;
    p.ty = scalar_type ();
    p.name = ident ();
    if (accept ("[")) {
      p.is_array = true;
      p.size = int_literal ();
      expect ("]");
    }
    return p;
  }

  StmtPtr make (Stmt::Kind kind, int line) {
    auto s = std::make_unique<Stmt> ();
    s->kind = kind;
    s->line = line;
    return s;
  }

  StmtPtr block () {
    auto b = make (Stmt::Block, tok ().line);
    expect ("{");
    nest ();
    while (!at ("}")) {
      if (tok ().kind == Tok::Eof)
        throw MiniCError (tok ().line, "unterminated block");
      b->stmts.push_back (stmt ());
    }
    expect ("}");
    unnest ();
    return b;
  }

  StmtPtr stmt () {
    const Token &t = tok ();
    int line = t.line;
    if (t.kind == Tok::Kw && is_type_keyword (t.text)) {
      Type ty = scalar_type ();
      std::string name = ident ();
      if (accept ("[")) {
        auto s = make (Stmt::ArrayDecl, line);
        s->ty = ty;
        s->name = name;
        s->size = int_literal ();
        expect ("]");
        expect (";");
        return s;
      }
      auto s = make (Stmt::VarDecl, line);
      s->ty = ty;
      s->name = name;
      expect ("=");
      s->value = expr ();
      expect (";");
      return s;
    }
    if (accept ("if"))
      return if_rest (line);
    if (accept ("for")) {
      auto s = make (Stmt::For, line);
      s->name = ident ();
      expect ("in");
      s->lo = int_literal ();
      expect ("..");
      s->hi = int_literal ();
      s->body = block ();
      return s;
    }
    if (accept ("break")) {
      expect (";");
      return make (Stmt::Break, line);
    }
    if (accept ("continue")) {
      expect (";");
      return make (Stmt::Continue, line);
    }
    if (accept ("return")) {
      auto s = make (Stmt::Return, line);
      if (!at (";"))
        s->value = expr ();
      expect (";");
      return s;
    }
    if (at ("assert") || at ("assume")) {
      auto s = make (tok ().text == "assert" ? Stmt::Assert : Stmt::Assume,
                     line);
      i_++;
      expect ("(");
      s->cond = expr ();
      expect (")");
      expect (";");
      return s;
    }
    if (at ("{"))
      return block ();
    if (t.kind == Tok::Ident) {
      if (peek ().text == "=") {
        auto s = make (Stmt::Assign, line);
        s->name = ident ();
        expect ("=");
        s->value = expr ();
        expect (";");
        return s;
      }
      if (peek ().text == "[") {
        auto s = make (Stmt::IndexAssign, line);
        s->name = ident ();
        expect ("[");
        s->index = expr ();
        expect ("]");
        if (accept ("=")) {
          s->value = expr ();
          expect (";");
          return s;
        }
        throw MiniCError (line,
                          "expression statement must be a function call");
      }
      if (peek ().text == "(") {
        auto s = make (Stmt::ExprStmt, line);
        s->call = expr ();
        expect (";");
        if (s->call->kind != Expr::Call)
          throw MiniCError (line,
                            "expression statement must be a function call");
        return s;
      }
    }
    throw MiniCError (line, "unexpected '" + shown () +
                                "' at start of statement");
  }

  StmtPtr if_rest (int line) {
    auto s = make (Stmt::If, line);
    expect ("(");
    s->cond = expr ();
    expect (")");
    s->then_block = block ();
    if (accept ("else")) {
      if (at ("if")) {
        int inner_line = tok ().line;
        i_++;
        nest ();
        auto els = make (Stmt::Block, inner_line);
        els->stmts.push_back (if_rest (inner_line));
        s->else_block = std::move (els);
        unnest ();
      } else {
        s->else_block = block ();
      }
    }
    return s;
  }

  ExprPtr make_expr (Expr::Kind kind, int line) {
    auto e = std::make_unique<Expr> ();
    e->kind = kind;
    e->line = line;
    return e;
  }

  bool at_binary (size_t level) const {
    if (tok ().kind != Tok::Punct)
      return false;
    for (const auto &op : BINARY_LEVELS[level])
      if (tok ().text == op)
        return true;
    return false;
  }

  ExprPtr expr (size_t level = 0) {
    if (level == BINARY_LEVELS.size ())
      return unary ();
    ExprPtr left = expr (level + 1);
    int chain = 0;
    while (at_binary (level)) {
      if (++chain > MAX_AST_DEPTH)
        throw MiniCError (tok ().line, "program is nested too deeply");
      auto e = make_expr (Expr::Binary, tok ().line);
      e->op = tok ().text;
      i_++;
      e->left = std::move (left);
      e->right = expr (level + 1);
      left = std::move (e);
    }
    return left;
  }

  ExprPtr unary () {
    const Token &t = tok ();
    int line = t.line;
    nest ();
    ExprPtr result;
    if (t.kind == Tok::Punct &&
        (t.text == "-" || t.text == "~" || t.text == "!")) {
      std::string op = t.text;
      i_++;
      if (op == "-" && tok ().kind == Tok::Int && tok ().value != 0) {
        result = make_expr (Expr::IntLit, line);
        result->value = -tok ().value;
        result->is_hex = tok ().is_hex;
        i_++;
      } else {
        result = make_expr (Expr::Unary, line);
        result->op = op;
        result->operand = unary ();
      }
    } else {
      result = primary ();
    }
    unnest ();
    return result;
  }

  ExprPtr primary () {
    const Token &t = tok ();
    int line = t.line;
    if (t.kind == Tok::Int) {
      auto e = make_expr (Expr::IntLit, line);
      e->value = t.value;
      e->is_hex = t.is_hex;
      i_++;
      return e;
    }
    if (accept ("true") || accept ("false")) {
      auto e = make_expr (Expr::BoolLit, line);
      e->value = toks_[i_ - 1].text == "true";
      return e;
    }
    if (accept ("(")) {
      ExprPtr e = expr ();
      expect (")");
      return e;
    }
    if (t.kind == Tok::Ident) {
      std::string name = ident ();
      if (accept ("(")) {
        auto e = make_expr (Expr::Call, line);
        e->name = name;
        if (!at (")")) {
          while (true) {
            e->args.push_back (expr ());
            if (!accept (","))
              break;
          }
        }
        expect (")");
        return e;
      }
      if (accept ("[")) {
        auto e = make_expr (Expr::Index, line);
        e->name = name;
        e->index = expr ();
        expect ("]");
        return e;
      }
      auto e = make_expr (Expr::Var, line);
      e->name = name;
      return e;
    }
    throw MiniCError (line, "unexpected '" + shown () + "' in expression");
  }
};

} // namespace

std::unique_ptr<Program> parse (const std::string &src) {
  return Parser (src).program ();
}

} // namespace minic
