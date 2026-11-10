#include "judge.h"

#include "json.h"

namespace minic {

namespace {

bool is_space (char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

// The answer is the last non-blank line of stdout, a JSON object.
JValue parse_answer (const std::string &out) {
  if (out.size () > MAX_OUTPUT_BYTES)
    throw InputError ("stdout too large");
  // Lines end at \n, \r or \r\n.
  std::string last;
  size_t i = 0, n = out.size ();
  while (i < n) {
    size_t j = i;
    while (j < n && out[j] != '\n' && out[j] != '\r')
      j++;
    std::string line = out.substr (i, j - i);
    bool blank = true;
    for (char c : line)
      blank = blank && is_space (c);
    if (!blank)
      last = line;
    if (j < n && out[j] == '\r' && j + 1 < n && out[j + 1] == '\n')
      j++;
    i = j + 1;
  }
  if (last.empty ())
    throw InputError ("no output");
  JValue ans = parse_json_strict (last);
  const JValue *verdict =
      ans.kind == JValue::Object ? ans.get ("verdict") : nullptr;
  if (!verdict || verdict->kind != JValue::String ||
      (verdict->s != "fail" && verdict->s != "safe" &&
       verdict->s != "unknown"))
    throw InputError ("last line must be a JSON object with \"verdict\" in "
                      "fail/safe/unknown");
  if (verdict->s == "fail" && !ans.get ("input"))
    throw InputError ("a \"fail\" verdict needs an \"input\" object");
  return ans;
}

} // namespace

Judgement judge (const Program &prog, const std::string &expected,
                 const std::string &stdout_bytes) {
  JValue ans;
  try {
    ans = parse_answer (stdout_bytes);
  } catch (const InputError &e) {
    return {"bad_output", 0, e.what ()};
  }
  const std::string &v = ans.get ("verdict")->s;
  if (v == "unknown")
    return {"unknown", 0, ""};
  if (v == "safe") {
    if (expected == "safe")
      return {"safe_ok", POINTS_CORRECT, ""};
    return {"safe_wrong", POINTS_WRONG_SAFE, ""};
  }
  Inputs in;
  try {
    in = validate_json (prog, *ans.get ("input"));
  } catch (const InputError &e) {
    return {"cex_invalid", 0, e.what ()};
  }
  Outcome out;
  try {
    out = run (prog, in);
  } catch (const InterpreterBug
               &e) { // never credit inputs that break the oracle
    return {"cex_invalid", 0,
            std::string ("ORACLE BUG, report to course staff: ") +
                e.what ()};
  } catch (const std::exception &e) {
    return {"cex_invalid", 0,
            std::string ("ORACLE ERROR, report to course staff: ") +
                e.what ()};
  }
  if (!out.is_failure ())
    return {"cex_invalid", 0, "program outcome: " + out.str ()};
  std::string detail = out.str ();
  if (expected == "safe") // the interpreter defines the semantics, so the
                          // ground truth was wrong
    detail = "GROUND TRUTH CONFLICT, report to course staff: " + detail;
  return {"cex_ok", POINTS_CORRECT, detail};
}

} // namespace minic
