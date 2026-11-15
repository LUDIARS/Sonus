#include "snd/base/notify.hpp"

#include <cstring>

namespace snd::base::notify {

namespace {
constexpr MemTag kTag = MemTag::Notify;
}

bool InitEndFlags(EndFlags& f, IAllocator& a, std::uint32_t capacity) noexcept {
  f = EndFlags{};
  if (capacity == 0u) return false;
  f.records = AllocateArray<EndRecord>(a, capacity, kTag);
  f.bits = AllocateArray<std::atomic<std::uint64_t>>(a, BitWords(capacity), kTag);
  if (f.records == nullptr || f.bits == nullptr) {
    DestroyEndFlags(f, a);
    return false;
  }
  f.capacity = capacity;
  return true;
}

void DestroyEndFlags(EndFlags& f, IAllocator& a) noexcept {
  FreeArray(a, f.records, f.capacity, kTag);
  FreeArray(a, f.bits, BitWords(f.capacity), kTag);
  f = EndFlags{};
}

bool InitReleaseFlags(ReleaseFlags& f, IAllocator& a, std::uint32_t capacity) noexcept {
  f = ReleaseFlags{};
  if (capacity == 0u) return false;
  f.generation = AllocateArray<std::uint32_t>(a, capacity, kTag);
  f.bits = AllocateArray<std::atomic<std::uint64_t>>(a, BitWords(capacity), kTag);
  if (f.generation == nullptr || f.bits == nullptr) {
    DestroyReleaseFlags(f, a);
    return false;
  }
  f.capacity = capacity;
  return true;
}

void DestroyReleaseFlags(ReleaseFlags& f, IAllocator& a) noexcept {
  FreeArray(a, f.generation, f.capacity, kTag);
  FreeArray(a, f.bits, BitWords(f.capacity), kTag);
  f = ReleaseFlags{};
}

bool InitEventRing(EventRing& r, IAllocator& a, std::uint32_t capacity) noexcept {
  const std::uint64_t cap = NextPowerOfTwo(capacity < 2u ? 2u : capacity);
  if (cap > 0x80000000u) return false;
  r.items = AllocateArray<EventRecord>(a, static_cast<std::size_t>(cap), kTag);
  if (r.items == nullptr) {
    r.capacity = 0;
    return false;
  }
  r.capacity = static_cast<std::uint32_t>(cap);
  r.write.store(0, std::memory_order_relaxed);
  r.read.store(0, std::memory_order_relaxed);
  r.dropped.store(0, std::memory_order_relaxed);
  return true;
}

void DestroyEventRing(EventRing& r, IAllocator& a) noexcept {
  FreeArray(a, r.items, r.capacity, kTag);
  r.items = nullptr;
  r.capacity = 0;
}

bool InitTripleBuffer(SnapshotTripleBuffer& tb, IAllocator& a, std::uint32_t maxPlaybacks,
                      std::uint32_t maxTransports, std::uint32_t busCapacity) noexcept {
  static_assert(std::is_trivially_destructible_v<PlaybackSnapshot> &&
                std::is_trivially_destructible_v<TransportSnapshot> && std::is_trivially_destructible_v<BusSnapshot>);
  for (SnapshotFrame& f : tb.frames) {
    f.seq.store(0, std::memory_order_relaxed);
    f.reserved = 0;  // front sequence (notify.hpp SwapFront)
    f.block = 0;
    f.clock = 0;
    f.stats = snd_stats{};
    f.busCount = 0;
    f.busCapacity = busCapacity;
    f.playbacks = nullptr;
    f.transports = nullptr;
    f.buses = nullptr;
  }
  bool ok = true;
  for (SnapshotFrame& f : tb.frames) {
    if (maxPlaybacks != 0u) ok = ok && (f.playbacks = AllocateArray<PlaybackSnapshot>(a, maxPlaybacks, kTag)) != nullptr;
    if (maxTransports != 0u) {
      ok = ok && (f.transports = AllocateArray<TransportSnapshot>(a, maxTransports, kTag)) != nullptr;
    }
    if (busCapacity != 0u) ok = ok && (f.buses = AllocateArray<BusSnapshot>(a, busCapacity, kTag)) != nullptr;
  }
  tb.middle.store(1u, std::memory_order_relaxed);
  tb.back = 0;
  tb.front = 2;
  tb.frontPublished.store(2u, std::memory_order_relaxed);
  if (!ok) {
    DestroyTripleBuffer(tb, a);
    return false;
  }
  return true;
}

void DestroyTripleBuffer(SnapshotTripleBuffer& tb, IAllocator& a) noexcept {
  for (SnapshotFrame& f : tb.frames) {
    FreeArray(a, f.playbacks, 0, kTag);  // trivially destructible: count unused
    FreeArray(a, f.transports, 0, kTag);
    FreeArray(a, f.buses, 0, kTag);
    f.playbacks = nullptr;
    f.transports = nullptr;
    f.buses = nullptr;
    f.busCapacity = 0;
  }
}

void SetFrameHeader(SnapshotFrame& f, BlockIndex block, ClockSample clock, const snd_stats& stats,
                    std::uint32_t busCount) noexcept {
  __atomic_store_n(&f.block, block, __ATOMIC_RELEASE);
  __atomic_store_n(&f.clock, clock, __ATOMIC_RELEASE);
  AtomicCopyIn(&f.stats, &stats, sizeof(snd_stats));
  __atomic_store_n(&f.busCount, busCount, __ATOMIC_RELEASE);
}

FrameHeader GetFrameHeader(const SnapshotFrame& f) noexcept {
  FrameHeader h;
  h.block = __atomic_load_n(&f.block, __ATOMIC_ACQUIRE);
  h.clock = __atomic_load_n(&f.clock, __ATOMIC_ACQUIRE);
  AtomicCopyOut(&h.stats, &f.stats, sizeof(snd_stats));
  h.busCount = __atomic_load_n(&f.busCount, __ATOMIC_ACQUIRE);
  return h;
}

}  // namespace snd::base::notify
