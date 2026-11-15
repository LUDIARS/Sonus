// Bounded single-producer / single-consumer ring (design.md 2.1).
//
// SpscRing<T>: lock-free, wait-free TryPush (producer thread) and TryPop
// (consumer thread); no allocation after Init. T must be trivially copyable.
// Capacity is a power of two. Producer and consumer indices live on separate
// cache lines and each side caches the other's index.
//
// ArenaCycle: the command-arena loop of 2.1 built from two rings:
//
//     game thread (producer)                    audio thread (consumer)
//     Current() -> record into arena            Take(&i)   -> apply arena i
//     Publish() -> push Current() to "publish", Return(i) -> push i to "return"
//                  pop a free arena from "return"
//
//   Arena indices are [0, count). Initially the producer holds arena 0 and
//   1..count-1 are free. Publish() fails (returns false, nothing changes) when
//   no arena has been returned yet — the producer keeps appending to its
//   current arena (2.1: Update() never waits for the audio thread). Payload
//   memory and headers are the caller's (core, commands.hpp layout); the
//   release/acquire pair of the rings orders the arena bytes.
#ifndef SND_BASE_SPSC_HPP
#define SND_BASE_SPSC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"

namespace snd::base {

template <class T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "SpscRing stores PODs");

 public:
  SpscRing() noexcept = default;
  ~SpscRing() { Destroy(); }
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Non-RT. capacity is rounded up to a power of two (>= 2).
  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag) noexcept {
    Destroy();
    const std::uint64_t cap = NextPowerOfTwo(capacity < 2u ? 2u : capacity);
    if (cap > 0x80000000u) return false;
    T* items = AllocateArray<T>(a, static_cast<std::size_t>(cap), tag);
    if (items == nullptr) return false;
    Bind(items, static_cast<std::uint32_t>(cap));
    alloc_ = &a;
    tag_ = tag;
    return true;
  }
  // Non-owning: `capacity` must be a power of two. Not thread-safe (setup only).
  void Bind(T* storage, std::uint32_t capacity) noexcept {
    items_ = storage;
    capacity_ = capacity;
    mask_ = capacity - 1u;
    write_.value.store(0, std::memory_order_relaxed);
    read_.value.store(0, std::memory_order_relaxed);
    cachedRead_ = 0;
    cachedWrite_ = 0;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) FreeArray(*alloc_, items_, capacity_, tag_);
    items_ = nullptr;
    alloc_ = nullptr;
    capacity_ = 0;
  }

  // Producer thread. false when full.
  bool TryPush(const T& v) noexcept {
    const std::uint64_t w = write_.value.load(std::memory_order_relaxed);
    if (w - cachedRead_ >= capacity_) {
      cachedRead_ = read_.value.load(std::memory_order_acquire);
      if (w - cachedRead_ >= capacity_) return false;
    }
    items_[w & mask_] = v;
    write_.value.store(w + 1u, std::memory_order_release);
    return true;
  }
  // Consumer thread. false when empty.
  bool TryPop(T& out) noexcept {
    const std::uint64_t r = read_.value.load(std::memory_order_relaxed);
    if (r == cachedWrite_) {
      cachedWrite_ = write_.value.load(std::memory_order_acquire);
      if (r == cachedWrite_) return false;
    }
    out = items_[r & mask_];
    read_.value.store(r + 1u, std::memory_order_release);
    return true;
  }
  // Consumer thread: oldest element without removing it (nullptr if empty).
  const T* Peek() noexcept {
    const std::uint64_t r = read_.value.load(std::memory_order_relaxed);
    if (r == cachedWrite_) {
      cachedWrite_ = write_.value.load(std::memory_order_acquire);
      if (r == cachedWrite_) return nullptr;
    }
    return &items_[r & mask_];
  }

  // Any thread; exact only when both sides are quiescent.
  std::uint32_t SizeApprox() const noexcept {
    const std::uint64_t w = write_.value.load(std::memory_order_acquire);
    const std::uint64_t r = read_.value.load(std::memory_order_acquire);
    return w >= r ? static_cast<std::uint32_t>(w - r) : 0u;
  }
  bool EmptyApprox() const noexcept { return SizeApprox() == 0u; }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  CachePadded<std::atomic<std::uint64_t>> write_;  // producer
  CachePadded<std::atomic<std::uint64_t>> read_;   // consumer
  alignas(kCacheLine) std::uint64_t cachedRead_ = 0;  // producer private
  alignas(kCacheLine) std::uint64_t cachedWrite_ = 0;  // consumer private
  T* items_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t mask_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

class ArenaCycle {
 public:
  ArenaCycle() noexcept = default;
  ArenaCycle(const ArenaCycle&) = delete;
  ArenaCycle& operator=(const ArenaCycle&) = delete;

  // Non-RT. count >= 2 (Config.commands.arenasPerThread, default 3).
  bool Init(IAllocator& a, std::uint32_t count, MemTag tag = MemTag::CommandArena) noexcept {
    if (count < 2u) return false;
    if (!publish_.Init(a, count, tag) || !return_.Init(a, count, tag)) {
      Destroy();
      return false;
    }
    count_ = count;
    current_ = 0;
    for (std::uint32_t i = 1; i < count; ++i) return_.TryPush(i);
    return true;
  }
  void Destroy() noexcept {
    publish_.Destroy();
    return_.Destroy();
    count_ = 0;
  }

  // --- producer (recording thread) ---
  std::uint32_t Current() const noexcept { return current_; }
  // Publishes Current() and switches to a returned arena. false (nothing
  // published) when every other arena is still with the consumer.
  bool Publish() noexcept {
    std::uint32_t next = 0;
    if (!return_.TryPop(next)) return false;
    publish_.TryPush(current_);  // cannot fail: at most count-1 arenas are in flight
    current_ = next;
    return true;
  }

  // --- consumer (audio thread) ---
  // Next published arena in publish order. false when none.
  bool Take(std::uint32_t& index) noexcept { return publish_.TryPop(index); }
  // Gives an applied arena back to the producer.
  void Return(std::uint32_t index) noexcept { return_.TryPush(index); }

  std::uint32_t Count() const noexcept { return count_; }

 private:
  SpscRing<std::uint32_t> publish_;
  SpscRing<std::uint32_t> return_;
  std::uint32_t current_ = 0;  // producer private
  std::uint32_t count_ = 0;
};

}  // namespace snd::base

#endif  // SND_BASE_SPSC_HPP
