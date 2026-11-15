// Fixed-width inline arrays with overflow chains of 16-entry blocks
// (design.md 3.1 "可変長の子": Parameter values, sends).
//
//   ChainPool<T>          shared pool of blocks of kBlockEntries (16) T.
//   InlineChain<T, N>     N inline entries (e.g. 8) + a singly linked chain of
//                         pool blocks for the overflow. Element order is
//                         insertion order except after RemoveSwap.
//
// Single-thread (the owning system). Init/Destroy non-RT; everything else is
// RT-safe and never allocates: PushBack returns false when the pool is
// exhausted (caller drops/counts, principle 2).
#ifndef SND_BASE_CHAIN_HPP
#define SND_BASE_CHAIN_HPP

#include <cstdint>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/pool.hpp"
#include "snd/internal/common.hpp"

namespace snd::base {

template <class T, std::uint32_t B = kParamOverflowBlockEntries>
class ChainPool {
  static_assert(std::is_trivially_copyable_v<T>, "chain entries are PODs");

 public:
  static constexpr std::uint32_t kBlockEntries = B;

  ChainPool() noexcept = default;
  ~ChainPool() { Destroy(); }
  ChainPool(const ChainPool&) = delete;
  ChainPool& operator=(const ChainPool&) = delete;

  bool Init(IAllocator& a, std::uint32_t blocks, MemTag tag = MemTag::Tables) noexcept {
    Destroy();
    std::size_t entries = 0;
    if (!CheckedMul(blocks, B, &entries)) return false;  // AllocateArray checks entries * sizeof(T)
    if (!free_.Init(a, blocks, tag)) return false;
    items_ = AllocateArray<T>(a, static_cast<std::size_t>(blocks) * B, tag);
    next_ = AllocateArray<std::uint32_t>(a, blocks, tag);
    if (items_ == nullptr || next_ == nullptr) {
      FreeArray(a, items_, static_cast<std::size_t>(blocks) * B, tag);
      FreeArray(a, next_, blocks, tag);
      items_ = nullptr;
      next_ = nullptr;
      free_.Destroy();
      return false;
    }
    alloc_ = &a;
    tag_ = tag;
    blocks_ = blocks;
    return true;
  }
  void Destroy() noexcept {
    if (alloc_ != nullptr) {
      FreeArray(*alloc_, items_, static_cast<std::size_t>(blocks_) * B, tag_);
      FreeArray(*alloc_, next_, blocks_, tag_);
    }
    free_.Destroy();
    items_ = nullptr;
    next_ = nullptr;
    alloc_ = nullptr;
    blocks_ = 0;
  }

  // kInvalidIndex when exhausted. The new block's next link is kInvalidIndex.
  std::uint32_t AcquireBlock() noexcept {
    const std::uint32_t b = free_.Pop();
    if (b != kInvalidIndex) next_[b] = kInvalidIndex;
    return b;
  }
  void ReleaseBlock(std::uint32_t block) noexcept { free_.Push(block); }
  T* Items(std::uint32_t block) noexcept { return items_ + static_cast<std::size_t>(block) * B; }
  const T* Items(std::uint32_t block) const noexcept { return items_ + static_cast<std::size_t>(block) * B; }
  std::uint32_t& Next(std::uint32_t block) noexcept { return next_[block]; }
  std::uint32_t Next(std::uint32_t block) const noexcept { return next_[block]; }
  std::uint32_t Available() const noexcept { return free_.Available(); }
  std::uint32_t Blocks() const noexcept { return blocks_; }

 private:
  IndexFreeList free_;
  T* items_ = nullptr;
  std::uint32_t* next_ = nullptr;
  std::uint32_t blocks_ = 0;
  IAllocator* alloc_ = nullptr;
  MemTag tag_ = MemTag::Tables;
};

template <class T, std::uint32_t N = kParamInlineSlots, std::uint32_t B = kParamOverflowBlockEntries>
struct InlineChain {
  using Pool = ChainPool<T, B>;
  static_assert(N > 0u);

  std::uint32_t size = 0;
  std::uint32_t head = kInvalidIndex;  // first overflow block
  T inlineItems[N];

  std::uint32_t Size() const noexcept { return size; }
  bool Empty() const noexcept { return size == 0u; }

  // nullptr if i >= Size(). O(i / B) for overflow entries.
  T* At(Pool& pool, std::uint32_t i) noexcept { return const_cast<T*>(AtImpl(pool, i)); }
  const T* At(const Pool& pool, std::uint32_t i) const noexcept { return AtImpl(pool, i); }

  // false when the pool has no block left (the value is not stored).
  bool PushBack(Pool& pool, const T& v) noexcept {
    if (size < N) {
      inlineItems[size++] = v;
      return true;
    }
    const std::uint32_t o = size - N;
    const std::uint32_t k = o / B;
    if (o % B == 0u) {  // needs a new block at chain position k
      const std::uint32_t nb = pool.AcquireBlock();
      if (nb == kInvalidIndex) return false;
      if (k == 0u) {
        head = nb;
      } else {
        pool.Next(BlockAt(pool, k - 1u)) = nb;
      }
    }
    pool.Items(BlockAt(pool, k))[o % B] = v;
    ++size;
    return true;
  }

  // Moves the last entry into i and shrinks; frees a trailing block that became empty.
  void RemoveSwap(Pool& pool, std::uint32_t i) noexcept {
    if (i >= size) return;
    const std::uint32_t last = size - 1u;
    if (i != last) *At(pool, i) = *At(pool, last);
    size = last;
    if (last >= N && (last - N) % B == 0u) {  // the last block is now empty
      const std::uint32_t k = (last - N) / B;
      const std::uint32_t b = BlockAt(pool, k);
      if (k == 0u) {
        head = kInvalidIndex;
      } else {
        pool.Next(BlockAt(pool, k - 1u)) = kInvalidIndex;
      }
      pool.ReleaseBlock(b);
    }
  }

  // Returns every block to the pool.
  void Clear(Pool& pool) noexcept {
    std::uint32_t b = head;
    while (b != kInvalidIndex) {
      const std::uint32_t n = pool.Next(b);
      pool.ReleaseBlock(b);
      b = n;
    }
    head = kInvalidIndex;
    size = 0;
  }

  // f(T&) for every entry in order.
  template <class F>
  void ForEach(Pool& pool, F&& f) noexcept {
    const std::uint32_t inl = size < N ? size : N;
    for (std::uint32_t i = 0; i < inl; ++i) f(inlineItems[i]);
    std::uint32_t remaining = size - inl;
    std::uint32_t b = head;
    while (remaining > 0u && b != kInvalidIndex) {
      const std::uint32_t n = remaining < B ? remaining : B;
      T* items = pool.Items(b);
      for (std::uint32_t i = 0; i < n; ++i) f(items[i]);
      remaining -= n;
      b = pool.Next(b);
    }
  }

 private:
  std::uint32_t BlockAt(const Pool& pool, std::uint32_t k) const noexcept {
    std::uint32_t b = head;
    for (std::uint32_t j = 0; j < k && b != kInvalidIndex; ++j) b = pool.Next(b);
    return b;
  }
  const T* AtImpl(const Pool& pool, std::uint32_t i) const noexcept {
    if (i >= size) return nullptr;
    if (i < N) return &inlineItems[i];
    const std::uint32_t o = i - N;
    return pool.Items(BlockAt(pool, o / B)) + (o % B);
  }
};

}  // namespace snd::base

#endif  // SND_BASE_CHAIN_HPP
