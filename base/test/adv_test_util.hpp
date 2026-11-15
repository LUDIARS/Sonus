// Shared helpers of the independent (adversarial) base tests.
#ifndef SND_BASE_TEST_ADV_TEST_UTIL_HPP
#define SND_BASE_TEST_ADV_TEST_UTIL_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace snd::advtest {

// Fixed-seed generator (reproducible model tests).
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) noexcept : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  std::uint64_t Next() noexcept {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  std::uint32_t Below(std::uint32_t n) noexcept { return n == 0u ? 0u : static_cast<std::uint32_t>(Next() % n); }
};

// Iteration multiplier: SND_STRESS_SCALE=<n> in the environment makes the
// stress loops n times longer (default 1). Sanitizer builds keep 1.
inline std::uint32_t StressScale() noexcept {
  const char* v = std::getenv("SND_STRESS_SCALE");
  if (v == nullptr) return 1u;
  const long n = std::strtol(v, nullptr, 10);
  return n < 1 ? 1u : static_cast<std::uint32_t>(n);
}

// Spin barrier for N threads (start everything at once to maximise overlap).
class StartGate {
 public:
  explicit StartGate(std::uint32_t n) noexcept : n_(n) {}
  void ArriveAndWait() noexcept {
    arrived_.fetch_add(1, std::memory_order_acq_rel);
    while (arrived_.load(std::memory_order_acquire) < n_) std::this_thread::yield();
  }

 private:
  std::uint32_t n_;
  std::atomic<std::uint32_t> arrived_{0};
};

}  // namespace snd::advtest

#endif  // SND_BASE_TEST_ADV_TEST_UTIL_HPP
