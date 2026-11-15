// Minimal test runner (design.md 12.2-7, 14). No third-party dependency.
//
//   #include "snd/test/test.hpp"
//   SND_TEST(Mixer, SumsInChunkOrder) {
//     SND_CHECK(x == 1);
//     SND_CHECK_EQ(a, b);                 // prints both values on failure
//     SND_CHECK_NEAR(f, 0.5f, 1e-6f);
//     SND_REQUIRE(ptr != nullptr);        // stops the test on failure
//   }
//
// Every test executable links sonus_testkit_main (provides main) and is one
// CTest test (snd_add_test). Command line:
//   <exe> [--list] [--filter <substring>] [--update-golden]
// After each test the runner fails it if an RT-safety violation was recorded
// outside snd::test::rt::ExpectViolations (snd/test/rt_guard.hpp).
#ifndef SND_TEST_TEST_HPP
#define SND_TEST_TEST_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace snd::test {

using TestFn = void (*)();

struct TestCase {
  const char* suite;
  const char* name;
  TestFn fn;
  const char* file;
  int line;
  TestCase* next;  // intrusive registration list (no allocation at static init)
};

void Register(TestCase* tc) noexcept;

struct Registrar {
  explicit Registrar(TestCase* tc) noexcept { Register(tc); }
};

// Records a failure of the running test. `message` may be null.
void ReportFailure(const char* file, int line, const char* expression, const char* message) noexcept;
// printf-style variant (the formatted text is truncated to 1 KiB).
void ReportFailureF(const char* file, int line, const char* expression, const char* fmt, ...) noexcept
    __attribute__((format(printf, 4, 5)));

bool CurrentTestFailed() noexcept;
const char* CurrentTestName() noexcept;

// Prints an informational line attributed to the running test.
void Note(const char* fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

// Captures failures instead of failing the test (used to test the framework itself).
class ExpectFailureScope {
 public:
  ExpectFailureScope() noexcept;
  ~ExpectFailureScope();
  ExpectFailureScope(const ExpectFailureScope&) = delete;
  ExpectFailureScope& operator=(const ExpectFailureScope&) = delete;
  std::uint32_t failures() const noexcept;
  const char* lastMessage() const noexcept;

 private:
  ExpectFailureScope* previous_;
  std::uint32_t failures_ = 0;
  char last_[1024] = {};
  friend void ReportFailure(const char*, int, const char*, const char*) noexcept;
};

// Global options set from the command line.
bool UpdateGoldenRequested() noexcept;

int RunAll(int argc, char** argv) noexcept;

// --- value printing for SND_CHECK_EQ / NE -----------------------------------------
namespace detail {
void FormatValue(char* out, std::size_t n, long long v) noexcept;
void FormatValue(char* out, std::size_t n, unsigned long long v) noexcept;
void FormatValue(char* out, std::size_t n, double v) noexcept;
void FormatValue(char* out, std::size_t n, const void* v) noexcept;
void FormatValue(char* out, std::size_t n, const char* v) noexcept;
void FormatValue(char* out, std::size_t n, bool v) noexcept;

template <class T>
void Format(char* out, std::size_t n, const T& v) noexcept {
  if constexpr (std::is_same_v<T, bool>) {
    FormatValue(out, n, v);
  } else if constexpr (std::is_enum_v<T>) {
    FormatValue(out, n, static_cast<long long>(static_cast<std::underlying_type_t<T>>(v)));
  } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
    FormatValue(out, n, static_cast<long long>(v));
  } else if constexpr (std::is_integral_v<T>) {
    FormatValue(out, n, static_cast<unsigned long long>(v));
  } else if constexpr (std::is_floating_point_v<T>) {
    FormatValue(out, n, static_cast<double>(v));
  } else if constexpr (std::is_same_v<std::decay_t<T>, const char*> ||
                       std::is_same_v<std::decay_t<T>, char*>) {
    FormatValue(out, n, static_cast<const char*>(v));
  } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
    FormatValue(out, n, static_cast<const void*>(v));
  } else {
    FormatValue(out, n, "<value>");
  }
}

void ReportBinary(const char* file, int line, const char* expr, const char* lhs,
                  const char* rhs) noexcept;

template <class T>
inline constexpr bool kCmpInt =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t> &&
    !std::is_same_v<T, wchar_t>;

template <class A, class B>
bool CheckEq(const A& a, const B& b, const char* file, int line, const char* expr, bool equal) noexcept {
  bool same;
  if constexpr (kCmpInt<A> && kCmpInt<B>) {
    same = std::cmp_equal(a, b);  // mixed signedness compares mathematically
  } else {
    same = (a == b);
  }
  if (same == equal) return true;
  char l[128], r[128];
  Format(l, sizeof(l), a);
  Format(r, sizeof(r), b);
  ReportBinary(file, line, expr, l, r);
  return false;
}

bool CheckNear(double a, double b, double tol, const char* file, int line, const char* expr) noexcept;
}  // namespace detail

}  // namespace snd::test

#define SND_TEST_CONCAT_INNER(a, b) a##b
#define SND_TEST_CONCAT(a, b) SND_TEST_CONCAT_INNER(a, b)

#define SND_TEST(Suite, Name)                                                                   \
  static void snd_test_fn_##Suite##_##Name();                                                   \
  static ::snd::test::TestCase snd_test_case_##Suite##_##Name{                                  \
      #Suite, #Name, &snd_test_fn_##Suite##_##Name, __FILE__, __LINE__, nullptr};               \
  static const ::snd::test::Registrar snd_test_reg_##Suite##_##Name{&snd_test_case_##Suite##_##Name}; \
  static void snd_test_fn_##Suite##_##Name()

#define SND_CHECK(cond)                                                                   \
  do {                                                                                    \
    if (!(cond)) ::snd::test::ReportFailure(__FILE__, __LINE__, #cond, nullptr);          \
  } while (0)

#define SND_CHECK_MSG(cond, ...)                                                          \
  do {                                                                                    \
    if (!(cond)) ::snd::test::ReportFailureF(__FILE__, __LINE__, #cond, __VA_ARGS__);     \
  } while (0)

#define SND_REQUIRE(cond)                                                                 \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      ::snd::test::ReportFailure(__FILE__, __LINE__, #cond, "(required)");                \
      return;                                                                             \
    }                                                                                     \
  } while (0)

#define SND_CHECK_EQ(a, b) \
  static_cast<void>(::snd::test::detail::CheckEq((a), (b), __FILE__, __LINE__, #a " == " #b, true))
#define SND_CHECK_NE(a, b) \
  static_cast<void>(::snd::test::detail::CheckEq((a), (b), __FILE__, __LINE__, #a " != " #b, false))
#define SND_REQUIRE_EQ(a, b)                                                                     \
  do {                                                                                           \
    if (!::snd::test::detail::CheckEq((a), (b), __FILE__, __LINE__, #a " == " #b, true)) return; \
  } while (0)
#define SND_CHECK_NEAR(a, b, tol)                                                                 \
  static_cast<void>(::snd::test::detail::CheckNear(static_cast<double>(a), static_cast<double>(b), \
                                                   static_cast<double>(tol), __FILE__, __LINE__,  \
                                                   #a " ~= " #b))

#endif  // SND_TEST_TEST_HPP
