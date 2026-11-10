// A strict JSON reader for answers and inputs (internal to minic).
#pragma once

#include <cstring>
#include <utility>

#include "internal.h"

namespace minic {

struct JValue {
  enum Kind { Null, Bool, Int, String, Array, Object };
  Kind kind = Null;
  bool b = false;
  int64_t i = 0;
  bool big = false; // an integer outside the int64 range
  std::string s;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue>> obj; // keys are unique

  const JValue *get (const std::string &key) const; // nullptr if absent
};

// Rejects invalid UTF-8, duplicate keys, NaN/Infinity and non-integer
// numbers. Throws InputError.
JValue parse_json_strict (const std::string &text);

// Check a JSON object against the parameters of `check`. Throws InputError.
Inputs validate_json (const Program &prog, const JValue &obj);

} // namespace minic
