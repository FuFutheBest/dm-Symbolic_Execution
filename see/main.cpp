// Entry point used by the grader:  ./build/see TASK.mc
//
// Print exactly one answer as the LAST line of stdout:
//     {"verdict": "fail", "input": {"x": 3, "buf": [1, 2, 3]}}
//     {"verdict": "safe"}
//     {"verdict": "unknown"}
// Debug output should go to stderr.
#include <iostream>

#include "engine.h"

static void answer_unknown (const std::string &why) {
  std::cerr << why << "\n";
  std::cout << "{\"verdict\": \"unknown\"}" << std::endl;
}

int main (int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " TASK.mc\n";
    return 2;
  }
  std::unique_ptr<minic::Program> prog;
  try {
    prog = minic::load (argv[1]);
  } catch (const minic::MiniCError &e) {
    std::cerr << "static error: " << e.what () << "\n";
    return 1;
  }
  Engine engine (*prog);
  std::string verdict;
  try {
    verdict = engine.run ();
  } catch (const Unsupported &e) {
    answer_unknown (std::string ("unsupported: ") + e.what ());
    return 0;
  } catch (const z3::exception &e) {
    answer_unknown (std::string ("z3 error: ") + e.msg ());
    return 0;
  }
  if (verdict == "unknown") {
    answer_unknown ("some query was undecided (e.g. a z3 timeout)");
    return 0;
  }
  if (verdict == "safe") {
    std::cout << "{\"verdict\": \"safe\"}" << std::endl;
    return 0;
  }
  // Check the counterexample on the interpreter before answering. If it
  // does not fail, the encoding disagrees with the semantics: that is a bug
  // to fix.
  minic::Inputs in = engine.counterexample ();
  try {
    minic::Outcome out = minic::run_input (*prog, in);
    if (!out.is_failure ()) {
      answer_unknown ("BUG: the model does not fail in the interpreter (" +
                      out.str () + ")");
      return 0;
    }
  } catch (const minic::InputError &e) {
    answer_unknown (std::string ("BUG: the model is not a valid input (") +
                    e.what () + ")");
    return 0;
  }
  std::cout << "{\"verdict\": \"fail\", \"input\": "
            << minic::to_json (*prog, in) << "}" << std::endl;
  return 0;
}
