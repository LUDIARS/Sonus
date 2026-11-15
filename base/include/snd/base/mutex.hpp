// Mutex wrapper for non-RT code (design.md 1.2 principle 1, CONTRACTS.md D-36).
//
// Every acquisition notes SND_RT_NOTE(Lock): taking a Mutex inside an RT
// scope (audio thread, RT workers) is reported by the test kit. Non-RT code
// in the library uses this instead of std::mutex directly. Lock()/Unlock()
// are not recursive.
#ifndef SND_BASE_MUTEX_HPP
#define SND_BASE_MUTEX_HPP

#include <mutex>

#include "snd/internal/rt_check.hpp"

namespace snd::base {

class Mutex {
 public:
  Mutex() noexcept = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void Lock() noexcept {
    SND_RT_NOTE(Lock);
    m_.lock();
  }
  bool TryLock() noexcept {
    SND_RT_NOTE(Lock);
    return m_.try_lock();
  }
  void Unlock() noexcept { m_.unlock(); }

  // BasicLockable spelling (for std::unique_lock / std::condition_variable_any).
  void lock() noexcept { Lock(); }
  bool try_lock() noexcept { return TryLock(); }
  void unlock() noexcept { Unlock(); }

  std::mutex& native() noexcept { return m_; }

 private:
  std::mutex m_;
};

class LockGuard {
 public:
  explicit LockGuard(Mutex& m) noexcept : m_(m) { m_.Lock(); }
  ~LockGuard() { m_.Unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  Mutex& m_;
};

}  // namespace snd::base

#endif  // SND_BASE_MUTEX_HPP
