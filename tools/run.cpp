// Run a MiniC program on one input:
//
//     ./build/run tasks/public/p02_warmup_xor.mc '{"x": 1, "y": 2}'
//     ./build/run tasks/public/p02_warmup_xor.mc input.json
//     ./build/run tasks/public/p02_warmup_xor.mc - < inputs.txt   (one JSON
//     input per line)
#include <fstream>
#include <iostream>
#include <sstream>

#include "minic.h"

int main (int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0]
              << " PROGRAM.mc '{\"x\": 1}'   (or a file holding the JSON "
                 "input)\n";
    return 2;
  }
  std::unique_ptr<minic::Program> prog;
  try {
    prog = minic::load (argv[1]);
  } catch (const minic::MiniCError &e) {
    std::cerr << "static error: " << e.what () << "\n";
    return 1;
  }
  std::string arg = argv[2];
  if (arg == "-") {
    std::string line;
    while (std::getline (std::cin, line)) {
      try {
        std::cout
            << minic::run (*prog, minic::parse_inputs (*prog, line)).str ()
            << "\n";
      } catch (const minic::InputError &e) {
        std::cout << "bad input: " << e.what () << "\n";
      }
    }
    return 0;
  }
  // Inline JSON starts with '{'; anything else names a file holding it.
  if (arg.find_first_not_of (" \t\r\n") != std::string::npos &&
      arg[arg.find_first_not_of (" \t\r\n")] != '{') {
    std::ifstream f (arg, std::ios::binary);
    if (!f) {
      std::cerr << "cannot open input file " << arg << "\n";
      return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf ();
    arg = ss.str ();
  }
  try {
    minic::Inputs in = minic::parse_inputs (*prog, arg);
    std::cout << minic::run (*prog, in).str () << "\n";
  } catch (const minic::InputError &e) {
    std::cerr << "bad input: " << e.what () << "\n";
    return 1;
  }
  return 0;
}
