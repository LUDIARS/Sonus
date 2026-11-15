#include "snd/base/log.hpp"

#include <bit>
#include <cstdio>
#include <cstring>

#include "snd/internal/rt_check.hpp"

namespace snd::base {

namespace {

struct Out {
  char* p;
  std::size_t n;    // capacity including NUL
  std::size_t len;  // characters written so far
  void Put(const char* s, std::size_t k) noexcept {
    for (std::size_t i = 0; i < k && len + 1u < n; ++i) p[len++] = s[i];
  }
  void Put(char c) noexcept { Put(&c, 1u); }
};

bool IsFlag(char c) noexcept { return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0'; }
bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool IsLength(char c) noexcept {
  return c == 'h' || c == 'l' || c == 'L' || c == 'q' || c == 'j' || c == 'z' || c == 't';
}

// Runs snprintf with a format we built ourselves from a validated spec.
template <class T>
void Emit(Out& o, const char* spec, T value) noexcept {
  char buf[128];
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  const int k = std::snprintf(buf, sizeof(buf), spec, value);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
  if (k > 0) o.Put(buf, static_cast<std::size_t>(k) < sizeof(buf) ? static_cast<std::size_t>(k) : sizeof(buf) - 1u);
}

}  // namespace

std::size_t FormatLogEntry(const LogEntry& e, char* out, std::size_t n) noexcept {
  SND_RT_NOTE(Log);
  if (out == nullptr || n == 0u) return 0;
  Out o{out, n, 0};
  const char* f = e.format != nullptr ? e.format : "(null format)";
  std::uint32_t argi = 0;
  while (*f != '\0') {
    if (*f != '%') {
      o.Put(*f++);
      continue;
    }
    const char* start = f++;
    if (*f == '%') {
      o.Put('%');
      ++f;
      continue;
    }
    // spec = '%' flags width .precision (length dropped) conversion
    char spec[40];
    std::size_t sl = 0;
    spec[sl++] = '%';
    while (IsFlag(*f) && sl < 8u) spec[sl++] = *f++;
    while (IsDigit(*f) && sl < 16u) spec[sl++] = *f++;
    if (*f == '.') {
      spec[sl++] = *f++;
      while (IsDigit(*f) && sl < 24u) spec[sl++] = *f++;
    }
    while (IsLength(*f)) ++f;
    const char conv = *f;
    if (conv == '\0') {  // dangling '%': print it literally
      o.Put(start, static_cast<std::size_t>(f - start));
      break;
    }
    ++f;
    const bool known = std::strchr("diuoxXcpsfFeEgGaA", conv) != nullptr;
    if (!known) {
      o.Put(start, static_cast<std::size_t>(f - start));
      continue;
    }
    if (argi >= e.argCount || argi >= kLogMaxArgs) {
      o.Put("<?>", 3u);
      continue;
    }
    const std::uint64_t a = e.args[argi++];
    switch (conv) {
      case 'd':
      case 'i':
        spec[sl++] = 'l';
        spec[sl++] = 'l';
        spec[sl++] = conv;
        spec[sl] = '\0';
        Emit(o, spec, static_cast<long long>(a));
        break;
      case 'u':
      case 'o':
      case 'x':
      case 'X':
        spec[sl++] = 'l';
        spec[sl++] = 'l';
        spec[sl++] = conv;
        spec[sl] = '\0';
        Emit(o, spec, static_cast<unsigned long long>(a));
        break;
      case 'c':
        spec[sl++] = 'c';
        spec[sl] = '\0';
        Emit(o, spec, static_cast<int>(static_cast<unsigned char>(a)));
        break;
      case 'p':
        spec[sl++] = 'p';
        spec[sl] = '\0';
        Emit(o, spec, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)));
        break;
      case 's': {
        const char* s = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(a));
        spec[sl++] = 's';
        spec[sl] = '\0';
        Emit(o, spec, s != nullptr ? s : "(null)");
        break;
      }
      default:  // floating point
        spec[sl++] = conv;
        spec[sl] = '\0';
        Emit(o, spec, std::bit_cast<double>(a));
        break;
    }
  }
  out[o.len] = '\0';
  return o.len;
}

}  // namespace snd::base
