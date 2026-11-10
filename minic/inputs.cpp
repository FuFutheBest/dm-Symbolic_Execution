// Inputs of `check`: validation, a strict JSON reader, and JSON output.
//
// Error messages never echo text chosen by the solver: grade reports may be
// shown to students, and echoed text would be a channel for leaking hidden
// tasks.
#include <unordered_set>

#include "json.h"

namespace minic {

namespace {

const int64_t INT_LO = -(int64_t (1) << 31);
const int64_t INT_HI = (int64_t (1) << 31) - 1;
const std::string INT_RANGE =
    " [" + std::to_string (INT_LO) + ", " + std::to_string (INT_HI) + "]";

void check_scalar (int64_t v, Type ty, const std::string &where) {
  if (ty == Type::Bool) {
    if (v != 0 && v != 1)
      throw InputError (where + ": expected true/false");
    return;
  }
  if (v < INT_LO || v > INT_HI)
    throw InputError (where + ": out of range for int" + INT_RANGE);
}

const char *json_type_name (const JValue &v) {
  switch (v.kind) {
  case JValue::Null:
    return "NoneType";
  case JValue::Bool:
    return "bool";
  case JValue::Int:
    return "int";
  case JValue::String:
    return "str";
  case JValue::Array:
    return "list";
  default:
    return "dict";
  }
}

int64_t json_scalar (const JValue &v, Type ty, const std::string &where) {
  if (ty == Type::Bool) {
    if (v.kind != JValue::Bool)
      throw InputError (where + ": expected true/false");
    return v.b;
  }
  if (v.kind != JValue::Int)
    throw InputError (where + ": expected an integer, got " +
                      json_type_name (v));
  if (v.big || v.i < INT_LO || v.i > INT_HI)
    throw InputError (where + ": out of range for int" + INT_RANGE);
  return v.i;
}

} // namespace

void validate (const Program &prog, const Inputs &in) {
  const Func &entry = *prog.entry;
  size_t extra = in.size ();
  std::string missing;
  for (const auto &p : entry.params) {
    if (in.count (p.name))
      extra--;
    else
      missing += (missing.empty () ? "" : ", ") + p.name;
  }
  if (extra)
    throw InputError (std::to_string (extra) +
                      " unknown parameter name(s)");
  if (!missing.empty ())
    throw InputError ("missing parameter(s): " + missing);
  for (const auto &p : entry.params) {
    const auto &v = in.at (p.name);
    if (!p.is_array) {
      if (v.size () != 1)
        throw InputError (p.name + ": a scalar needs exactly one value");
      check_scalar (v[0], p.ty, p.name);
    } else {
      if (int64_t (v.size ()) != p.size)
        throw InputError (p.name + ": expected a list of exactly " +
                          std::to_string (p.size) + " values");
      for (size_t k = 0; k < v.size (); k++)
        check_scalar (v[k], p.ty, p.name + "[" + std::to_string (k) + "]");
    }
  }
}

Inputs validate_json (const Program &prog, const JValue &obj) {
  if (obj.kind != JValue::Object)
    throw InputError (
        "input must be a JSON object mapping parameter names to values");
  const Func &entry = *prog.entry;
  size_t extra = 0;
  for (const auto &kv : obj.obj) {
    bool known = false;
    for (const auto &p : entry.params)
      known = known || p.name == kv.first;
    extra += !known;
  }
  std::string missing;
  for (const auto &p : entry.params)
    if (!obj.get (p.name))
      missing += (missing.empty () ? "" : ", ") + p.name;
  if (extra)
    throw InputError (std::to_string (extra) +
                      " unknown parameter name(s)");
  if (!missing.empty ())
    throw InputError ("missing parameter(s): " + missing);
  Inputs out;
  for (const auto &p : entry.params) {
    const JValue &v = *obj.get (p.name);
    if (!p.is_array) {
      out[p.name] = std::vector<int64_t> (1, json_scalar (v, p.ty, p.name));
    } else {
      if (v.kind != JValue::Array || int64_t (v.arr.size ()) != p.size)
        throw InputError (p.name + ": expected a list of exactly " +
                          std::to_string (p.size) + " values");
      std::vector<int64_t> elems;
      for (size_t k = 0; k < v.arr.size (); k++)
        elems.push_back (json_scalar (
            v.arr[k], p.ty, p.name + "[" + std::to_string (k) + "]"));
      out[p.name] = std::move (elems);
    }
  }
  return out;
}

Inputs parse_inputs (const Program &prog, const std::string &json) {
  return validate_json (prog, parse_json_strict (json));
}

std::string to_json (const Program &prog, const Inputs &in) {
  auto scalar = [] (int64_t v, Type ty) {
    return ty == Type::Bool ? std::string (v ? "true" : "false")
                            : std::to_string (v);
  };
  std::string s = "{";
  bool first = true;
  for (const auto &p : prog.entry->params) {
    auto it = in.find (p.name);
    if (it == in.end ())
      continue;
    s += first ? "\"" : ", \"";
    first = false;
    s += p.name + "\": ";
    if (!p.is_array) {
      s += it->second.empty () ? "null" : scalar (it->second[0], p.ty);
    } else {
      s += "[";
      for (size_t k = 0; k < it->second.size (); k++)
        s += (k ? ", " : "") + scalar (it->second[k], p.ty);
      s += "]";
    }
  }
  return s + "}";
}

// ------------------------------------------------------------- strict JSON

const JValue *JValue::get (const std::string &key) const {
  for (const auto &kv : obj)
    if (kv.first == key)
      return &kv.second;
  return nullptr;
}

namespace {

const size_t MAX_JSON_BYTES = 1 << 20;
const int MAX_JSON_DEPTH =
    1000; // deeper input is rejected (Python allows more)

[[noreturn]] void bad_json () {
  throw InputError ("invalid JSON (JSONDecodeError)");
}

// Strict UTF-8, as Python's decoder: no overlong forms, surrogates or
// values > U+10FFFF.
bool valid_utf8 (const std::string &s) {
  size_t i = 0, n = s.size ();
  while (i < n) {
    unsigned char c = s[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    int len;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1F;
      min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0F;
      min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07;
      min = 0x10000;
    } else
      return false;
    if (i + len > n)
      return false;
    for (int k = 1; k < len; k++) {
      unsigned char d = s[i + k];
      if ((d & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += len;
  }
  return true;
}

void append_utf8 (std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += char (cp);
  } else if (cp < 0x800) {
    out += char (0xC0 | (cp >> 6));
    out += char (0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char (0xE0 | (cp >> 12));
    out += char (0x80 | ((cp >> 6) & 0x3F));
    out += char (0x80 | (cp & 0x3F));
  } else {
    out += char (0xF0 | (cp >> 18));
    out += char (0x80 | ((cp >> 12) & 0x3F));
    out += char (0x80 | ((cp >> 6) & 0x3F));
    out += char (0x80 | (cp & 0x3F));
  }
}

// Python's json module, plus: no duplicate keys, no NaN/Infinity, integers
// only.
class JsonReader {
public:
  explicit JsonReader (const std::string &s) : s_ (s) {}

  JValue document () {
    ws ();
    JValue v = value (0);
    ws ();
    if (i_ != s_.size ())
      bad_json ();
    return v;
  }

private:
  const std::string &s_;
  size_t i_ = 0;

  bool at_end () const { return i_ >= s_.size (); }
  char cur () const { return at_end () ? '\0' : s_[i_]; }
  bool starts (const char *lit) const {
    return s_.compare (i_, strlen (lit), lit) == 0;
  }
  void ws () {
    while (!at_end () && (s_[i_] == ' ' || s_[i_] == '\t' ||
                          s_[i_] == '\n' || s_[i_] == '\r'))
      i_++;
  }

  JValue value (int depth) {
    if (depth > MAX_JSON_DEPTH)
      throw InputError ("invalid JSON (RecursionError)");
    JValue v;
    char c = cur ();
    if (c == '{')
      return object (depth);
    if (c == '[')
      return array (depth);
    if (c == '"') {
      v.kind = JValue::String;
      v.s = string ();
      return v;
    }
    if (starts ("true")) {
      i_ += 4;
      v.kind = JValue::Bool;
      v.b = true;
      return v;
    }
    if (starts ("false")) {
      i_ += 5;
      v.kind = JValue::Bool;
      v.b = false;
      return v;
    }
    if (starts ("null")) {
      i_ += 4;
      v.kind = JValue::Null;
      return v;
    }
    if (starts ("NaN") || starts ("Infinity") || starts ("-Infinity"))
      throw InputError ("NaN/Infinity are not allowed");
    if (c == '-' || (c >= '0' && c <= '9'))
      return number ();
    bad_json ();
  }

  JValue number () {
    size_t start = i_;
    if (cur () == '-')
      i_++;
    if (cur () == '0') {
      i_++;
    } else if (cur () >= '1' && cur () <= '9') {
      while (cur () >= '0' && cur () <= '9')
        i_++;
    } else {
      bad_json ();
    }
    size_t int_end = i_;
    bool is_float = false;
    if (cur () == '.' && i_ + 1 < s_.size () && s_[i_ + 1] >= '0' &&
        s_[i_ + 1] <= '9') {
      is_float = true;
      i_++;
      while (cur () >= '0' && cur () <= '9')
        i_++;
    }
    if (cur () == 'e' || cur () == 'E') {
      size_t k = i_ + 1;
      if (k < s_.size () && (s_[k] == '+' || s_[k] == '-'))
        k++;
      if (k < s_.size () && s_[k] >= '0' && s_[k] <= '9') {
        is_float = true;
        i_ = k;
        while (cur () >= '0' && cur () <= '9')
          i_++;
      }
    }
    if (is_float)
      throw InputError ("non-integer number");
    JValue v;
    v.kind = JValue::Int;
    bool neg = s_[start] == '-';
    uint64_t mag = 0;
    for (size_t k = start + neg; k < int_end; k++) {
      int d = s_[k] - '0';
      if (mag > (uint64_t (INT64_MAX) + 1 - d) / 10) {
        v.big = true;
        break;
      }
      mag = mag * 10 + d;
    }
    if (!v.big) {
      if (!neg && mag > uint64_t (INT64_MAX))
        v.big = true;
      else
        v.i = neg ? int64_t (0 - mag) : int64_t (mag);
    }
    return v;
  }

  uint32_t hex4 () {
    if (i_ + 4 > s_.size ())
      bad_json ();
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
      char c = s_[i_ + k];
      int d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      else
        bad_json ();
      v = v * 16 + d;
    }
    i_ += 4;
    return v;
  }

  std::string string () {
    i_++; // opening quote
    std::string out;
    while (true) {
      if (at_end ())
        bad_json ();
      unsigned char c = s_[i_];
      if (c == '"') {
        i_++;
        return out;
      }
      if (c < 0x20)
        bad_json ();
      if (c != '\\') {
        out += char (c);
        i_++;
        continue;
      }
      i_++;
      if (at_end ())
        bad_json ();
      char e = s_[i_++];
      switch (e) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t cp = hex4 ();
        if (cp >= 0xD800 && cp <= 0xDBFF && starts ("\\u")) {
          size_t save = i_;
          i_ += 2;
          uint32_t lo = hex4 ();
          if (lo >= 0xDC00 && lo <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          else
            i_ = save; // a lone surrogate, kept as is
        }
        append_utf8 (out, cp);
        break;
      }
      default:
        bad_json ();
      }
    }
  }

  JValue object (int depth) {
    i_++;
    JValue v;
    v.kind = JValue::Object;
    std::unordered_set<std::string>
        keys; // untrusted input may have very many keys
    ws ();
    if (cur () == '}') {
      i_++;
      return v;
    }
    while (true) {
      if (cur () != '"')
        bad_json ();
      std::string key = string ();
      ws ();
      if (cur () != ':')
        bad_json ();
      i_++;
      ws ();
      JValue item = value (depth + 1);
      if (!keys.insert (key).second)
        throw InputError ("duplicate key in JSON object");
      v.obj.emplace_back (std::move (key), std::move (item));
      ws ();
      if (cur () == ',') {
        i_++;
        ws ();
        continue;
      }
      if (cur () == '}') {
        i_++;
        return v;
      }
      bad_json ();
    }
  }

  JValue array (int depth) {
    i_++;
    JValue v;
    v.kind = JValue::Array;
    ws ();
    if (cur () == ']') {
      i_++;
      return v;
    }
    while (true) {
      v.arr.push_back (value (depth + 1));
      ws ();
      if (cur () == ',') {
        i_++;
        ws ();
        continue;
      }
      if (cur () == ']') {
        i_++;
        return v;
      }
      bad_json ();
    }
  }
};

} // namespace

JValue parse_json_strict (const std::string &text) {
  if (text.size () > MAX_JSON_BYTES)
    throw InputError ("JSON too large");
  if (!valid_utf8 (text))
    throw InputError ("invalid UTF-8");
  return JsonReader (text).document ();
}

} // namespace minic
