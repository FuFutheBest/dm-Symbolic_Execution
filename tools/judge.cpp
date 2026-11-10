// Command-line entry to the scoring code, used by the course's own tests;
// not meant to be run by hand.
//
//     judge --check TASK.mc              exit 0 if the task loads
//     judge TASK.mc fail|safe < STDOUT   print
//     "status<TAB>points<TAB>detail"
#include <iostream>
#include <iterator>

#include "judge.h"

int main (int argc, char **argv) {
  try {
    if (argc == 3 && std::string (argv[1]) == "--check") {
      minic::load (argv[2]);
      return 0;
    }
    if (argc != 3 || (std::string (argv[2]) != "fail" &&
                      std::string (argv[2]) != "safe")) {
      std::cerr << "usage: judge TASK.mc fail|safe < STDOUT\n";
      return 2;
    }
    auto prog = minic::load (argv[1]);
    std::cin >> std::noskipws;
    std::string out ((std::istreambuf_iterator<char> (std::cin)),
                     std::istreambuf_iterator<char> ());
    minic::Judgement j = minic::judge (*prog, argv[2], out);
    std::cout << j.status << "\t" << j.points << "\t" << j.detail << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what () << "\n";
    return 1;
  }
}
