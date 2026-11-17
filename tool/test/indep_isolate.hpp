// Independent tests of the tool: runs a piece of a test in a child process, so that a
// crash (Tela's exception reaching -fno-exceptions code ends in std::terminate) is
// reported as a failed check instead of ending the whole test executable.
#ifndef SND_TOOL_TEST_INDEP_ISOLATE_HPP
#define SND_TOOL_TEST_INDEP_ISOLATE_HPP

#include <cstdio>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#define INDEP_HAVE_FORK 1
#else
#define INDEP_HAVE_FORK 0
#endif

namespace indep {

inline constexpr int kNotIsolated = -1;  // no fork on this platform: nothing was run
inline constexpr int kSignalBase = 1000;  // result kSignalBase + signal: the child was killed

// fn() returns the child's exit code (0 = fine). The child ends with _exit (no atexit work).
template <class Fn>
int Isolated(Fn&& fn) {
#if INDEP_HAVE_FORK
  std::fflush(nullptr);
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int code = fn();
    std::fflush(nullptr);
    ::_exit(code);
  }
  if (pid < 0) return kNotIsolated;
  int st = 0;
  if (::waitpid(pid, &st, 0) != pid) return kNotIsolated;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return kSignalBase + WTERMSIG(st);
  return kNotIsolated;
#else
  (void)fn;
  return kNotIsolated;
#endif
}

}  // namespace indep

#endif  // SND_TOOL_TEST_INDEP_ISOLATE_HPP
