// Sparse-set row bookkeeping for schema::TableRows (design.md 3.1).
//
//   dense [0, count)      live rows; columns are indexed by DENSE position
//   denseToSlot[dense]    stable slot of a row
//   slotToDense[slot]     dense position, kInvalidSlot = free slot
//   generation[slot]      generation of the live row / of the last row in the
//                         slot (kept on Remove so stale handles stay stale)
//
// Removal swaps the last dense row into the hole ("末尾入れ替え削除"). The
// caller owns the column arrays, so Remove returns which dense row moved and
// the caller moves its columns (or uses RemoveAndMove, which does it).
// Iterate with `for (d = 0; d < rows.count; ++d)`; dense positions are only
// valid during one system's iteration (3.1). Cross-table references store slots.
//
// Indexing::Direct tables (slot == position, Listener / Spatial) have no
// dense/sparse arrays: liveness is generation[slot] != 0, Remove zeroes it.
// CAUTION: because Remove zeroes the generation, a Direct slot that is
// re-inserted can get the same generation again, so a stale HandleRef to a
// Direct row may look current. Never refer to Direct rows by handle (they are
// addressed by fixed slot: listener index, emitter*L + listener); use
// IsLive(slot) only.
//
// Slot choice: rows whose slot is chosen by the game side (Playback, Emitter,
// Transport, ...: 2.1 handle issue) are inserted with Insert(slot, generation).
// Rows whose slot the audio side chooses (Voice, RealVoice, Stream, ...) take
// a slot from an IndexFreeList (pool.hpp) owned by the creating system and use
// InsertNext(slot), which bumps the slot generation.
//
// Thread safety: single writer (the row creator/destroyer system of the
// schema, which runs on the audio thread). All functions except Init/Destroy
// are RT-safe and O(1) (Clear is O(count)).
#ifndef SND_BASE_SPARSE_SET_HPP
#define SND_BASE_SPARSE_SET_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/handle.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/schema.hpp"

namespace snd::base::rows {

using schema::Indexing;
using schema::TableRows;

// Bytes of row bookkeeping for `capacity` slots (one block, 64-byte aligned):
// generation[cap] | slotToDense[cap] | denseToSlot[cap] (the last two Sparse only).
// Returns 0 if the size does not fit in size_t (32-bit targets).
constexpr std::size_t StorageBytes(std::uint32_t capacity, Indexing indexing) noexcept {
  std::size_t raw = 0, one = 0, total = 0;
  if (!CheckedMul(sizeof(std::uint32_t), capacity, &raw) || !CheckedAlignUp(raw, kBufferAlignment, &one) ||
      !CheckedMul(one, indexing == Indexing::Sparse ? 3u : 1u, &total)) {
    return 0;
  }
  return total;
}

// Binds caller memory of StorageBytes() bytes (8-byte aligned at least) and
// clears it: every slot free, generations 0. RT-safe.
inline void Bind(TableRows& r, std::uint32_t capacity, Indexing indexing, void* memory) noexcept {
  auto* p = static_cast<std::uint8_t*>(memory);
  const std::size_t one = AlignUp(sizeof(std::uint32_t) * static_cast<std::size_t>(capacity), kBufferAlignment);
  r.capacity = capacity;
  r.count = 0;
  r.generation = reinterpret_cast<std::uint32_t*>(p);
  std::memset(r.generation, 0, sizeof(std::uint32_t) * capacity);
  if (indexing == Indexing::Sparse) {
    r.slotToDense = reinterpret_cast<std::uint32_t*>(p + one);
    r.denseToSlot = reinterpret_cast<SlotIndex*>(p + 2u * one);
    for (std::uint32_t i = 0; i < capacity; ++i) {
      r.slotToDense[i] = kInvalidSlot;
      r.denseToSlot[i] = kInvalidSlot;
    }
  } else {
    r.slotToDense = nullptr;
    r.denseToSlot = nullptr;
  }
}

// Owning variant (non-RT). Destroy() frees the block (r.generation is its start).
inline bool Init(TableRows& r, IAllocator& a, std::uint32_t capacity, Indexing indexing,
                 MemTag tag = MemTag::Tables) noexcept {
  r = TableRows{};
  if (capacity == 0u || capacity == kInvalidSlot) return false;
  const std::size_t bytes = StorageBytes(capacity, indexing);
  if (bytes == 0u) return false;
  void* p = a.Allocate(bytes, kBufferAlignment, tag);
  if (p == nullptr) return false;
  Bind(r, capacity, indexing, p);
  return true;
}
inline void Destroy(TableRows& r, IAllocator& a, MemTag tag = MemTag::Tables) noexcept {
  if (r.generation != nullptr) a.Free(r.generation, tag);
  r = TableRows{};
}

inline bool IsSparse(const TableRows& r) noexcept { return r.slotToDense != nullptr; }

inline bool IsLive(const TableRows& r, SlotIndex slot) noexcept {
  if (slot >= r.capacity) return false;
  return IsSparse(r) ? r.slotToDense[slot] != kInvalidSlot : r.generation[slot] != 0u;
}
// True if the row in h.slot is live and has generation h.generation (3.1: stale = ignore).
inline bool IsCurrent(const TableRows& r, HandleRef h) noexcept {
  return h.generation != 0u && IsLive(r, h.slot) && r.generation[h.slot] == h.generation;
}
// Dense position of a live slot, kInvalidSlot otherwise. Direct: slot itself.
inline std::uint32_t DenseOf(const TableRows& r, SlotIndex slot) noexcept {
  if (slot >= r.capacity) return kInvalidSlot;
  if (!IsSparse(r)) return r.generation[slot] != 0u ? slot : kInvalidSlot;
  return r.slotToDense[slot];
}
inline std::uint32_t DenseOf(const TableRows& r, HandleRef h) noexcept {
  return IsCurrent(r, h) ? DenseOf(r, h.slot) : kInvalidSlot;
}
inline SlotIndex SlotAt(const TableRows& r, std::uint32_t dense) noexcept {
  return IsSparse(r) ? r.denseToSlot[dense] : dense;
}
inline HandleRef HandleAt(const TableRows& r, std::uint32_t dense) noexcept {
  const SlotIndex s = SlotAt(r, dense);
  return HandleRef{s, r.generation[s]};
}

// Inserts a row for `slot` with `generation` (!= 0) at dense position `count`.
// Returns the dense position, or kInvalidSlot if the slot is out of range,
// already live, the table is full, or generation is 0. Column values at the
// returned position are stale; the caller writes them.
inline std::uint32_t Insert(TableRows& r, SlotIndex slot, std::uint32_t generation) noexcept {
  if (slot >= r.capacity || generation == 0u || r.count >= r.capacity || IsLive(r, slot)) return kInvalidSlot;
  r.generation[slot] = generation;
  if (!IsSparse(r)) {
    ++r.count;
    return slot;
  }
  const std::uint32_t d = r.count++;
  r.denseToSlot[d] = slot;
  r.slotToDense[slot] = d;
  return d;
}
// Same with the slot's next generation (audio-side slot choice). The new
// generation is r.generation[slot] afterwards.
inline std::uint32_t InsertNext(TableRows& r, SlotIndex slot) noexcept {
  if (slot >= r.capacity) return kInvalidSlot;
  return Insert(r, slot, NextGeneration(r.generation[slot]));
}

struct RemoveResult {
  std::uint32_t dense;      // position that became free (now holds the moved row), kInvalidSlot if nothing removed
  std::uint32_t movedFrom;  // old position of the moved row (== dense when the last row was removed)
  bool Removed() const noexcept { return dense != kInvalidSlot; }
  bool Moved() const noexcept { return dense != kInvalidSlot && movedFrom != dense; }
};

// Removes the row of `slot`. Sparse: the last row moves into the hole; the
// caller must copy its columns from `movedFrom` to `dense`. Generation is
// kept (stale handles stay stale). Not live -> {kInvalidSlot, kInvalidSlot}.
inline RemoveResult Remove(TableRows& r, SlotIndex slot) noexcept {
  if (!IsLive(r, slot)) return RemoveResult{kInvalidSlot, kInvalidSlot};
  if (!IsSparse(r)) {
    r.generation[slot] = 0u;
    --r.count;
    return RemoveResult{slot, slot};
  }
  const std::uint32_t d = r.slotToDense[slot];
  const std::uint32_t last = --r.count;
  if (d != last) {
    const SlotIndex moved = r.denseToSlot[last];
    r.denseToSlot[d] = moved;
    r.slotToDense[moved] = d;
  }
  r.denseToSlot[last] = kInvalidSlot;
  r.slotToDense[slot] = kInvalidSlot;
  return RemoveResult{d, last};
}

// Removes only if `h` is current. Stale handles: nothing happens.
inline RemoveResult Remove(TableRows& r, HandleRef h) noexcept {
  if (!IsCurrent(r, h)) return RemoveResult{kInvalidSlot, kInvalidSlot};
  return Remove(r, h.slot);
}

// Moves column element `from` -> `to` (atomics through load/store).
template <class T>
inline void MoveElement(T* column, std::uint32_t to, std::uint32_t from) noexcept {
  if (column == nullptr) return;
  if constexpr (schema::kIsAtomic<T>) {
    column[to].store(column[from].load(std::memory_order_relaxed), std::memory_order_relaxed);
  } else {
    static_assert(std::is_trivially_copyable_v<T>, "columns are PODs");
    std::memcpy(static_cast<void*>(column + to), static_cast<const void*>(column + from), sizeof(T));
  }
}

// Remove + move every given column of the swapped row:
//   rows::RemoveAndMove(t.rows, slot, t.col.eventRef, t.col.seed, ...);
template <class... Cols>
inline RemoveResult RemoveAndMove(TableRows& r, SlotIndex slot, Cols*... columns) noexcept {
  const RemoveResult res = Remove(r, slot);
  if (res.Moved()) (MoveElement(columns, res.dense, res.movedFrom), ...);
  return res;
}

// Removes every row (generations kept). O(count).
inline void Clear(TableRows& r) noexcept {
  if (IsSparse(r)) {
    for (std::uint32_t d = 0; d < r.count; ++d) {
      r.slotToDense[r.denseToSlot[d]] = kInvalidSlot;
      r.denseToSlot[d] = kInvalidSlot;
    }
  } else {
    for (std::uint32_t s = 0; s < r.capacity; ++s) r.generation[s] = 0u;
  }
  r.count = 0;
}

// Debug check of the invariants (O(capacity)); used by tests and asserts.
inline bool Validate(const TableRows& r) noexcept {
  if (r.count > r.capacity) return false;
  if (!IsSparse(r)) {
    std::uint32_t live = 0;
    for (std::uint32_t s = 0; s < r.capacity; ++s) live += r.generation[s] != 0u ? 1u : 0u;
    return live == r.count;
  }
  std::uint32_t live = 0;
  for (std::uint32_t s = 0; s < r.capacity; ++s) {
    const std::uint32_t d = r.slotToDense[s];
    if (d == kInvalidSlot) continue;
    ++live;
    if (d >= r.count || r.denseToSlot[d] != s || r.generation[s] == 0u) return false;
  }
  return live == r.count;
}

}  // namespace snd::base::rows

#endif  // SND_BASE_SPARSE_SET_HPP
