// Run your solver on the tasks and score it like the course grader.
//
//     make grade                     all public tasks
//     make grade BLOCK=loop          the tasks of one block
//     ./build/grade tasks/public/p03*.mc              some tasks
//     ./build/grade -v -j 4 --timeout 10              more options (see
//     --help)
//
// Tasks are grouped in feature blocks (tasks/blocks.json). A block's score
// is its points times the fraction of its tasks solved (a wrong "safe"
// counts -2), and the total is the sum over blocks, capped at the full
// score.
//
// The solver runs as `./build/see TASK.mc` from this directory, one process
// per task, on a read-only copy of the task file, with the limits of the
// course grader: 60 s of wall-clock and CPU time, 2 GiB of memory, 1 MiB of
// standard output. Answers are scored by minic::judge (minic/judge.cpp),
// compiled into this program. The course grader runs the same command in a
// sandbox with its own copy of minic/ and tools/, so changes you make there
// do not count.
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "json.h"
#include "judge.h"

namespace fs = std::filesystem;

namespace {

const size_t OUTPUT_LIMIT =
    64u << 20; // bytes the solver may write to stdout/stderr files
const rlim_t MEMORY_LIMIT =
    rlim_t (2) << 30; // address space of the solver, as on the grader
const size_t STDERR_KEEP = 4096; // tail of the solver's stderr shown by -v

const std::map<std::string, std::string> STATUS = {
    {"cex_ok", "valid counterexample"},
    {"safe_ok", "correctly proved safe"},
    {"safe_wrong", "claimed safe, but the program can fail"},
    {"cex_invalid", "counterexample does not make the program fail"},
    {"unknown", "no answer"},
    {"bad_output", "output is not a valid answer"},
    {"timeout", "time limit exceeded"},
    {"crash", "solver exited abnormally"},
};

struct Block {
  std::string id, name, parent;
  int points = 0;
};

struct Task {
  std::string path, name, src, expected, block;
  std::unique_ptr<minic::Program> prog;
};

struct Result {
  minic::Judgement j;
  double elapsed = 0;
  std::string err;
};

[[noreturn]] void die (const std::string &msg) {
  std::cerr << "grade: " << msg << "\n";
  std::exit (2);
}

std::string read_file (const std::string &path) {
  std::ifstream f (path, std::ios::binary);
  if (!f)
    die ("cannot read " + path);
  std::stringstream ss;
  ss << f.rdbuf ();
  return ss.str ();
}

// The first `limit` + 1 bytes of a file (one more than the limit, so the
// caller can tell that it was exceeded).
std::string read_capped (const std::string &path, size_t limit) {
  std::ifstream f (path, std::ios::binary);
  std::string out (limit + 1, '\0');
  f.read (out.data (), (std::streamsize) out.size ());
  out.resize ((size_t) f.gcount ());
  return out;
}

// The last `keep` bytes of a file, prefixed with a marker when the file was
// longer.
std::string read_tail (const std::string &path, size_t keep) {
  std::ifstream f (path, std::ios::binary | std::ios::ate);
  if (!f)
    return "";
  auto size = (size_t) f.tellg ();
  size_t from = size > keep ? size - keep : 0;
  f.seekg ((std::streamoff) from);
  std::string out (size - from, '\0');
  f.read (out.data (), (std::streamsize) out.size ());
  out.resize ((size_t) f.gcount ());
  return (from ? "[... " + std::to_string (from) + " bytes skipped ...]\n"
               : "") +
         out;
}

minic::JValue load_json (const std::string &path) {
  try {
    return minic::parse_json_strict (read_file (path));
  } catch (const std::exception &e) {
    die (path + ": " + e.what ());
  }
}

const minic::JValue &field (const minic::JValue &obj,
                            const std::string &key,
                            const std::string &where) {
  const minic::JValue *v = obj.get (key);
  if (!v)
    die (where + ": missing \"" + key + "\"");
  return *v;
}

std::vector<std::string> split (const std::string &s) {
  std::vector<std::string> out;
  std::istringstream in (s);
  for (std::string w; in >> w;)
    out.push_back (w);
  return out;
}

// A private directory holding a read-only copy of the task and the solver's
// output files. Removed on destruction.
struct Workdir {
  fs::path dir;
  explicit Workdir (const std::string &src) {
    std::string tmpl =
        (fs::temp_directory_path () / "lab2-XXXXXX").string ();
    if (!mkdtemp (tmpl.data ()))
      die (std::string ("mkdtemp: ") + std::strerror (errno));
    dir = tmpl;
    std::ofstream (task ()) << src;
    chmod (task ().c_str (), 0444);
  }
  ~Workdir () {
    std::error_code ec;
    fs::remove_all (dir, ec);
  }
  std::string task () const { return (dir / "task.mc").string (); }
  std::string out () const { return (dir / "stdout").string (); }
  std::string err () const { return (dir / "stderr").string (); }
};

// Runs the solver on a private copy of the task. Returns the exit status
// (none on timeout), its standard output (capped), the elapsed time and the
// tail of its standard error.
struct Run {
  std::optional<std::string>
      fail; // "exit code N" / "signal N"; nullopt if ok or timed out
  bool timeout = false;
  std::string out, err;
  double elapsed = 0;
};

Run run_solver (const std::vector<std::string> &cmd, const std::string &src,
                double timeout) {
  Workdir w (src);
  std::vector<std::string> args = cmd;
  args.push_back (w.task ());
  std::vector<char *> argv;
  for (auto &a : args)
    argv.push_back (a.data ());
  argv.push_back (nullptr);

  int out_fd = open (w.out ().c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int err_fd = open (w.err ().c_str (), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int null_fd = open ("/dev/null", O_RDONLY);
  if (out_fd < 0 || err_fd < 0 || null_fd < 0)
    die ("cannot open the solver's output files");

  auto start = std::chrono::steady_clock::now ();
  pid_t pid = fork ();
  if (pid < 0)
    die (std::string ("fork: ") + std::strerror (errno));
  if (pid == 0) {
    // Own session and process group, so a timeout kills the solver's
    // children too. A descendant that calls setsid() itself escapes this;
    // the course grader's sandbox (cgroup) catches those.
    setsid ();
    rlimit fsize{OUTPUT_LIMIT, OUTPUT_LIMIT};
    setrlimit (RLIMIT_FSIZE,
               &fsize); // a flood of output cannot fill the disk
    rlimit mem{MEMORY_LIMIT, MEMORY_LIMIT};
    setrlimit (RLIMIT_AS, &mem);
    rlim_t cpu =
        (rlim_t) std::ceil (timeout) + 1; // CPU seconds, over all threads
    rlimit cpulim{cpu, cpu};
    setrlimit (RLIMIT_CPU, &cpulim);
    dup2 (null_fd, 0);
    dup2 (out_fd, 1);
    dup2 (err_fd, 2);
    execvp (argv[0], argv.data ());
    _exit (127);
  }
  close (out_fd);
  close (err_fd);
  close (null_fd);

  Run r;
  int status = 0;
  for (;;) {
    pid_t done = waitpid (pid, &status, WNOHANG);
    if (done == pid)
      break;
    std::chrono::duration<double> dt =
        std::chrono::steady_clock::now () - start;
    if (dt.count () >= timeout) {
      r.timeout = true;
      break;
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (5));
  }
  r.elapsed = std::chrono::duration<double> (
                  std::chrono::steady_clock::now () - start)
                  .count ();
  killpg (pid, SIGKILL); // stray children; harmless if nothing is left
  if (r.timeout)
    waitpid (pid, &status, 0);
  else if (WIFEXITED (status) && WEXITSTATUS (status) != 0)
    r.fail = "exit code " + std::to_string (WEXITSTATUS (status));
  else if (WIFSIGNALED (status) && WTERMSIG (status) == SIGXCPU)
    r.timeout =
        true; // used more CPU time than the limit (e.g. several threads)
  else if (WIFSIGNALED (status))
    r.fail = "signal " + std::to_string (WTERMSIG (status));
  r.out = read_capped (w.out (), minic::MAX_OUTPUT_BYTES);
  r.err = read_tail (w.err (), STDERR_KEEP);
  return r;
}

Result grade_one (const std::vector<std::string> &cmd, const Task &t,
                  double timeout) {
  Run r = run_solver (cmd, t.src, timeout);
  Result res;
  res.elapsed = r.elapsed;
  res.err = r.err;
  if (r.timeout) {
    res.j = {"timeout", 0, ""};
  } else if (r.fail) {
    res.j = {"crash", 0, *r.fail};
  } else {
    try {
      res.j = minic::judge (*t.prog, t.expected, r.out);
    } catch (const std::exception &) {
      res.j = {"bad_output", 0, "JUDGE ERROR, report to course staff"};
    }
  }
  return res;
}

std::string json_str (const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\')
      out += '\\', out += (char) c;
    else if (c < 0x20) {
      char buf[8];
      std::snprintf (buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else
      out += (char) c;
  }
  return out + "\"";
}

void usage (const char *prog) {
  std::cout
      << "usage: " << prog << " [options] [TASK.mc ...]\n"
      << "  (default: tasks/public/*.mc)\n"
      << "  --block ID         only the tasks of this block (see "
         "tasks/blocks.json)\n"
      << "  --expected FILE    ground truth (default: expected.json next "
         "to the tasks)\n"
      << "  --blocks FILE      block table (default: tasks/blocks.json)\n"
      << "  --timeout SEC      seconds per task (default 60), wall-clock "
         "and CPU\n"
      << "  -j N               tasks graded in parallel (default 1)\n"
      << "  --solver CMD       solver command (default ./build/see)\n"
      << "  --json FILE        also write the results as JSON\n"
      << "  -v                 show the solver's stderr on failures\n";
}

} // namespace

int main (int argc, char **argv) {
  std::vector<std::string> tasks;
  std::string expected_path, blocks_path = "tasks/blocks.json", block,
                             solver = "./build/see", json_path;
  double timeout = 60;
  int jobs = 1;
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&] () -> std::string {
      if (i + 1 >= argc)
        die (a + " needs a value");
      return argv[++i];
    };
    auto number = [&] (double lo) -> double {
      std::string v = value ();
      double x = 0;
      size_t used = 0;
      try {
        x = std::stod (v, &used);
      } catch (const std::exception &) {
        die (a + ": not a number: " + v);
      }
      if (used != v.size () || !std::isfinite (x) || x < lo)
        die (a + ": invalid value " + v);
      return x;
    };
    if (a == "-h" || a == "--help")
      return usage (argv[0]), 0;
    else if (a == "--block")
      block = value ();
    else if (a == "--expected")
      expected_path = value ();
    else if (a == "--blocks")
      blocks_path = value ();
    else if (a == "--timeout")
      timeout = number (0.001);
    else if (a == "-j" || a == "--jobs")
      jobs = (int) std::min (1024.0, number (1));
    else if (a == "--solver")
      solver = value ();
    else if (a == "--json")
      json_path = value ();
    else if (a == "-v" || a == "--verbose")
      verbose = true;
    else if (!a.empty () && a[0] == '-')
      die ("unknown option " + a + " (try --help)");
    else
      tasks.push_back (a);
  }
  if (tasks.empty ()) {
    for (auto &e : fs::directory_iterator ("tasks/public"))
      if (e.path ().extension () == ".mc")
        tasks.push_back (e.path ().string ());
    std::sort (tasks.begin (), tasks.end ());
    if (tasks.empty ())
      die ("no tasks in tasks/public");
  }
  if (expected_path.empty ())
    expected_path =
        (fs::path (tasks[0]).parent_path () / "expected.json").string ();

  // The block table and the ground truth.
  minic::JValue table = load_json (blocks_path);
  int full_score = (int) field (table, "full_score", blocks_path).i;
  std::vector<Block> blocks;
  for (const auto &b : field (table, "blocks", blocks_path).arr) {
    Block blk;
    blk.id = field (b, "id", blocks_path).s;
    blk.name = field (b, "name", blocks_path).s;
    blk.points = (int) field (b, "points", blocks_path).i;
    const minic::JValue *req = b.get ("requires");
    blk.parent = req && req->kind == minic::JValue::String ? req->s : "";
    if (blk.points < 0)
      die (blocks_path + ": block " + blk.id + " has negative points");
    for (const Block &other : blocks)
      if (other.id == blk.id)
        die (blocks_path + ": duplicate block " + blk.id);
    blocks.push_back (blk);
  }
  auto known_block = [&] (const std::string &id) {
    return std::any_of (blocks.begin (), blocks.end (),
                        [&] (const Block &b) { return b.id == id; });
  };
  for (const Block &b : blocks)
    if (!b.parent.empty () && !known_block (b.parent))
      die (blocks_path + ": block " + b.id + " requires unknown block " +
           b.parent);
  if (!block.empty () && !known_block (block))
    die ("unknown block " + block + "; see " + blocks_path);
  minic::JValue manifest = load_json (expected_path);
  // Every entry must be well formed: a typo here must stop the run, not
  // silently change a score. Also count the tasks of each block, so a
  // partial run is scored against the whole block.
  std::map<std::string, int> block_total;
  if (manifest.kind != minic::JValue::Object)
    die (expected_path + ": not a JSON object");
  for (const auto &[task_name, info] : manifest.obj) {
    const minic::JValue *exp = info.get ("expected");
    const minic::JValue *blk = info.get ("block");
    if (!exp || exp->kind != minic::JValue::String ||
        (exp->s != "safe" && exp->s != "fail"))
      die (expected_path + ": " + task_name +
           ": \"expected\" must be \"safe\" or \"fail\"");
    if (!blk || blk->kind != minic::JValue::String || !known_block (blk->s))
      die (expected_path + ": " + task_name + ": unknown block");
    block_total[blk->s]++;
  }

  std::vector<std::string> cmd = split (solver);
  if (cmd.empty ())
    die ("empty solver command");
  if (cmd[0].rfind ("./", 0) == 0 && !fs::exists (cmd[0]))
    die (cmd[0] + " not found; run make first");

  // Read and check every task before any solver runs: a solver must not be
  // able to change a task that is graded later.
  std::vector<Task> loaded;
  for (const std::string &path : tasks) {
    Task t;
    t.path = path;
    t.name = fs::path (path).filename ().string ();
    const minic::JValue *info = manifest.get (t.name);
    if (!info)
      die (t.name + " is not in " + expected_path);
    t.expected = field (*info, "expected", expected_path).s;
    t.block = field (*info, "block", expected_path).s;
    if (!block.empty () && t.block != block)
      continue;
    t.src = read_file (path);
    try {
      t.prog = minic::load_source (t.src);
    } catch (const std::exception &e) {
      die (t.name + ": " + e.what ());
    }
    loaded.push_back (std::move (t));
  }
  if (loaded.empty ())
    die ("no tasks of block " + block + " among the given tasks");

  // Grade `jobs` tasks at a time; report them in order as they finish.
  std::vector<std::optional<Result>> results (loaded.size ());
  std::atomic<size_t> next{0};
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::thread> workers;
  for (int w = 0; w < jobs; w++)
    workers.emplace_back ([&] {
      for (size_t i; (i = next++) < loaded.size ();) {
        Result r = grade_one (cmd, loaded[i], timeout);
        std::lock_guard<std::mutex> lock (mu);
        results[i] = std::move (r);
        cv.notify_all ();
      }
    });

  double total_time = 0;
  std::map<std::string, std::vector<int>> block_points;
  for (size_t i = 0; i < loaded.size (); i++) {
    std::unique_lock<std::mutex> lock (mu);
    cv.wait (lock, [&] { return results[i].has_value (); });
    const Result &r = *results[i];
    const Task &t = loaded[i];
    lock.unlock ();
    total_time += r.elapsed;
    block_points[t.block].push_back (r.j.points);
    char mark = r.j.points == 1 ? '+' : r.j.points == 0 ? ' ' : '-';
    std::cout << "[" << mark << "] " << std::left << std::setw (28)
              << t.name << " " << std::setw (5) << t.block << " "
              << std::showpos << r.j.points << std::noshowpos << "  "
              << std::right << std::fixed << std::setprecision (2)
              << std::setw (6) << r.elapsed << "s  "
              << STATUS.at (r.j.status);
    if (!r.j.detail.empty ())
      std::cout << " (" << r.j.detail << ")";
    std::cout << std::endl;
    if (verbose && r.j.points <= 0 && !r.err.empty ()) {
      std::string tail = r.err;
      while (!tail.empty () &&
             (tail.back () == '\n' || tail.back () == ' '))
        tail.pop_back ();
      std::cout << "    ";
      for (char c : tail)
        std::cout << c << (c == '\n' ? "    " : "");
      std::cout << std::endl;
    }
  }
  for (auto &w : workers)
    w.join ();

  std::cout << "\n";
  double total = 0;
  std::map<std::string, double> block_score;
  bool partial = false;
  for (const Block &b : blocks) {
    auto it = block_points.find (b.id);
    if (it == block_points.end ())
      continue;
    const std::vector<int> &pts = it->second;
    int got = 0, solved = 0;
    for (int p : pts)
      got += p, solved += p == 1;
    int n = block_total[b.id]; // the whole block, not only the tasks run
    double score = (double) b.points * got / (double) n;
    block_score[b.id] = score;
    total += score;
    std::cout << std::left << std::setw (6) << b.id << " " << std::right
              << std::setw (2) << solved << "/" << std::left
              << std::setw (2) << n << " tasks  " << std::right
              << std::fixed << std::setprecision (1) << std::setw (5)
              << score << " / " << std::left << std::setw (3) << b.points
              << "  " << b.name;
    if ((int) pts.size () < n) {
      std::cout << "  (" << pts.size () << " of " << n << " run)";
      partial = true;
    }
    std::cout << "\n";
  }
  total = std::min ((double) full_score, std::max (0.0, total));
  std::cout << "\nscore: " << std::fixed << std::setprecision (1) << total
            << " / " << full_score << "   total time: " << total_time
            << "s\n";
  if (partial)
    std::cout
        << "(only some tasks were run; tasks not run count as unsolved)\n";
  if (!block.empty ())
    std::cout << "(block " << block
              << " only; the other blocks are not shown)\n";

  if (!json_path.empty ()) {
    std::ofstream f (json_path);
    f << std::fixed << std::setprecision (3) << "{\"tasks\": {";
    for (size_t i = 0; i < loaded.size (); i++) {
      const Result &r = *results[i];
      f << (i ? ", " : "") << json_str (loaded[i].name)
        << ": {\"block\": " << json_str (loaded[i].block)
        << ", \"status\": " << json_str (r.j.status)
        << ", \"points\": " << r.j.points << ", \"elapsed\": " << r.elapsed
        << ", \"detail\": " << json_str (r.j.detail) << "}";
    }
    f << "}, \"blocks\": {";
    bool first = true;
    for (const auto &[id, score] : block_score)
      f << (first ? "" : ", ") << json_str (id) << ": " << score,
          first = false;
    f << "}, \"score\": " << total << "}\n";
    if (!f)
      die ("cannot write " + json_path);
  }
  return 0;
}
