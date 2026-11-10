// Declarations shared by the files of the minic library.
#pragma once

#include "minic.h"

namespace minic {

const char *const ENTRY = "check";

std::unique_ptr<Program> parse (const std::string &src); // parser.cpp
void check (Program &prog);                              // checker.cpp

} // namespace minic
