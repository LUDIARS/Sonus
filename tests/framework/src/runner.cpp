#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

namespace snd::test {
namespace {

TestCase* gHead = nullptr;
TestCase* gTail = nullptr;
TestCase* gCurrent = nullptr;
bool gCurrentFailed = false;
ExpectFailureScope* gExpect = nullptr;
bool gUpdateGolden = false;

void PrintLine(FILE* f, const char* fmt, ...) noexcept __attribute__((format(printf, 2, 3)));
void PrintLine(FILE* f, const char* fmt, ...) noexcept {
  // The framework may report from inside an RT scope; its own IO is not a violation.
  snd::rt::SuspendScope suspend;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fflush(f);
}

// Truncating string append (keeps dst NUL-terminated).
void AppendBounded(char* dst, std::size_t cap, std::size_t* len, const char* src) noexcept {
  if (src == nullptr || cap == 0) return;
  while (*src != '\0' && *len + 1 < cap) dst[(*len)++] = *src++;
  dst[*len] = '\0';
}

bool Matches(const TestCase* tc, const char* filter) noexcept {
  if (filter == nullptr || filter[0] == '\0') return true;
  char full[512];
  std::snprintf(full, sizeof(full), "%s.%s", tc->suite, tc->name);
  return std::strstr(full, filter) != nullptr;
}

}  // namespace

void Register(TestCase* tc) noexcept {
  tc->next = nullptr;
  if (gTail == nullptr) {
    gHead = gTail = tc;
  } else {
    gTail->next = tc;
    gTail = tc;
  }
}

ExpectFailureScope::ExpectFailureScope() noexcept : previous_(gExpect) { gExpect = this; }
ExpectFailureScope::~ExpectFailureScope() { gExpect = previous_; }
std::uint32_t ExpectFailureScope::failures() const noexcept { return failures_; }
const char* ExpectFailureScope::lastMessage() const noexcept { return last_; }

void ReportFailure(const char* file, int line, const char* expression, const char* message) noexcept {
  if (gExpect != nullptr) {
    ++gExpect->failures_;
    std::size_t len = 0;
    AppendBounded(gExpect->last_, sizeof(gExpect->last_), &len, expression);
    if (message != nullptr) {
      AppendBounded(gExpect->last_, sizeof(gExpect->last_), &len, " ");
      AppendBounded(gExpect->last_, sizeof(gExpect->last_), &len, message);
    }
    return;
  }
  gCurrentFailed = true;
  PrintLine(stderr, "%s:%d: FAILED: %s%s%s\n", file, line, expression ? expression : "",
            message ? "  " : "", message ? message : "");
}

void ReportFailureF(const char* file, int line, const char* expression, const char* fmt, ...) noexcept {
  char buf[1024];
  {
    snd::rt::SuspendScope suspend;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  ReportFailure(file, line, expression, buf);
}

void Note(const char* fmt, ...) noexcept {
  char buf[1024];
  {
    snd::rt::SuspendScope suspend;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  }
  PrintLine(stdout, "    note: %s\n", buf);
}

bool CurrentTestFailed() noexcept { return gCurrentFailed; }
const char* CurrentTestName() noexcept { return gCurrent ? gCurrent->name : ""; }
bool UpdateGoldenRequested() noexcept { return gUpdateGolden; }

namespace detail {
void FormatValue(char* out, std::size_t n, long long v) noexcept { std::snprintf(out, n, "%lld", v); }
void FormatValue(char* out, std::size_t n, unsigned long long v) noexcept {
  std::snprintf(out, n, "%llu (0x%llx)", v, v);
}
void FormatValue(char* out, std::size_t n, double v) noexcept { std::snprintf(out, n, "%.9g", v); }
void FormatValue(char* out, std::size_t n, const void* v) noexcept { std::snprintf(out, n, "%p", v); }
void FormatValue(char* out, std::size_t n, const char* v) noexcept {
  std::snprintf(out, n, "\"%s\"", v ? v : "(null)");
}
void FormatValue(char* out, std::size_t n, bool v) noexcept { std::snprintf(out, n, "%s", v ? "true" : "false"); }

void ReportBinary(const char* file, int line, const char* expr, const char* lhs, const char* rhs) noexcept {
  char msg[300];
  std::snprintf(msg, sizeof(msg), "(lhs: %s, rhs: %s)", lhs, rhs);
  ReportFailure(file, line, expr, msg);
}

bool CheckNear(double a, double b, double tol, const char* file, int line, const char* expr) noexcept {
  const double d = a > b ? a - b : b - a;
  if (d <= tol) return true;  // NaN fails
  char msg[200];
  std::snprintf(msg, sizeof(msg), "(lhs: %.9g, rhs: %.9g, |diff| %.3g > tol %.3g)", a, b, d, tol);
  ReportFailure(file, line, expr, msg);
  return false;
}
}  // namespace detail

int RunAll(int argc, char** argv) noexcept {
  const char* filter = nullptr;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) {
      list = true;
    } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    } else if (std::strcmp(argv[i], "--update-golden") == 0) {
      gUpdateGolden = true;
    } else {
      PrintLine(stderr, "usage: %s [--list] [--filter <substring>] [--update-golden]\n", argv[0]);
      return 2;
    }
  }
  if (const char* env = std::getenv("SND_UPDATE_GOLDEN"); env != nullptr && env[0] == '1') {
    gUpdateGolden = true;
  }

  if (list) {
    for (TestCase* tc = gHead; tc != nullptr; tc = tc->next) {
      if (Matches(tc, filter)) PrintLine(stdout, "%s.%s\n", tc->suite, tc->name);
    }
    return 0;
  }

  rt::Install();
  int run = 0;
  int failed = 0;
  for (TestCase* tc = gHead; tc != nullptr; tc = tc->next) {
    if (!Matches(tc, filter)) continue;
    ++run;
    gCurrent = tc;
    gCurrentFailed = false;
    rt::Reset();
    PrintLine(stdout, "[ RUN      ] %s.%s\n", tc->suite, tc->name);
    tc->fn();
    if (rt::UnexpectedTotal() > 0) {
      rt::Record records[16];
      const std::uint32_t n = rt::Records(records, 16);
      for (std::uint32_t i = 0; i < n; ++i) {
        if (records[i].expected) continue;
        PrintLine(stderr, "    RT violation: %s at %s\n", snd::rt::ViolationName(records[i].kind),
                  records[i].site ? records[i].site : "?");
      }
      ReportFailure(tc->file, tc->line, "RT safety", "violations inside an RT scope (see above)");
    }
    if (gCurrentFailed) {
      ++failed;
      PrintLine(stdout, "[  FAILED  ] %s.%s\n", tc->suite, tc->name);
    } else {
      PrintLine(stdout, "[       OK ] %s.%s\n", tc->suite, tc->name);
    }
  }
  gCurrent = nullptr;
  PrintLine(stdout, "[==========] %d test(s) run, %d failed\n", run, failed);
  if (run == 0 && filter != nullptr) PrintLine(stdout, "note: no test matched filter '%s'\n", filter);
  return failed == 0 ? 0 : 1;
}

}  // namespace snd::test
