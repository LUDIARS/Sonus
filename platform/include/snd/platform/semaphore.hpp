// Counting semaphore created up front (design.md 1.2 principle 1, 2.3).
//
// "事前に作った同期プリミティブでの起床と待機" — the only blocking primitive the
// audio thread and RT workers may use. Construct it at init (non-RT); after
// that every member is RT-safe in the sense of principle 1: no allocation,
// no mutex, no IO. Post never blocks. Wait/WaitFor block only in the kernel
// (Linux: a futex on the counter; other OSes: std::counting_semaphore, which
// is a stub for now and not verified RT-safe).
//
// Memory ordering: a Post happens-before the Wait/TryWait that consumes it.
#ifndef SND_PLATFORM_SEMAPHORE_HPP
#define SND_PLATFORM_SEMAPHORE_HPP

#include <atomic>
#include <cstdint>

#if !defined(__linux__)
#include <semaphore>
#endif

namespace snd::platform {

class Semaphore {
 public:
  explicit Semaphore(std::uint32_t initial = 0) noexcept;
  ~Semaphore();
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;

  // Any thread. Adds `count` permits and wakes up to `count` waiters.
  void Post(std::uint32_t count = 1) noexcept;
  // Any thread. Blocks until a permit is available, then takes it.
  void Wait() noexcept;
  // Any thread. Takes a permit if one is available, never blocks.
  bool TryWait() noexcept;
  // Any thread. Waits at most `timeoutNs`; false on timeout.
  bool WaitFor(std::uint64_t timeoutNs) noexcept;

  // Current permit count (racy; for tests and statistics).
  std::int32_t ValueApprox() const noexcept;

 private:
#if defined(__linux__)
  bool TryTake() noexcept;
  std::atomic<std::int32_t> count_;
  std::atomic<std::int32_t> waiters_{0};
#else
  std::counting_semaphore<0x7FFFFFFF> sem_;
  std::atomic<std::int32_t> approx_;
#endif
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_SEMAPHORE_HPP
