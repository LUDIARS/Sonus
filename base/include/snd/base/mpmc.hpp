// Bounded lock-free multi-producer / multi-consumer queue (Vyukov's array
// queue with per-cell sequence numbers).
//
// Used where several RT threads post to a non-RT thread (CONTRACTS.md D-08:
// SourceServices::postJob / postIo from the audio thread and RT workers; the
// lock-free log). TryPush / TryPop never block, allocate or lock and are
// RT-safe; they retry a CAS only under contention with another producer
// (resp. consumer). They fail instead of waiting when the queue is full/empty.
// FIFO per producer; T must be trivially copyable.
#ifndef SND_BASE_MPMC_HPP
#define SND_BASE_MPMC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"

namespace snd::base {

template <class T>
class MpmcRing {
  static_assert(std::is_trivially_copyable_v<T>, "MpmcRing stores PODs");

 public:
  struct Cell {
    std::atomic<std::uint64_t> seq{0};
    T value;
  };

  MpmcRing() noexcept = default;
  ~MpmcRing() { Destroy(); }
  MpmcRing(const MpmcRing&) = delete;
  MpmcRing& operator=(const MpmcRing&) = delete;

  // Non-RT. capacity rounded up to a power of two (>= 2).
  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag) noexcept {
    Destroy();
    const std::uint64_t cap = NextPowerOfTwo(capacity < 2u ? 2u : capacity);
    if (cap > 0x80000000u) return false;
    cells_ = AllocateArray<Cell>(a, static_cast<std::size_t>(cap), tag);
    if (cells_ == nullptr) return false;
    alloc_ = &a;
    tag_ = tag;
    capacity_ = static_cast<std::uint32_t>(cap);
    mask_ = capacity_ - 1u;
    for (std::uint32_t i = 0; i < capacity_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    enqueue_.value.store(0, std::memory_order_relaxed);
    dequeue_.value.store(0, std::memory_order_relaxed);
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) FreeArray(*alloc_, cells_, capacity_, tag_);
    cells_ = nullptr;
    alloc_ = nullptr;
    capacity_ = 0;
  }

  // Any thread. false when full.
  bool TryPush(const T& v) noexcept {
    std::uint64_t pos = enqueue_.value.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
      const std::int64_t diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
      if (diff == 0) {
        if (enqueue_.value.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
          c.value = v;
          c.seq.store(pos + 1u, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enqueue_.value.load(std::memory_order_relaxed);
      }
    }
  }
  // Any thread. false when empty.
  bool TryPop(T& out) noexcept {
    std::uint64_t pos = dequeue_.value.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const std::uint64_t seq = c.seq.load(std::memory_order_acquire);
      const std::int64_t diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1u);
      if (diff == 0) {
        if (dequeue_.value.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
          out = c.value;
          c.seq.store(pos + mask_ + 1u, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_.value.load(std::memory_order_relaxed);
      }
    }
  }

  std::uint32_t SizeApprox() const noexcept {
    const std::uint64_t e = enqueue_.value.load(std::memory_order_relaxed);
    const std::uint64_t d = dequeue_.value.load(std::memory_order_relaxed);
    return e >= d ? static_cast<std::uint32_t>(e - d) : 0u;
  }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  CachePadded<std::atomic<std::uint64_t>> enqueue_;
  CachePadded<std::atomic<std::uint64_t>> dequeue_;
  Cell* cells_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t mask_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

}  // namespace snd::base

#endif  // SND_BASE_MPMC_HPP
