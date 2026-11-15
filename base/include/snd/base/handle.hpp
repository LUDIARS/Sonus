// Handles: 64 bit = generation (high 32) + slot (low 32) (design.md 3.1, CONTRACTS.md D-05).
//
// Generation 0 is never issued, so the value 0 is the invalid handle. The
// contract type is snd::HandleRef (snd/internal/common.hpp); helpers here
// convert and advance generations.
//
// HandleAllocator is the game-side slot table of 2.1 "ハンドルの払い出し":
// any registered thread may Acquire() concurrently (lock-free) and gets a slot
// plus a fresh generation immediately; Update() Release()s the slot after the
// audio side reported its end (notify.hpp). Stale handles are detected with
// IsCurrent() (D-29: "記録時に古いと判れば ... SND_ERR_INVALID_HANDLE").
#ifndef SND_BASE_HANDLE_HPP
#define SND_BASE_HANDLE_HPP

#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/pool.hpp"
#include "snd/internal/common.hpp"
#include "snd/types.h"

namespace snd::base {

constexpr std::uint64_t MakeHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
  return (static_cast<std::uint64_t>(generation) << 32) | slot;
}
constexpr std::uint32_t HandleSlot(std::uint64_t h) noexcept { return static_cast<std::uint32_t>(h); }
constexpr std::uint32_t HandleGeneration(std::uint64_t h) noexcept { return static_cast<std::uint32_t>(h >> 32); }
constexpr bool IsValidHandle(std::uint64_t h) noexcept { return HandleGeneration(h) != 0u; }

// Next generation after g, skipping 0 on wrap.
constexpr std::uint32_t NextGeneration(std::uint32_t g) noexcept { return g + 1u == 0u ? 1u : g + 1u; }

static_assert(MakeHandle(5, 7) == SND_HANDLE_MAKE(5, 7));
static_assert(HandleRef::FromValue(MakeHandle(5, 7)).slot == 5u);
static_assert(NextGeneration(0xFFFFFFFFu) == 1u);

// Lock-free slot + generation allocator.
//  Acquire  any thread, RT-safe, lock-free. Returns HandleRef{kInvalidSlot, 0} when full.
//  Release  any thread, RT-safe. Bumps the generation so old handles go stale.
//  IsCurrent any thread, RT-safe.
// Slots are reissued LIFO; the order is deterministic for a single thread.
class HandleAllocator {
 public:
  HandleAllocator() noexcept = default;
  ~HandleAllocator() { Destroy(); }
  HandleAllocator(const HandleAllocator&) = delete;
  HandleAllocator& operator=(const HandleAllocator&) = delete;

  // Non-RT. Slots [0, capacity); generations start at 1.
  bool Init(IAllocator& a, std::uint32_t capacity, MemTag tag = MemTag::Tables) noexcept {
    Destroy();
    if (!free_.Init(a, capacity, tag)) return false;
    generation_ = AllocateArray<std::atomic<std::uint32_t>>(a, capacity, tag);
    if (generation_ == nullptr) {
      free_.Destroy();
      return false;
    }
    for (std::uint32_t i = 0; i < capacity; ++i) generation_[i].store(1u, std::memory_order_relaxed);
    alloc_ = &a;
    tag_ = tag;
    capacity_ = capacity;
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) FreeArray(*alloc_, generation_, capacity_, tag_);
    free_.Destroy();
    generation_ = nullptr;
    alloc_ = nullptr;
    capacity_ = 0;
  }

  HandleRef Acquire() noexcept {
    const std::uint32_t slot = free_.Pop();
    if (slot == kInvalidIndex) return HandleRef{kInvalidSlot, 0u};
    // The bit 31 of the stored generation marks "live" so IsCurrent can reject
    // released slots whose generation was not reissued yet.
    const std::uint32_t g = generation_[slot].load(std::memory_order_relaxed);
    generation_[slot].store(g | kLiveBit, std::memory_order_release);
    return HandleRef{slot, g};
  }
  // `h` must be the live handle of its slot; stale or invalid handles are ignored (returns false).
  bool Release(HandleRef h) noexcept {
    if (h.slot >= capacity_ || h.generation == 0u) return false;
    std::uint32_t expected = h.generation | kLiveBit;
    std::uint32_t next = NextGen(h.generation);
    if (!generation_[h.slot].compare_exchange_strong(expected, next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
      return false;
    }
    free_.Push(h.slot);
    return true;
  }
  bool IsCurrent(HandleRef h) const noexcept {
    return h.slot < capacity_ && h.generation != 0u &&
           generation_[h.slot].load(std::memory_order_acquire) == (h.generation | kLiveBit);
  }
  std::uint32_t Capacity() const noexcept { return capacity_; }
  std::uint32_t Available() const noexcept { return free_.Available(); }

  // Generations issued by this allocator stay below 2^31 (bit 31 = live flag).
  static constexpr std::uint32_t kLiveBit = 0x80000000u;

 private:
  static std::uint32_t NextGen(std::uint32_t g) noexcept {
    const std::uint32_t n = (g + 1u) & ~kLiveBit;
    return n == 0u ? 1u : n;
  }
  ConcurrentIndexFreeList free_;
  std::atomic<std::uint32_t>* generation_ = nullptr;
  std::uint32_t capacity_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Tables;
};

}  // namespace snd::base

#endif  // SND_BASE_HANDLE_HPP
