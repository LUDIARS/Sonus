// Free lists and fixed-size block pools (design.md 3.1, 3.3).
//
// IndexFreeList            single-thread LIFO of indices [0, capacity).
// ConcurrentIndexFreeList  lock-free MPMC LIFO (Treiber stack with a 32-bit
//                          ABA tag). Wait-free? No: Pop/Push retry a CAS under
//                          contention, but never block, allocate or lock.
// BlockPool / ConcurrentBlockPool
//                          fixed-size blocks carved from one allocation, with
//                          the matching free list. Blocks are addressed by
//                          index (stable, cheap to store in tables) or pointer.
//
// Init/Destroy are non-RT (allocate through IAllocator). Every other member
// is RT-safe. All lists start FULL (every index free) after Init.
#ifndef SND_BASE_POOL_HPP
#define SND_BASE_POOL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/internal/common.hpp"

namespace snd::base {

inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

// Single-thread free list. Pop order after Init is 0, 1, 2, ... (deterministic).
class IndexFreeList {
 public:
  IndexFreeList() noexcept = default;
  ~IndexFreeList() { Destroy(); }
  IndexFreeList(const IndexFreeList&) = delete;
  IndexFreeList& operator=(const IndexFreeList&) = delete;

  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag) noexcept {
    Destroy();
    if (capacity == 0u || capacity == kInvalidIndex) return false;
    stack_ = AllocateArray<std::uint32_t>(a, capacity, tag);
    if (stack_ == nullptr) return false;
    alloc_ = &a;
    tag_ = tag;
    capacity_ = capacity;
    Reset();
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) FreeArray(*alloc_, stack_, capacity_, tag_);
    stack_ = nullptr;
    alloc_ = nullptr;
    capacity_ = size_ = 0;
  }
  // Marks every index free again. RT-safe (O(capacity)).
  void Reset() noexcept {
    for (std::uint32_t i = 0; i < capacity_; ++i) stack_[i] = capacity_ - 1u - i;
    size_ = capacity_;
  }
  // kInvalidIndex when empty.
  std::uint32_t Pop() noexcept { return size_ == 0u ? kInvalidIndex : stack_[--size_]; }
  // Index must have come from Pop and not been pushed since (not checked).
  void Push(std::uint32_t index) noexcept {
    if (size_ < capacity_) stack_[size_++] = index;
  }
  std::uint32_t Available() const noexcept { return size_; }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  std::uint32_t* stack_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t size_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

// Lock-free MPMC free list. head = (tag << 32) | index; next[] is atomic so
// concurrent readers of a node being popped are well defined (TSan clean).
class ConcurrentIndexFreeList {
 public:
  ConcurrentIndexFreeList() noexcept = default;
  ~ConcurrentIndexFreeList() { Destroy(); }
  ConcurrentIndexFreeList(const ConcurrentIndexFreeList&) = delete;
  ConcurrentIndexFreeList& operator=(const ConcurrentIndexFreeList&) = delete;

  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag) noexcept {
    Destroy();
    if (capacity == 0u || capacity == kInvalidIndex) return false;
    next_ = AllocateArray<std::atomic<std::uint32_t>>(a, capacity, tag);
    if (next_ == nullptr) return false;
    alloc_ = &a;
    tag_ = tag;
    capacity_ = capacity;
    Reset();
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) FreeArray(*alloc_, next_, capacity_, tag_);
    next_ = nullptr;
    alloc_ = nullptr;
    capacity_ = 0;
  }
  // Not thread-safe: only while no other thread uses the list.
  void Reset() noexcept {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      next_[i].store(i + 1u < capacity_ ? i + 1u : kInvalidIndex, std::memory_order_relaxed);
    }
    head_.value.store(capacity_ > 0u ? 0u : kInvalidIndex, std::memory_order_release);
    available_.value.store(capacity_, std::memory_order_relaxed);
  }
  // Any thread. kInvalidIndex when empty.
  std::uint32_t Pop() noexcept {
    std::uint64_t head = head_.value.load(std::memory_order_acquire);
    for (;;) {
      const std::uint32_t index = static_cast<std::uint32_t>(head);
      if (index == kInvalidIndex) return kInvalidIndex;
      const std::uint32_t next = next_[index].load(std::memory_order_relaxed);
      const std::uint64_t tag = (head >> 32) + 1u;
      if (head_.value.compare_exchange_weak(head, (tag << 32) | next, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        available_.value.fetch_sub(1u, std::memory_order_relaxed);
        return index;
      }
    }
  }
  // Any thread. `index` must be owned by the caller (popped, not yet pushed).
  void Push(std::uint32_t index) noexcept {
    std::uint64_t head = head_.value.load(std::memory_order_relaxed);
    for (;;) {
      next_[index].store(static_cast<std::uint32_t>(head), std::memory_order_relaxed);
      const std::uint64_t tag = (head >> 32) + 1u;
      if (head_.value.compare_exchange_weak(head, (tag << 32) | index, std::memory_order_release,
                                            std::memory_order_relaxed)) {
        available_.value.fetch_add(1u, std::memory_order_relaxed);
        return;
      }
    }
  }
  // Approximate under concurrency.
  std::uint32_t Available() const noexcept { return available_.value.load(std::memory_order_relaxed); }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  CachePadded<std::atomic<std::uint64_t>> head_;
  CachePadded<std::atomic<std::uint32_t>> available_;
  std::atomic<std::uint32_t>* next_ = nullptr;
  std::uint32_t capacity_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

// Fixed-size blocks over one allocation. FreeList is IndexFreeList (one
// thread) or ConcurrentIndexFreeList (any thread). Block memory is aligned to
// `align` (default 64) and blockBytes is rounded up to a multiple of it.
template <class FreeList>
class BasicBlockPool {
 public:
  BasicBlockPool() noexcept = default;
  ~BasicBlockPool() { Destroy(); }
  BasicBlockPool(const BasicBlockPool&) = delete;
  BasicBlockPool& operator=(const BasicBlockPool&) = delete;

  bool Init(IAllocator& a, std::size_t blockBytes, std::uint32_t blockCount, MemTag tag,
            std::size_t align = kBufferAlignment) noexcept {
    Destroy();
    if (blockBytes == 0u || blockCount == 0u || !IsPowerOfTwo(align)) return false;
    if (align < kMinAlignment) align = kMinAlignment;
    stride_ = AlignUp(blockBytes, align);
    if (!free_.Init(a, blockCount, tag)) return false;
    memory_ = static_cast<std::uint8_t*>(a.Allocate(stride_ * blockCount, align, tag));
    if (memory_ == nullptr) {
      free_.Destroy();
      return false;
    }
    alloc_ = &a;
    tag_ = tag;
    count_ = blockCount;
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) alloc_->Free(memory_, tag_);
    free_.Destroy();
    memory_ = nullptr;
    alloc_ = nullptr;
    count_ = 0;
    stride_ = 0;
  }

  // kInvalidIndex when exhausted.
  std::uint32_t AcquireIndex() noexcept { return free_.Pop(); }
  void ReleaseIndex(std::uint32_t index) noexcept { free_.Push(index); }
  // nullptr when exhausted.
  void* Acquire() noexcept {
    const std::uint32_t i = free_.Pop();
    return i == kInvalidIndex ? nullptr : memory_ + stride_ * i;
  }
  void Release(void* block) noexcept {
    if (block != nullptr) free_.Push(IndexOf(block));
  }

  void* Block(std::uint32_t index) const noexcept { return memory_ + stride_ * index; }
  std::uint32_t IndexOf(const void* block) const noexcept {
    return static_cast<std::uint32_t>((static_cast<const std::uint8_t*>(block) - memory_) / stride_);
  }
  bool Owns(const void* p) const noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return memory_ != nullptr && b >= memory_ && b < memory_ + stride_ * count_;
  }
  std::size_t BlockBytes() const noexcept { return stride_; }
  std::uint32_t BlockCount() const noexcept { return count_; }
  std::uint32_t Available() const noexcept { return free_.Available(); }

 private:
  FreeList free_;
  std::uint8_t* memory_ = nullptr;
  std::size_t stride_ = 0;
  std::uint32_t count_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

using BlockPool = BasicBlockPool<IndexFreeList>;
using ConcurrentBlockPool = BasicBlockPool<ConcurrentIndexFreeList>;

}  // namespace snd::base

#endif  // SND_BASE_POOL_HPP
