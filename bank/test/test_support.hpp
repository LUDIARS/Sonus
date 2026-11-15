// Shared helpers of the bank tests.
#ifndef SND_BANK_TEST_SUPPORT_HPP
#define SND_BANK_TEST_SUPPORT_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>

#include "snd/ext/allocator.hpp"

namespace testsupport {

// Thread-safe allocator that counts live allocations and can fail on demand.
class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag) override {
    std::lock_guard<std::mutex> lock(m_);
    if (failAt_ >= 0 && calls_++ == failAt_) return nullptr;
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (posix_memalign(&p, align, bytes) != 0) return nullptr;
    live_.insert(p);
    ++total_;
    return p;
  }
  void Free(void* p, snd::MemTag) override {
    if (p == nullptr) return;
    std::lock_guard<std::mutex> lock(m_);
    if (live_.erase(p) == 0) ++badFrees_;
    std::free(p);
  }
  std::size_t Live() const {
    std::lock_guard<std::mutex> lock(m_);
    return live_.size();
  }
  std::size_t Total() const {
    std::lock_guard<std::mutex> lock(m_);
    return total_;
  }
  std::size_t BadFrees() const {
    std::lock_guard<std::mutex> lock(m_);
    return badFrees_;
  }
  // Fails the n-th Allocate from now (0 = next); -1 disables.
  void FailAt(long n) {
    std::lock_guard<std::mutex> lock(m_);
    failAt_ = n;
    calls_ = 0;
  }

 private:
  mutable std::mutex m_;
  std::set<void*> live_;
  std::size_t total_ = 0;
  std::size_t badFrees_ = 0;
  long failAt_ = -1;
  long calls_ = 0;
};

// Deterministic xorshift (fixed seeds in the fuzz test).
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
  std::uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t Below(std::uint32_t n) { return n == 0 ? 0 : static_cast<std::uint32_t>(Next() % n); }
};

}  // namespace testsupport

#endif  // SND_BANK_TEST_SUPPORT_HPP
