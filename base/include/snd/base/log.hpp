// Lock-free logging (design.md 1.2 principle 1, 12.1 base).
//
// RT threads never format. They push {level, static format string, up to 4
// raw arguments} into a fixed MPMC ring (Log / Push: RT-safe, lock-free, no
// allocation; when the ring is full the entry is dropped and counted). A
// non-RT thread drains the ring and formats (Drain / FormatLogEntry note
// SND_RT_NOTE(Log), so formatting inside an RT scope is reported).
//
//   ring.Log(LogLevel::Warn, "voice %u starved after %.2f ms", slot, ms);
//   ...                                      // later, on a non-RT thread
//   ring.Drain([](const LogEntry& e, const char* text) { puts(text); });
//
// Format strings must have static storage duration (only the pointer is
// stored). Supported conversions: d i u o x X c p s f F e E g G a A %%, with
// flags / width / precision; length modifiers are accepted and ignored (all
// integers are 64-bit). %s arguments must also be static strings. Missing
// arguments print "<?>".
#ifndef SND_BASE_LOG_HPP
#define SND_BASE_LOG_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/mpmc.hpp"
#include "snd/types.h"

namespace snd::base {

enum class LogLevel : std::uint32_t {
  Debug = SND_LOG_DEBUG,
  Info = SND_LOG_INFO,
  Warn = SND_LOG_WARN,
  Error = SND_LOG_ERROR,
};

inline constexpr std::uint32_t kLogMaxArgs = 4;

struct LogEntry {
  const char* format;  // static string
  std::uint64_t args[kLogMaxArgs];
  std::uint64_t timestamp;  // caller-defined (e.g. block index / clock); 0 if unused
  std::uint32_t level;      // LogLevel
  std::uint32_t argCount;
};

// Converts one argument to its 64-bit representation (RT-safe).
template <class T>
constexpr std::uint64_t LogArg(T v) noexcept {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    return v ? 1u : 0u;
  } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
    if constexpr (std::is_enum_v<U>) {
      return static_cast<std::uint64_t>(static_cast<std::underlying_type_t<U>>(v));
    } else if constexpr (std::is_signed_v<U>) {
      return static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
    } else {
      return static_cast<std::uint64_t>(v);
    }
  } else if constexpr (std::is_floating_point_v<U>) {
    return std::bit_cast<std::uint64_t>(static_cast<double>(v));
  } else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v));
  } else {
    static_assert(std::is_pointer_v<U>, "log arguments: integers, enums, floats, pointers, static strings");
    return 0;
  }
}

// Formats an entry into `out` (always NUL-terminated if n > 0). Returns the
// number of characters written (excluding NUL). Non-RT (notes Log).
std::size_t FormatLogEntry(const LogEntry& e, char* out, std::size_t n) noexcept;

class LogRing {
 public:
  LogRing() noexcept = default;
  LogRing(const LogRing&) = delete;
  LogRing& operator=(const LogRing&) = delete;

  // Non-RT. capacity rounded up to a power of two.
  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag = MemTag::Misc) noexcept {
    return ring_.Init(a, capacity, tag);
  }
  void Destroy() noexcept { ring_.Destroy(); }

  // Any thread, RT-safe. Entries below the minimum level are discarded (not counted).
  void SetMinLevel(LogLevel l) noexcept { minLevel_.store(static_cast<std::uint32_t>(l), std::memory_order_relaxed); }

  // Any thread, RT-safe. false when filtered out or dropped (ring full).
  bool Push(const LogEntry& e) noexcept {
    if (e.level < minLevel_.load(std::memory_order_relaxed)) return false;
    if (ring_.TryPush(e)) return true;
    dropped_.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  template <class... Args>
  bool Log(LogLevel level, const char* format, Args... args) noexcept {
    static_assert(sizeof...(Args) <= kLogMaxArgs, "at most 4 log arguments");
    LogEntry e{format, {LogArg(args)...}, 0u, static_cast<std::uint32_t>(level),
               static_cast<std::uint32_t>(sizeof...(Args))};
    return Push(e);
  }
  template <class... Args>
  bool LogAt(std::uint64_t timestamp, LogLevel level, const char* format, Args... args) noexcept {
    static_assert(sizeof...(Args) <= kLogMaxArgs, "at most 4 log arguments");
    LogEntry e{format, {LogArg(args)...}, timestamp, static_cast<std::uint32_t>(level),
               static_cast<std::uint32_t>(sizeof...(Args))};
    return Push(e);
  }

  // Any thread: raw entry without formatting (e.g. to forward as an EventRecord).
  bool TryPop(LogEntry& out) noexcept { return ring_.TryPop(out); }

  // Non-RT: pops up to `max` entries and calls sink(const LogEntry&, const char* text)
  // with the formatted text (truncated to 512 bytes). Returns the count.
  template <class Sink>
  std::uint32_t Drain(Sink&& sink, std::uint32_t max = 0xFFFFFFFFu) noexcept {
    char text[512];
    std::uint32_t n = 0;
    LogEntry e;
    while (n < max && ring_.TryPop(e)) {
      FormatLogEntry(e, text, sizeof(text));
      sink(static_cast<const LogEntry&>(e), static_cast<const char*>(text));
      ++n;
    }
    return n;
  }

  std::uint32_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  std::uint32_t TakeDropped() noexcept { return dropped_.exchange(0u, std::memory_order_relaxed); }
  std::uint32_t Capacity() const noexcept { return ring_.Capacity(); }

 private:
  MpmcRing<LogEntry> ring_;
  std::atomic<std::uint32_t> minLevel_{0};
  std::atomic<std::uint32_t> dropped_{0};
};

}  // namespace snd::base

#endif  // SND_BASE_LOG_HPP
