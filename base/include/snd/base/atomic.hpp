// Atomic helpers (design.md 2.1, 12.1 base).
//
// Everything here is RT-safe (no allocation, no lock, no syscall) and may be
// called from any thread, including the audio thread and RT workers.
#ifndef SND_BASE_ATOMIC_HPP
#define SND_BASE_ATOMIC_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace snd::base {

// Destructive-interference size used for padding hot atomics (2.1 SPSC indices).
inline constexpr std::size_t kCacheLine = 64;

// Value padded to its own cache line(s) to avoid false sharing.
template <class T>
struct alignas(kCacheLine) CachePadded {
  T value{};
};

// Spin-loop hint (PAUSE / YIELD). RT-safe.
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Raises `a` to at least `v` (monotonic maximum, e.g. arena high-water marks).
template <class T>
inline void AtomicFetchMax(std::atomic<T>& a, T v, std::memory_order mo = std::memory_order_relaxed) noexcept {
  T cur = a.load(std::memory_order_relaxed);
  while (cur < v && !a.compare_exchange_weak(cur, v, mo, std::memory_order_relaxed)) {
  }
}

// --- Word-wise atomic copies --------------------------------------------------
// Copies used by seqlock readers/writers (notify.hpp, design 2.1 / D-16): the
// buffers are accessed concurrently, so every 8-byte word goes through an
// atomic access: release stores (CopyIn) and acquire loads (CopyOut). That
// ordering makes the seqlock correct without standalone fences, which
// ThreadSanitizer does not support (well-defined under the C++ memory model;
// free on x86, ldar/stlr on ARM). `bytes` must be a multiple of 8 and both pointers 8-byte
// aligned. The may_alias type allows copying any POD (floats, structs).
#if defined(__GNUC__) || defined(__clang__)
using AliasU64 = std::uint64_t __attribute__((may_alias));
inline void AtomicCopyIn(void* dst, const void* src, std::size_t bytes) noexcept {
  auto* d = static_cast<AliasU64*>(dst);
  const auto* s = static_cast<const AliasU64*>(src);
  for (std::size_t i = 0, n = bytes / 8u; i < n; ++i) __atomic_store_n(&d[i], s[i], __ATOMIC_RELEASE);
}
inline void AtomicCopyOut(void* dst, const void* src, std::size_t bytes) noexcept {
  auto* d = static_cast<AliasU64*>(dst);
  auto* s = static_cast<AliasU64*>(const_cast<void*>(src));
  for (std::size_t i = 0, n = bytes / 8u; i < n; ++i) d[i] = __atomic_load_n(&s[i], __ATOMIC_ACQUIRE);
}
#else
inline void AtomicCopyIn(void* dst, const void* src, std::size_t bytes) noexcept { std::memcpy(dst, src, bytes); }
inline void AtomicCopyOut(void* dst, const void* src, std::size_t bytes) noexcept { std::memcpy(dst, src, bytes); }
#endif

// Bit helpers.
constexpr bool IsPowerOfTwo(std::uint64_t v) noexcept { return v != 0u && (v & (v - 1u)) == 0u; }
constexpr std::uint64_t NextPowerOfTwo(std::uint64_t v) noexcept {
  if (v <= 1u) return 1u;
  --v;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v |= v >> 32;
  return v + 1u;
}
constexpr std::size_t AlignUp(std::size_t v, std::size_t align) noexcept {
  return (v + (align - 1u)) & ~(align - 1u);
}
inline int CountTrailingZeros64(std::uint64_t v) noexcept { return std::countr_zero(v); }

}  // namespace snd::base

#endif  // SND_BASE_ATOMIC_HPP
