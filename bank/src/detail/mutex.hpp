// Mutex wrapper that reports RT violations (CONTRACTS.md D-36).
// detail: belongs in base (CONTRACTS.md §6) once base provides its lock wrapper.
#ifndef SND_BANK_SRC_DETAIL_MUTEX_HPP
#define SND_BANK_SRC_DETAIL_MUTEX_HPP

#include <mutex>

#include "snd/internal/rt_check.hpp"

namespace snd::bank::detail {

class Mutex {
 public:
  void lock() {
    SND_RT_NOTE(Lock);
    m_.lock();
  }
  void unlock() { m_.unlock(); }

 private:
  std::mutex m_;
};

class LockGuard {
 public:
  explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
  ~LockGuard() { m_.unlock(); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;

 private:
  Mutex& m_;
};

}  // namespace snd::bank::detail

#endif  // SND_BANK_SRC_DETAIL_MUTEX_HPP
