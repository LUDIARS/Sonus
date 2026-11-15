// RT-safety detection hooks (design.md 1.2 principle 1, 14). FROZEN CONTRACT.
//
// The audio thread and RT workers mark the code they run with an RT scope:
//
//   void AudioThread::RenderBlock() {
//     SND_RT_SCOPE();          // everything below must obey principle 1
//     ...
//   }
//
// Inside a scope, the test kit (tests/framework, linked only into test
// executables) detects:
//   - heap allocation / deallocation (operator new/delete always; malloc
//     family, pthread mutex/rwlock and open/read/write via libc interposition
//     in builds without sanitizers),
//   - explicit notes placed by library code at forbidden operations:
//       SND_RT_NOTE(Lock)    in any lock wrapper (snd base mutex),
//       SND_RT_NOTE(FileIo)  in every IFileSystem implementation,
//       SND_RT_NOTE(Log)     in formatted logging,
//       SND_RT_NOTE(Alloc)   in IAllocator implementations.
// A violation is reported to the installed handler (the test kit records it
// and fails the running test). With SND_RT_CHECKS=0 (release) everything here
// compiles to nothing.
#ifndef SND_INTERNAL_RT_CHECK_HPP
#define SND_INTERNAL_RT_CHECK_HPP

#include <atomic>
#include <cstdint>

#ifndef SND_RT_CHECKS
#define SND_RT_CHECKS 0
#endif

namespace snd::rt {

enum class Violation : std::uint32_t {
  Alloc = 0,   // heap allocation
  Free = 1,    // heap deallocation
  Lock = 2,    // mutex / rwlock acquisition
  FileIo = 3,  // file open/read/write, IFileSystem calls
  Log = 4,     // formatted logging
  Blocking = 5,  // other blocking calls (sleep, condition wait) noted explicitly
  Count = 6,
};

inline constexpr const char* ViolationName(Violation v) noexcept {
  switch (v) {
    case Violation::Alloc: return "alloc";
    case Violation::Free: return "free";
    case Violation::Lock: return "lock";
    case Violation::FileIo: return "file-io";
    case Violation::Log: return "log";
    case Violation::Blocking: return "blocking";
    case Violation::Count: break;
  }
  return "?";
}

// Must not allocate, lock or do IO. `site` is a static string (may be null).
using ViolationHandler = void (*)(Violation kind, const char* site) noexcept;

#if SND_RT_CHECKS

// volatile: the compiler must not drop the scope bookkeeping around calls it
// believes touch no user memory (malloc, operator new) at high optimisation.
inline thread_local volatile std::uint32_t tRtDepth = 0;     // > 0: inside an RT scope
inline thread_local volatile std::uint32_t tSuspendDepth = 0;  // > 0: checks suspended
inline std::atomic<ViolationHandler> gHandler{nullptr};

inline constexpr bool kEnabled = true;

inline bool InRtScope() noexcept { return tRtDepth > 0u && tSuspendDepth == 0u; }

inline void SetHandler(ViolationHandler h) noexcept { gHandler.store(h, std::memory_order_release); }

inline void Report(Violation v, const char* site) noexcept {
  if (!InRtScope()) return;
  ViolationHandler h = gHandler.load(std::memory_order_acquire);
  if (h == nullptr) return;
  tSuspendDepth = tSuspendDepth + 1u;
  h(v, site);
  tSuspendDepth = tSuspendDepth - 1u;
}

class RtScope {
 public:
  RtScope() noexcept { tRtDepth = tRtDepth + 1u; }
  ~RtScope() { tRtDepth = tRtDepth - 1u; }
  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;
};

// Temporarily leaves RT checking (e.g. the test kit's own bookkeeping). Never
// use in library code.
class SuspendScope {
 public:
  SuspendScope() noexcept { tSuspendDepth = tSuspendDepth + 1u; }
  ~SuspendScope() { tSuspendDepth = tSuspendDepth - 1u; }
  SuspendScope(const SuspendScope&) = delete;
  SuspendScope& operator=(const SuspendScope&) = delete;
};

#else  // SND_RT_CHECKS

inline constexpr bool kEnabled = false;
inline constexpr bool InRtScope() noexcept { return false; }
inline void SetHandler(ViolationHandler) noexcept {}
inline void Report(Violation, const char*) noexcept {}
class RtScope {
 public:
  RtScope() noexcept = default;
  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;
};
class SuspendScope {
 public:
  SuspendScope() noexcept = default;
  SuspendScope(const SuspendScope&) = delete;
  SuspendScope& operator=(const SuspendScope&) = delete;
};

#endif  // SND_RT_CHECKS

}  // namespace snd::rt

#define SND_RT_CONCAT_INNER(a, b) a##b
#define SND_RT_CONCAT(a, b) SND_RT_CONCAT_INNER(a, b)

#if SND_RT_CHECKS
#define SND_RT_SCOPE() ::snd::rt::RtScope SND_RT_CONCAT(snd_rt_scope_, __LINE__)
#define SND_RT_NOTE(kind) ::snd::rt::Report(::snd::rt::Violation::kind, __FILE__)
#else
#define SND_RT_SCOPE() static_cast<void>(0)
#define SND_RT_NOTE(kind) static_cast<void>(0)
#endif

#endif  // SND_INTERNAL_RT_CHECK_HPP
