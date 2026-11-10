// Scoring of one solver answer. The course grader uses exactly this code
// (from its own trusted copy), so local and official scores agree.
#pragma once

#include "minic.h"

namespace minic {

const int POINTS_CORRECT = 1;
const int POINTS_WRONG_SAFE = -2;
const size_t MAX_OUTPUT_BYTES = 1 << 20;

struct Judgement {
  std::string
      status; // cex_ok safe_ok safe_wrong cex_invalid unknown bad_output
  int points = 0;
  std::string detail;
};

// `expected` is the ground truth, "fail" or "safe"; `stdout_bytes` is
// everything the solver printed on standard output.
Judgement judge (const Program &prog, const std::string &expected,
                 const std::string &stdout_bytes);

} // namespace minic
