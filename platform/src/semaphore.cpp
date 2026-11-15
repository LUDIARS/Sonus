#include "snd/platform/semaphore.hpp"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>

#include "snd/platform/clock.hpp"
#endif

namespace snd::platform {

#if defined(__linux__)
namespace {
// The futex word is the counter itself; the kernel re-checks `expected`
// atomically before sleeping, so a Post between our check and the syscall
// is never lost.
void FutexWait(std::atomic<std::int32_t>* word, std::int32_t expected, const timespec* relTimeout) noexcept {
  syscall(SYS_futex, reinterpret_cast<std::int32_t*>(word), FUTEX_WAIT_PRIVATE, expected, relTimeout, nullptr, 0);
}
void FutexWake(std::atomic<std::int32_t>* word, std::int32_t count) noexcept {
  syscall(SYS_futex, reinterpret_cast<std::int32_t*>(word), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}
static_assert(sizeof(std::atomic<std::int32_t>) == sizeof(std::int32_t));
}  // namespace

Semaphore::Semaphore(std::uint32_t initial) noexcept : count_(static_cast<std::int32_t>(initial)) {}
Semaphore::~Semaphore() = default;

void Semaphore::Post(std::uint32_t count) noexcept {
  if (count == 0u) return;
  count_.fetch_add(static_cast<std::int32_t>(count), std::memory_order_seq_cst);
  if (waiters_.load(std::memory_order_seq_cst) > 0) {
    FutexWake(&count_, static_cast<std::int32_t>(count));
  }
}

bool Semaphore::TryTake() noexcept {
  std::int32_t c = count_.load(std::memory_order_relaxed);
  while (c > 0) {
    if (count_.compare_exchange_weak(c, c - 1, std::memory_order_acquire, std::memory_order_relaxed)) return true;
  }
  return false;
}

bool Semaphore::TryWait() noexcept { return TryTake(); }

void Semaphore::Wait() noexcept {
  for (;;) {
    if (TryTake()) return;
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    if (count_.load(std::memory_order_seq_cst) <= 0) FutexWait(&count_, 0, nullptr);
    waiters_.fetch_sub(1, std::memory_order_relaxed);
  }
}

bool Semaphore::WaitFor(std::uint64_t timeoutNs) noexcept {
  const std::uint64_t deadline = MonotonicNanoseconds() + timeoutNs;
  for (;;) {
    if (TryTake()) return true;
    const std::uint64_t now = MonotonicNanoseconds();
    if (now >= deadline) return false;
    const std::uint64_t rem = deadline - now;
    timespec ts;
    ts.tv_sec = static_cast<time_t>(rem / 1000000000ull);
    ts.tv_nsec = static_cast<long>(rem % 1000000000ull);
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    if (count_.load(std::memory_order_seq_cst) <= 0) FutexWait(&count_, 0, &ts);
    waiters_.fetch_sub(1, std::memory_order_relaxed);
  }
}

std::int32_t Semaphore::ValueApprox() const noexcept { return count_.load(std::memory_order_relaxed); }

#else  // stub: std::counting_semaphore

Semaphore::Semaphore(std::uint32_t initial) noexcept
    : sem_(static_cast<std::ptrdiff_t>(initial)), approx_(static_cast<std::int32_t>(initial)) {}
Semaphore::~Semaphore() = default;
void Semaphore::Post(std::uint32_t count) noexcept {
  if (count == 0u) return;
  approx_.fetch_add(static_cast<std::int32_t>(count), std::memory_order_relaxed);
  sem_.release(static_cast<std::ptrdiff_t>(count));
}
void Semaphore::Wait() noexcept {
  sem_.acquire();
  approx_.fetch_sub(1, std::memory_order_relaxed);
}
bool Semaphore::TryWait() noexcept {
  if (!sem_.try_acquire()) return false;
  approx_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
bool Semaphore::WaitFor(std::uint64_t timeoutNs) noexcept {
  if (!sem_.try_acquire_for(std::chrono::nanoseconds(timeoutNs))) return false;
  approx_.fetch_sub(1, std::memory_order_relaxed);
  return true;
}
std::int32_t Semaphore::ValueApprox() const noexcept { return approx_.load(std::memory_order_relaxed); }

#endif

}  // namespace snd::platform
