// Local-only test/benchmark entry point; not included in handin.zip.
#include "engine.h"
#include <iostream>
#include <random>

static void verify(const std::string &source, const EngineOptions &options,
                   const std::string &expected) {
  auto p = minic::load_source(source);
  Engine engine(*p, options);
  std::string result = engine.run();
  if (result != expected)
    throw std::runtime_error("expected " + expected + ", got " + result + "\n" +
                             source);
  if (result == "fail" &&
      !minic::run_input(*p, engine.counterexample()).is_failure())
    throw std::runtime_error("invalid counterexample\n" + source);
}

static void self_test(const EngineOptions &options) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"void check() { assert(2147483647 + 1 == -2147483648); "
       "assert(-(-2147483648) == -2147483648); assert(65536 * 65536 == 0); "
       "assert(-7 / 2 == -3); assert(-7 % 2 == -1); assert(7 % -2 == 1); "
       "assert((1 << 31) == -2147483648); assert((-1 >> 31) == -1); "
       "assert((-8 >> 1) == -4); assert(~0 == -1); assert(true != false); }",
       "safe"},
      {"void check() { int x = 1 / 0; }", "fail"},
      {"void check() { int x = -2147483648 % -1; }", "fail"},
      {"void check() { int x = -2147483648 / -1; }", "fail"},
      {"void check() { int x = 1 << 32; }", "fail"},
      {"void check() { int x = 1 >> -1; }", "fail"},
      {"void check() { assert(true || 1 / 0 == 0); "
       "assert(!(false && 1 / 0 == 0)); }",
       "safe"},
      {"void check() { assume(false); int a[1]; a[-1] = 1; }", "safe"},
      {"void check() { int a[1]; a[-1] = 1; }", "fail"},
      {"void check() { int a[1]; int x = a[1]; }", "fail"},
      {"void check(int x) { if (x > 0) { assume(x > 10); } "
       "assert(x <= 0 || x > 10); }",
       "safe"},
      {"void check(int x) { assert(x != 0); assume(x != 0); }", "fail"},
      {"void check(int x) { assume(x != 0); assert(x != 0); }", "safe"},
      {"void check(int x) { if (x == 0) { assert(false); } "
       "assume(false); }",
       "fail"},
      {"void check(int x) { int a[3]; assume(x >= 0 && x < 3); "
       "a[x] = 7; assert(a[x] == 7); }",
       "safe"},
      {"void check(int x) { bool a[3]; assume(x >= 0 && x < 3); "
       "a[x] = true; assert(a[x]); }",
       "safe"},
      {"int f(int a[2]) { a[0] = 9; return a[0]; } "
       "void check() { int a[2]; assert(f(a) == 9); assert(a[0] == 0); }",
       "safe"},
      {"int f(int x) { for i in 0..3 { if (x == i) { return i; } } "
       "return -1; } void check(int x) { assume(x >= 0 && x < 3); "
       "assert(f(x) == x); }",
       "safe"},
      {"void check() { int x = 0; for i in 0..5 { if (i == 1) { continue; } "
       "if (i == 3) { break; } x = x + 1; } assert(x == 2); }",
       "safe"},
      {"int stop() { assume(false); return 1; } "
       "void check() { int a[1]; a[99] = stop(); }",
       "safe"},
      {"void check(int x) { if (x > 0) { assert(x > 0); } "
       "else { assert(x <= 0); } assert(x != 0); }",
       "fail"},
  };
  for (const auto &[source, expected] : cases)
    verify(source, options, expected);

  // Differential constant-folding tests, including random 32-bit extremes.
  std::mt19937 random(20260719);
  const std::vector<std::string> ops = {
      "+",  "-",  "*", "&",  "|", "^",  "/",  "%",
      "<<", ">>", "<", "<=", ">", ">=", "==", "!="};
  for (unsigned k = 0; k < 120; ++k) {
    int64_t x = minic::wrap(static_cast<int64_t>(random()));
    int64_t y = minic::wrap(static_cast<int64_t>(random()));
    const auto &op = ops[k % ops.size()];
    if (op == "<<" || op == ">>")
      y = k % 35 - int64_t(1);
    bool comparison = k % ops.size() >= 10;
    std::string expression =
        "(" + std::to_string(x) + ") " + op + " (" + std::to_string(y) + ")";
    std::string expected_value;
    if (comparison) {
      auto probe =
          minic::load_source("void check() { assert(" + expression + "); }");
      expected_value =
          minic::run_input(*probe, {}).is_failure() ? "false" : "true";
    } else {
      // Recover the exact reference result by binary search, using only the
      // interpreter's public outcome API rather than duplicating arithmetic.
      auto probe =
          minic::load_source("void check(int limit) { int z = " + expression +
                             "; assert(z <= limit); }");
      auto outcome = minic::run_input(*probe, {{"limit", {2147483647}}});
      if (outcome.kind == minic::Outcome::Trap) {
        verify("void check() { int z = " + expression + "; }", options, "fail");
        continue;
      }
      int64_t lo = -2147483648LL, hi = 2147483647;
      while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (minic::run_input(*probe, {{"limit", {mid}}}).is_failure())
          lo = mid + 1;
        else
          hi = mid;
      }
      expected_value = std::to_string(lo);
    }
    std::string source =
        "void check() { " + std::string(comparison ? "bool" : "int") +
        " z = " + expression + "; assert(z == " + expected_value + "); }";
    verify(source, options, "safe");
  }

  // Exhaustively enumerate bounded inputs and compare against the interpreter.
  for (int threshold = -3; threshold <= 3; ++threshold) {
    std::string source =
        "void check(int x, int y) { assume(x >= -3 && x <= 3); "
        "assume(y >= -3 && y <= 3); int a[7]; a[x + 3] = y; "
        "int r = a[y + 3]; if (x < y) { r = r + 1; } "
        "assert(r <= " +
        std::to_string(threshold) + "); }";
    auto p = minic::load_source(source);
    bool failure = false;
    for (int x = -3; x <= 3; ++x)
      for (int y = -3; y <= 3; ++y)
        failure |= minic::run_input(*p, {{"x", {x}}, {"y", {y}}}).is_failure();
    verify(source, options, failure ? "fail" : "safe");
  }

  // Deterministic unknown: resource limits exercise the same result handling
  // as a wall-clock timeout, without depending on machine speed.
  for (bool batch : {false, true}) {
    EngineOptions limited = options;
    limited.batch_failures = batch;
    limited.prune_branches = true;
    limited.rlimit = 1;
    verify("void check(int x, int y) { if (x * y == 1234567) { "
           "assert(x == 0); } }",
           limited, "unknown");
  }
  std::cout << "PASS: " << cases.size() + 120 + 7 + 2 << " checks\n";
}

int main(int argc, char **argv) {
  EngineOptions options;
  std::string task;
  bool test = false;
  for (int k = 1; k < argc; ++k) {
    std::string arg = argv[k];
    if (arg == "--incremental")
      options.incremental = true;
    else if (arg == "--batch")
      options.batch_failures = true;
    else if (arg == "--fresh")
      options.incremental = false;
    else if (arg == "--no-fold")
      options.constant_folding = false;
    else if (arg == "--simplify")
      options.simplification = true;
    else if (arg == "--no-simplify")
      options.simplification = false;
    else if (arg == "--eager")
      options.batch_failures = false;
    else if (arg == "--prune")
      options.prune_branches = true;
    else if (arg == "--array")
      options.array_theory = true;
    else if (arg == "--unlimited") {
      options.timeout_ms = 0;
      options.solver_budget_ms = 0;
    } else if (arg == "--self-test")
      test = true;
    else if (arg.rfind("--", 0) == 0)
      return 2;
    else
      task = arg;
  }
  try {
    if (test) {
      self_test(options);
      return 0;
    }
    auto p = minic::load(task);
    Engine engine(*p, options);
    std::string verdict = engine.run();
    if (verdict == "fail") {
      auto input = engine.counterexample();
      if (!minic::run_input(*p, input).is_failure())
        throw std::runtime_error("invalid counterexample");
      std::cout << "{\"verdict\":\"fail\",\"input\":"
                << minic::to_json(*p, input) << "}\n";
    } else
      std::cout << "{\"verdict\":\"" << verdict << "\"}\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (test)
      return 1;
    std::cout << "{\"verdict\":\"unknown\"}\n";
  }
}
