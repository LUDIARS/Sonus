// Audio -> game notification channels (design.md 2.1, CONTRACTS.md D-16).
//
// Implements the protocols written at the top of snd/internal/notify.hpp on
// the contract structs (EndFlags, ReleaseFlags, EventRing,
// SnapshotTripleBuffer). Threads:
//
//   writer side (Publish, audio thread)    PublishEnd, PublishRelease,
//                                          PushEvent, BeginSnapshot / EndSnapshot,
//                                          SnapshotStore           -> RT-safe, wait-free
//   reader side (Update, one game thread)  ConsumeEnds, ConsumeReleases,
//                                          PopEvent / DrainEvents, SwapFront
//                                                                  -> RT-safe, wait-free
//   queries (any thread)                   ReadFront + SnapshotLoad -> lock-free (seqlock retry)
//   Init* / Destroy* (owner thread, init)  allocate through IAllocator (MemTag::Notify)
#ifndef SND_BASE_NOTIFY_HPP
#define SND_BASE_NOTIFY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/notify.hpp"

namespace snd::base::notify {

using snd::notify::BusSnapshot;
using snd::notify::EndFlags;
using snd::notify::EndRecord;
using snd::notify::EventRecord;
using snd::notify::EventRing;
using snd::notify::PlaybackSnapshot;
using snd::notify::ReleaseFlags;
using snd::notify::SnapshotFrame;
using snd::notify::SnapshotTripleBuffer;
using snd::notify::TransportSnapshot;

constexpr std::uint32_t BitWords(std::uint32_t capacity) noexcept { return (capacity + 63u) / 64u; }

// --- 1. Lossless flag arrays --------------------------------------------------
bool InitEndFlags(EndFlags& f, IAllocator& a, std::uint32_t capacity) noexcept;
void DestroyEndFlags(EndFlags& f, IAllocator& a) noexcept;

// Audio thread: write the record, then set the bit with release.
// A slot must not be published again before the reader consumed it (2.1).
inline void PublishEnd(EndFlags& f, SlotIndex slot, const EndRecord& rec) noexcept {
  if (slot >= f.capacity) return;
  f.records[slot] = rec;
  f.bits[slot >> 6].fetch_or(std::uint64_t{1} << (slot & 63u), std::memory_order_release);
}

// Update thread: takes every set bit (exchange(0, acquire) per word) and
// calls fn(SlotIndex slot, const EndRecord& rec) in slot order. Returns the count.
template <class Fn>
std::uint32_t ConsumeEnds(EndFlags& f, Fn&& fn) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t w = 0, words = BitWords(f.capacity); w < words; ++w) {
    if (f.bits[w].load(std::memory_order_relaxed) == 0u) continue;
    std::uint64_t bits = f.bits[w].exchange(0u, std::memory_order_acquire);
    while (bits != 0u) {
      const std::uint32_t slot = w * 64u + static_cast<std::uint32_t>(CountTrailingZeros64(bits));
      bits &= bits - 1u;
      const EndRecord rec = f.records[slot];
      fn(slot, rec);
      ++n;
    }
  }
  return n;
}

bool InitReleaseFlags(ReleaseFlags& f, IAllocator& a, std::uint32_t capacity) noexcept;
void DestroyReleaseFlags(ReleaseFlags& f, IAllocator& a) noexcept;

inline void PublishRelease(ReleaseFlags& f, SlotIndex slot, std::uint32_t generation) noexcept {
  if (slot >= f.capacity) return;
  f.generation[slot] = generation;
  f.bits[slot >> 6].fetch_or(std::uint64_t{1} << (slot & 63u), std::memory_order_release);
}
// fn(SlotIndex slot, std::uint32_t releasedGeneration)
template <class Fn>
std::uint32_t ConsumeReleases(ReleaseFlags& f, Fn&& fn) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t w = 0, words = BitWords(f.capacity); w < words; ++w) {
    if (f.bits[w].load(std::memory_order_relaxed) == 0u) continue;
    std::uint64_t bits = f.bits[w].exchange(0u, std::memory_order_acquire);
    while (bits != 0u) {
      const std::uint32_t slot = w * 64u + static_cast<std::uint32_t>(CountTrailingZeros64(bits));
      bits &= bits - 1u;
      fn(slot, f.generation[slot]);
      ++n;
    }
  }
  return n;
}

// --- 2. Event ring (SPSC, drop when full) -----------------------------------------
// capacity is rounded up to a power of two (Config.eventRingCapacity).
bool InitEventRing(EventRing& r, IAllocator& a, std::uint32_t capacity) noexcept;
void DestroyEventRing(EventRing& r, IAllocator& a) noexcept;

// Audio thread. false = full: the record is dropped and `dropped` incremented.
inline bool PushEvent(EventRing& r, const EventRecord& rec) noexcept {
  const std::uint64_t w = r.write.load(std::memory_order_relaxed);
  const std::uint64_t rd = r.read.load(std::memory_order_acquire);
  if (w - rd >= r.capacity) {
    r.dropped.fetch_add(1u, std::memory_order_relaxed);
    return false;
  }
  r.items[w & (r.capacity - 1u)] = rec;
  r.write.store(w + 1u, std::memory_order_release);
  return true;
}
// Update thread. false when empty.
inline bool PopEvent(EventRing& r, EventRecord& out) noexcept {
  const std::uint64_t rd = r.read.load(std::memory_order_relaxed);
  if (rd == r.write.load(std::memory_order_acquire)) return false;
  out = r.items[rd & (r.capacity - 1u)];
  r.read.store(rd + 1u, std::memory_order_release);
  return true;
}
// Update thread: fn(const EventRecord&) for every pending record, at most `max`.
template <class Fn>
std::uint32_t DrainEvents(EventRing& r, Fn&& fn, std::uint32_t max = 0xFFFFFFFFu) noexcept {
  std::uint32_t n = 0;
  EventRecord rec;
  while (n < max && PopEvent(r, rec)) {
    fn(static_cast<const EventRecord&>(rec));
    ++n;
  }
  return n;
}
// Update thread: returns and clears the drop counter (stats.dropped_events).
inline std::uint32_t TakeDropped(EventRing& r) noexcept { return r.dropped.exchange(0u, std::memory_order_relaxed); }

// --- 3. Snapshot triple buffer with per-frame seqlock -------------------------------
// Allocates the per-frame arrays: playbacks[maxPlaybacks], transports[maxTransports],
// buses[busCapacity] for each of the 3 frames (all zero).
bool InitTripleBuffer(SnapshotTripleBuffer& tb, IAllocator& a, std::uint32_t maxPlaybacks,
                      std::uint32_t maxTransports, std::uint32_t busCapacity) noexcept;
void DestroyTripleBuffer(SnapshotTripleBuffer& tb, IAllocator& a) noexcept;

// Seqlock-safe copies of snapshot PODs (sizeof % 8 == 0, 8-byte aligned).
template <class T>
inline void SnapshotStore(T* dst, const T& v) noexcept {
  static_assert(sizeof(T) % 8u == 0u && std::is_trivially_copyable_v<T>);
  AtomicCopyIn(dst, &v, sizeof(T));
}
template <class T>
inline T SnapshotLoad(const T* src) noexcept {
  static_assert(sizeof(T) % 8u == 0u && std::is_trivially_copyable_v<T>);
  T v;
  AtomicCopyOut(&v, src, sizeof(T));
  return v;
}

// Writer (Publish): opens the back frame (seq becomes odd). Fill it only with
// SnapshotStore / SetFrameHeader, then call EndSnapshot.
inline SnapshotFrame& BeginSnapshot(SnapshotTripleBuffer& tb) noexcept {
  SnapshotFrame& f = tb.frames[tb.back];
  // No fence needed: every data word is stored with release (SnapshotStore),
  // so a reader that observes any new word also observes this odd sequence.
  const std::uint32_t s = f.seq.load(std::memory_order_relaxed);
  f.seq.store(s + 1u, std::memory_order_relaxed);
  return f;
}
// Writer: header fields (block, clock, stats, busCount) of the open frame.
void SetFrameHeader(SnapshotFrame& f, BlockIndex block, ClockSample clock, const snd_stats& stats,
                    std::uint32_t busCount) noexcept;
// Writer: closes the frame (seq even) and exchanges it with the middle (dirty).
inline void EndSnapshot(SnapshotTripleBuffer& tb) noexcept {
  SnapshotFrame& f = tb.frames[tb.back];
  f.seq.store(f.seq.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
  const std::uint32_t prev = tb.middle.exchange(tb.back | snd::notify::kTripleDirtyBit, std::memory_order_acq_rel);
  tb.back = prev & 3u;
}

// Reader (Update): takes the newest frame if there is one. Returns true if
// the front changed. Queries afterwards see that frame.
inline bool SwapFront(SnapshotTripleBuffer& tb) noexcept {
  if ((tb.middle.load(std::memory_order_relaxed) & snd::notify::kTripleDirtyBit) == 0u) return false;
  const std::uint32_t prev = tb.middle.exchange(tb.front, std::memory_order_acq_rel);
  tb.front = prev & 3u;
  tb.frontPublished.store(tb.front, std::memory_order_release);
  return true;
}

// Query (any thread): calls copy(const SnapshotFrame&) — which must read only
// through SnapshotLoad / GetFrameHeader — until it ran on a stable frame.
// Returns false after `maxAttempts` torn reads (practically never).
template <class Fn>
bool ReadFront(const SnapshotTripleBuffer& tb, Fn&& copy, std::uint32_t maxAttempts = 1024u) noexcept {
  for (std::uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
    const std::uint32_t idx = tb.frontPublished.load(std::memory_order_acquire) & 3u;
    const SnapshotFrame& f = tb.frames[idx];
    const std::uint32_t s1 = f.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) {
      CpuRelax();
      continue;
    }
    copy(f);  // acquire loads: the re-check below cannot move before them
    if (f.seq.load(std::memory_order_relaxed) == s1) return true;
    CpuRelax();
  }
  return false;
}
struct FrameHeader {
  BlockIndex block;
  ClockSample clock;
  snd_stats stats;
  std::uint32_t busCount;
};
FrameHeader GetFrameHeader(const SnapshotFrame& f) noexcept;

}  // namespace snd::base::notify

#endif  // SND_BASE_NOTIFY_HPP
