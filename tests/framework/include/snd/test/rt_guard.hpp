// RT-safety violation recorder for tests (design.md 14 "RT安全性").
//
// The runner installs Recorder as the snd::rt violation handler. Inside an
// RT scope (SND_RT_SCOPE / snd::rt::RtScope) every detected allocation, lock,
// file IO or noted violation is recorded. At the end of each test, violations
// that happened outside an ExpectViolations scope fail the test.
//
// Detection sources (see contract/internal/include/snd/internal/rt_check.hpp):
//   - operator new/delete: always in test executables (sonus_testkit_rthooks)
//   - malloc/calloc/realloc/free/aligned_alloc/posix_memalign,
//     pthread_mutex_lock/trylock, pthread_rwlock_rdlock/wrlock,
//     fopen/open/read/write: glibc builds without sanitizers (LibcHooksActive())
//   - SND_RT_NOTE(kind) placed in library code: always
//
// Set SND_RT_ABORT=1 in the environment to abort() at the first unexpected
// violation (to get a stack trace in a debugger).
#ifndef SND_TEST_RT_GUARD_HPP
#define SND_TEST_RT_GUARD_HPP

#include <cstdint>

#include "snd/internal/rt_check.hpp"

namespace snd::test::rt {

using snd::rt::Violation;

struct Record {
  Violation kind;
  const char* site;  // static string, may be null
  bool expected;
};

// Installs the handler (done by the runner before the first test).
void Install() noexcept;

// Counts since the last Reset (both expected and unexpected).
std::uint32_t Count(Violation kind) noexcept;
std::uint32_t Total() noexcept;
std::uint32_t UnexpectedTotal() noexcept;
// First records (up to 64) since the last Reset.
std::uint32_t Records(Record* out, std::uint32_t capacity) noexcept;
void Reset() noexcept;

// True if the libc interposition hooks are compiled into this executable.
bool LibcHooksActive() noexcept;
// True if operator new/delete hooks are linked.
bool NewHooksActive() noexcept;

// Violations inside this scope are recorded as expected (they do not fail the test).
class ExpectViolations {
 public:
  ExpectViolations() noexcept;
  ~ExpectViolations();
  ExpectViolations(const ExpectViolations&) = delete;
  ExpectViolations& operator=(const ExpectViolations&) = delete;
};

}  // namespace snd::test::rt

#endif  // SND_TEST_RT_GUARD_HPP
