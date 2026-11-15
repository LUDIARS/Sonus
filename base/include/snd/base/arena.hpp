// Linear (bump) arenas: frame arena and per-thread arenas (design.md 3.3).
//
//  - The frame arena is the scratch of the serial section (logic) of one
//    block; it is Reset() at the start of every block.
//  - Thread arenas are the scratch of the parallel section (render): one per
//    RT thread (audio thread + RT workers), never shared.
//
// Init/Destroy allocate from an IAllocator (non-RT). Allocate/Reset/Mark/Rewind
// are RT-safe and wait-free but NOT thread-safe: an arena has one user thread.
// Allocation failure returns nullptr and counts an overflow (3.3: "超過は
// アサート＋負荷LOD" — the caller decides; the arena never grows).
#ifndef SND_BASE_ARENA_HPP
#define SND_BASE_ARENA_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/internal/common.hpp"

namespace snd::base {

class LinearArena {
 public:
  LinearArena() noexcept = default;
  ~LinearArena() { Destroy(); }
  LinearArena(const LinearArena&) = delete;
  LinearArena& operator=(const LinearArena&) = delete;

  // Owning: allocates `bytes` (64-byte aligned) from `a`. Non-RT. false on failure.
  bool Init(IAllocator& a, std::size_t bytes, MemTag tag) noexcept {
    Destroy();
    if (bytes == 0u) return false;
    void* p = a.Allocate(bytes, kBufferAlignment, tag);
    if (p == nullptr) return false;
    Bind(p, bytes);
    alloc_ = &a;
    tag_ = tag;
    return true;
  }
  // Non-owning: uses caller memory. RT-safe.
  void Bind(void* memory, std::size_t bytes) noexcept {
    base_ = static_cast<std::uint8_t*>(memory);
    capacity_ = memory != nullptr ? bytes : 0u;
    used_ = 0;
    highWater_ = 0;
    overflows_ = 0;
  }
  // Frees owned memory (non-RT). Safe to call twice.
  void Destroy() noexcept {
    if (alloc_ != nullptr && base_ != nullptr) alloc_->Free(base_, tag_);
    alloc_ = nullptr;
    base_ = nullptr;
    capacity_ = used_ = highWater_ = 0;
  }

  // Bump allocation, `align` a power of two. nullptr when exhausted (counted).
  void* Allocate(std::size_t bytes, std::size_t align = kBufferAlignment) noexcept {
    const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t at = (b + used_ + (align - 1u)) & ~static_cast<std::uintptr_t>(align - 1u);
    const std::size_t start = static_cast<std::size_t>(at - b);
    if (base_ == nullptr || start > capacity_ || bytes > capacity_ - start) {
      ++overflows_;
      return nullptr;
    }
    used_ = start + bytes;
    if (used_ > highWater_) highWater_ = used_;
    return base_ + start;
  }
  template <class T>
  T* AllocateArray(std::size_t count, std::size_t align = alignof(T) > 16 ? alignof(T) : 16) noexcept {
    if (count > (~static_cast<std::size_t>(0)) / sizeof(T)) {
      ++overflows_;
      return nullptr;
    }
    return static_cast<T*>(Allocate(count * sizeof(T), align));
  }
  // Same, zero-filled.
  void* AllocateZeroed(std::size_t bytes, std::size_t align = kBufferAlignment) noexcept {
    void* p = Allocate(bytes, align);
    if (p != nullptr) std::memset(p, 0, bytes);
    return p;
  }

  void Reset() noexcept { used_ = 0; }
  std::size_t Mark() const noexcept { return used_; }
  void Rewind(std::size_t mark) noexcept {
    if (mark <= used_) used_ = mark;
  }

  std::uint8_t* data() const noexcept { return base_; }
  std::size_t Capacity() const noexcept { return capacity_; }
  std::size_t Used() const noexcept { return used_; }
  std::size_t Remaining() const noexcept { return capacity_ - used_; }
  std::size_t HighWater() const noexcept { return highWater_; }
  std::uint32_t Overflows() const noexcept { return overflows_; }
  void ClearStats() noexcept {
    highWater_ = used_;
    overflows_ = 0;
  }

 private:
  std::uint8_t* base_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t used_ = 0;
  std::size_t highWater_ = 0;
  std::uint32_t overflows_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Misc;
};

// One arena per thread, carved from a single allocation (3.3 スレッド別アリーナ:
// (RT workers + audio thread) x fixed size). Each arena starts on its own
// cache line, so threads never share a line. Arena(i) for i < Count().
class ThreadArenaSet {
 public:
  ThreadArenaSet() noexcept = default;
  ~ThreadArenaSet() { Destroy(); }
  ThreadArenaSet(const ThreadArenaSet&) = delete;
  ThreadArenaSet& operator=(const ThreadArenaSet&) = delete;

  // Non-RT. `bytesPerThread` is rounded up to 64. Tag: MemTag::ThreadArena.
  bool Init(IAllocator& a, std::uint32_t threads, std::size_t bytesPerThread,
            MemTag tag = MemTag::ThreadArena) noexcept {
    Destroy();
    if (threads == 0u || bytesPerThread == 0u) return false;
    const std::size_t per = AlignUp(bytesPerThread, kBufferAlignment);
    arenas_ = AllocateArray<LinearArena>(a, threads, tag);
    if (arenas_ == nullptr) return false;
    memory_ = static_cast<std::uint8_t*>(a.Allocate(per * threads, kBufferAlignment, tag));
    if (memory_ == nullptr) {
      FreeArray(a, arenas_, threads, tag);
      arenas_ = nullptr;
      return false;
    }
    for (std::uint32_t i = 0; i < threads; ++i) arenas_[i].Bind(memory_ + per * i, per);
    count_ = threads;
    alloc_ = &a;
    tag_ = tag;
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) {
      FreeArray(*alloc_, arenas_, count_, tag_);
      alloc_->Free(memory_, tag_);
    }
    arenas_ = nullptr;
    memory_ = nullptr;
    count_ = 0;
    alloc_ = nullptr;
  }

  std::uint32_t Count() const noexcept { return count_; }
  LinearArena& Arena(std::uint32_t i) noexcept { return arenas_[i]; }
  const LinearArena& Arena(std::uint32_t i) const noexcept { return arenas_[i]; }

 private:
  LinearArena* arenas_ = nullptr;
  std::uint8_t* memory_ = nullptr;
  std::uint32_t count_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::ThreadArena;
};

}  // namespace snd::base

#endif  // SND_BASE_ARENA_HPP
