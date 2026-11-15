// Allocators and memory accounting (design.md 1.2 principle 2, 3.3).
//
// All memory of the runtime comes from the user's IAllocator at
// initialisation (or bank loading on normal workers). Nothing here may be
// called from the audio thread or RT workers: every allocating entry point
// notes SND_RT_NOTE(Alloc) / SND_RT_NOTE(Free) so tests catch misuse.
//
//   snd::IAllocator& sys = snd::base::DefaultAllocator();     // malloc family
//   snd::base::AccountingAllocator acc(sys);                  // per-tag stats (3.3)
//   acc.SetBudget(snd::MemTag::StreamPool, 8u << 20);         // optional cap
//   float* buf = snd::base::AllocateArray<float>(acc, 256, snd::MemTag::Tables);
//   ...
//   snd::base::FreeArray(acc, buf, 256, snd::MemTag::Tables);
#ifndef SND_BASE_ALLOCATOR_HPP
#define SND_BASE_ALLOCATOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "snd/ext/allocator.hpp"
#include "snd/ext/types.hpp"
#include "snd/internal/common.hpp"

namespace snd::base {

inline constexpr std::size_t kMinAlignment = 16;  // IAllocator contract: align >= 16
inline constexpr std::uint32_t kMemTagCount = static_cast<std::uint32_t>(MemTag::Count);

// IAllocator over the C runtime (posix_memalign / _aligned_malloc).
// Thread-safe. Not RT-safe (notes Alloc / Free). Never fails except on OOM or
// invalid arguments (bytes == 0, align not a power of two) -> nullptr.
class MallocAllocator final : public IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, MemTag tag) override;
  void Free(void* p, MemTag tag) override;
};

// Process-wide MallocAllocator (used when the user passes no allocator).
IAllocator& DefaultAllocator() noexcept;

// Snapshot of one memory class (3.3 "区分ごとに集計").
struct MemTagUsage {
  std::uint64_t used = 0;         // bytes currently allocated (payload, excluding headers)
  std::uint64_t peak = 0;         // high-water mark of `used`
  std::uint64_t budget = 0;       // 0 = unlimited
  std::uint64_t allocations = 0;  // live allocation count
  std::uint64_t failures = 0;     // requests refused (budget or upstream failure)
};

// Decorator that tracks bytes per MemTag and optionally enforces a budget per
// tag. Each block carries a small header (the payload size), so Free() can
// account without the caller passing the size. Thread-safe (atomics only);
// not RT-safe because the upstream allocator is not.
//
// Budget semantics: a request that would push `used` above a non-zero budget
// returns nullptr and counts a failure (callers implement 1.2 principle 2 by
// virtualising / rejecting, not by retrying).
class AccountingAllocator final : public IAllocator {
 public:
  explicit AccountingAllocator(IAllocator& upstream) noexcept : upstream_(upstream) {}
  AccountingAllocator(const AccountingAllocator&) = delete;
  AccountingAllocator& operator=(const AccountingAllocator&) = delete;

  void* Allocate(std::size_t bytes, std::size_t align, MemTag tag) override;
  void Free(void* p, MemTag tag) override;

  // Any thread. Takes effect for later requests; does not free anything.
  void SetBudget(MemTag tag, std::uint64_t bytes) noexcept;
  // Any thread, lock-free (relaxed snapshot; fields may be from slightly different instants).
  MemTagUsage Usage(MemTag tag) const noexcept;
  std::uint64_t TotalUsed() const noexcept;
  // Fills snd_stats::memory_used / memory_budget style arrays (SND_MEM_TAG_COUNT entries).
  void FillStats(std::uint64_t* used, std::uint64_t* budget) const noexcept;
  IAllocator& upstream() const noexcept { return upstream_; }

 private:
  struct Counters {
    std::atomic<std::uint64_t> used{0};
    std::atomic<std::uint64_t> peak{0};
    std::atomic<std::uint64_t> budget{0};
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> failures{0};
  };
  static std::uint32_t Index(MemTag tag) noexcept;
  IAllocator& upstream_;
  Counters counters_[kMemTagCount];
};

// --- Typed helpers (non-RT) ---------------------------------------------------
// Alignment used for arrays: max(alignof(T), 16), or `align` if larger.
template <class T>
constexpr std::size_t ArrayAlignment(std::size_t align = kBufferAlignment) noexcept {
  std::size_t a = alignof(T) > kMinAlignment ? alignof(T) : kMinAlignment;
  return a > align ? a : align;
}

// Allocates `count` value-initialised T (64-byte aligned by default, 3.4).
// Returns nullptr on failure or count == 0.
template <class T>
T* AllocateArray(IAllocator& a, std::size_t count, MemTag tag, std::size_t align = kBufferAlignment) noexcept {
  if (count == 0u || count > (~static_cast<std::size_t>(0)) / sizeof(T)) return nullptr;
  void* p = a.Allocate(count * sizeof(T), ArrayAlignment<T>(align), tag);
  if (p == nullptr) return nullptr;
  T* t = static_cast<T*>(p);
  for (std::size_t i = 0; i < count; ++i) ::new (static_cast<void*>(t + i)) T();
  return t;
}

// Destroys and frees an array from AllocateArray. p may be nullptr.
template <class T>
void FreeArray(IAllocator& a, T* p, std::size_t count, MemTag tag) noexcept {
  if (p == nullptr) return;
  if constexpr (!std::is_trivially_destructible_v<T>) {
    for (std::size_t i = 0; i < count; ++i) p[i].~T();
  }
  a.Free(p, tag);
}

// Constructs one object in allocator memory / destroys it.
template <class T, class... Args>
T* New(IAllocator& a, MemTag tag, Args&&... args) noexcept {
  void* p = a.Allocate(sizeof(T), ArrayAlignment<T>(kMinAlignment), tag);
  if (p == nullptr) return nullptr;
  return ::new (p) T(std::forward<Args>(args)...);
}
template <class T>
void Delete(IAllocator& a, T* p, MemTag tag) noexcept {
  if (p == nullptr) return;
  p->~T();
  a.Free(p, tag);
}

// Owning, move-only array with 64-byte aligned storage (3.4 "64バイト境界").
// Allocate/Release are non-RT; element access is RT-safe.
template <class T>
class AlignedBuffer {
 public:
  AlignedBuffer() noexcept = default;
  ~AlignedBuffer() { Release(); }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& o) noexcept { *this = std::move(o); }
  AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
    if (this != &o) {
      Release();
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0u);
      alloc_ = std::exchange(o.alloc_, nullptr);
      tag_ = o.tag_;
    }
    return *this;
  }

  // Replaces the contents with `count` value-initialised elements. false on failure.
  bool Allocate(IAllocator& a, std::size_t count, MemTag tag) noexcept {
    Release();
    data_ = AllocateArray<T>(a, count, tag);
    if (data_ == nullptr) return false;
    size_ = count;
    alloc_ = &a;
    tag_ = tag;
    return true;
  }
  void Release() noexcept {
    if (data_ != nullptr) FreeArray(*alloc_, data_, size_, tag_);
    data_ = nullptr;
    size_ = 0;
    alloc_ = nullptr;
  }

  T* data() noexcept { return data_; }
  const T* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0u; }
  T& operator[](std::size_t i) noexcept { return data_[i]; }
  const T& operator[](std::size_t i) const noexcept { return data_[i]; }
  T* begin() noexcept { return data_; }
  T* end() noexcept { return data_ + size_; }

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

}  // namespace snd::base

#endif  // SND_BASE_ALLOCATOR_HPP
