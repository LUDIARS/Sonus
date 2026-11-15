#include <cerrno>
#include <cstring>
#include <new>
#include <thread>

#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/thread.hpp"

#if defined(_WIN32)
#include <chrono>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

namespace snd::platform {

// --- clock ---------------------------------------------------------------------
#if defined(_WIN32)
std::uint64_t MonotonicNanoseconds() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}
void SleepFor(std::uint64_t ns) noexcept {
  SND_RT_NOTE(Blocking);
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
void SleepUntil(std::uint64_t deadlineNs) noexcept {
  const std::uint64_t now = MonotonicNanoseconds();
  if (deadlineNs > now) SleepFor(deadlineNs - now);
}
#else
std::uint64_t MonotonicNanoseconds() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
void SleepUntil(std::uint64_t deadlineNs) noexcept {
  SND_RT_NOTE(Blocking);
#if defined(__linux__)
  timespec ts;
  ts.tv_sec = static_cast<time_t>(deadlineNs / 1000000000ull);
  ts.tv_nsec = static_cast<long>(deadlineNs % 1000000000ull);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
#else
  const std::uint64_t now = MonotonicNanoseconds();
  if (deadlineNs <= now) return;
  const std::uint64_t ns = deadlineNs - now;
  timespec ts{static_cast<time_t>(ns / 1000000000ull), static_cast<long>(ns % 1000000000ull)};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
#endif
}
void SleepFor(std::uint64_t ns) noexcept { SleepUntil(MonotonicNanoseconds() + ns); }
#endif

void YieldThread() noexcept { std::this_thread::yield(); }

// --- threads -------------------------------------------------------------------------
const char* PriorityResultName(PriorityResult r) noexcept {
  switch (r) {
    case PriorityResult::Applied: return "applied";
    case PriorityResult::FellBack: return "fell-back-to-normal";
    case PriorityResult::Unsupported: return "unsupported";
    case PriorityResult::Failed: return "failed";
  }
  return "?";
}

std::uint32_t HardwareConcurrency() noexcept {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0u ? 1u : static_cast<std::uint32_t>(n);
}

#if defined(_WIN32)
// Stub (stage 1 targets Linux): std::thread, no priorities.
struct Thread::Native {
  std::thread t;
};

bool Thread::Start(const ThreadDesc& desc, ThreadFn fn, void* user) noexcept {
  static_assert(sizeof(Native) <= sizeof(native_) && alignof(Native) <= 8, "Thread::native_ too small");
  if (started_ || fn == nullptr) return false;
  auto* n = ::new (native_) Native();
  n->t = std::thread([fn, user] { fn(user); });
  started_ = true;
  priority_ = desc.priority == ThreadPriority::Normal ? PriorityResult::Applied : PriorityResult::Unsupported;
  return true;
}
void Thread::Join() noexcept {
  if (!started_) return;
  auto* n = reinterpret_cast<Native*>(native_);
  n->t.join();
  n->~Native();
  started_ = false;
}
PriorityResult SetCurrentThreadPriority(ThreadPriority priority, int) noexcept {
  return priority == ThreadPriority::Normal ? PriorityResult::Applied : PriorityResult::Unsupported;
}
void SetCurrentThreadName(const char*) noexcept {}
#else
struct Thread::Native {
  pthread_t handle;
};

namespace {
// Copies at most 15 bytes and terminates (pthread names are limited to 16 bytes).
void CopyName(char (&dst)[16], const char* src) noexcept {
  std::size_t n = 0;
  while (n < sizeof(dst) - 1u && src[n] != '\0') ++n;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

struct StartData {
  ThreadFn fn;
  void* user;
  char name[16];
};

void* Trampoline(void* p) {
  StartData d = *static_cast<StartData*>(p);
  delete static_cast<StartData*>(p);
  SetCurrentThreadName(d.name);
  d.fn(d.user);
  return nullptr;
}

PriorityResult ApplyPriority(pthread_t t, ThreadPriority priority, int rtPriority) noexcept {
#if defined(__linux__)
  sched_param sp{};
  if (priority == ThreadPriority::Normal) {
    sp.sched_priority = 0;
    return pthread_setschedparam(t, SCHED_OTHER, &sp) == 0 ? PriorityResult::Applied : PriorityResult::Failed;
  }
  const int lo = sched_get_priority_min(SCHED_FIFO);
  const int hi = sched_get_priority_max(SCHED_FIFO);
  sp.sched_priority = rtPriority < lo ? lo : (rtPriority > hi ? hi : rtPriority);
  const int rc = pthread_setschedparam(t, SCHED_FIFO, &sp);
  if (rc == 0) return PriorityResult::Applied;
  if (rc == EPERM) return PriorityResult::FellBack;
  return PriorityResult::Failed;
#else
  (void)t;
  (void)rtPriority;
  return priority == ThreadPriority::Normal ? PriorityResult::Applied : PriorityResult::Unsupported;
#endif
}
}  // namespace

bool Thread::Start(const ThreadDesc& desc, ThreadFn fn, void* user) noexcept {
  static_assert(sizeof(Native) <= sizeof(native_) && alignof(Native) <= 8, "Thread::native_ too small");
  if (started_ || fn == nullptr) return false;
  auto* data = new (std::nothrow) StartData{fn, user, {}};
  if (data == nullptr) return false;
  if (desc.name != nullptr) {
    CopyName(data->name, desc.name);
  }
  // The scheduling policy is part of the creation attributes, so the thread
  // starts at its priority (no window at normal priority, no race with a
  // thread that already exited). EPERM -> create again at normal priority.
  auto* n = ::new (native_) Native();
  int rc = 0;
  priority_ = PriorityResult::Applied;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool realtime = desc.priority == ThreadPriority::Realtime && attempt == 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    std::size_t stack = desc.stackBytes;
    if (stack == 0u && desc.minStackBytes != 0u) {
      std::size_t osDefault = 0;
      if (pthread_attr_getstacksize(&attr, &osDefault) != 0) osDefault = 0;
      if (osDefault < desc.minStackBytes) stack = desc.minStackBytes;
    } else if (stack != 0u && stack < desc.minStackBytes) {
      stack = desc.minStackBytes;
    }
    // A refused size is ignored as before unless a minimum was asked for (D-47).
    if (stack != 0u && pthread_attr_setstacksize(&attr, stack) != 0 && desc.minStackBytes != 0u) {
      pthread_attr_destroy(&attr);
      rc = EINVAL;
      break;
    }
#if defined(__linux__)
    if (realtime) {
      const int lo = sched_get_priority_min(SCHED_FIFO);
      const int hi = sched_get_priority_max(SCHED_FIFO);
      sched_param sp{};
      sp.sched_priority = desc.realtimePriority < lo ? lo : (desc.realtimePriority > hi ? hi : desc.realtimePriority);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &sp);
    }
#endif
    rc = pthread_create(&n->handle, &attr, &Trampoline, data);
    pthread_attr_destroy(&attr);
    if (rc == 0) break;
    if (!realtime) break;
    // Realtime refused (EPERM: no CAP_SYS_NICE / RLIMIT_RTPRIO; EINVAL: policy unsupported).
    priority_ = rc == EPERM ? PriorityResult::FellBack : PriorityResult::Failed;
  }
  if (rc != 0) {
    delete data;
    n->~Native();
    return false;
  }
#if !defined(__linux__)
  if (desc.priority == ThreadPriority::Realtime) priority_ = PriorityResult::Unsupported;
#endif
  started_ = true;
  return true;
}

void Thread::Join() noexcept {
  if (!started_) return;
  auto* n = reinterpret_cast<Native*>(native_);
  pthread_join(n->handle, nullptr);
  n->~Native();
  started_ = false;
}

PriorityResult SetCurrentThreadPriority(ThreadPriority priority, int realtimePriority) noexcept {
  return ApplyPriority(pthread_self(), priority, realtimePriority);
}

void SetCurrentThreadName(const char* name) noexcept {
#if defined(__linux__)
  if (name == nullptr || name[0] == '\0') return;
  char buf[16] = {};
  CopyName(buf, name);
  pthread_setname_np(pthread_self(), buf);
#else
  (void)name;
#endif
}
#endif

Thread::~Thread() { Join(); }

}  // namespace snd::platform
