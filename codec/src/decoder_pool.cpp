// Decoder pool (design.md 3.3, D-14). Lock-free tagged Treiber stack of slot indices.
#include "snd/codec/decoder_pool.hpp"

#include <cstdint>
#include <new>

#include "detail.hpp"

namespace snd::codec {
namespace {

constexpr std::uint32_t kNil = 0xFFFFFFFFu;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "decoder pool needs lock-free 64-bit atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

constexpr std::uint64_t Pack(std::uint32_t tag, std::uint32_t index) noexcept {
  return (static_cast<std::uint64_t>(tag) << 32) | index;
}
constexpr std::uint32_t IndexOf(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v); }
constexpr std::uint32_t TagOf(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v >> 32); }

struct Layout {
  std::uint64_t slotBytes;  // rounded
  std::uint64_t slotsBytes;
  std::uint64_t nextOffset;
  std::uint64_t wordOffset;
  std::uint64_t total;
};

bool ComputeLayout(std::uint32_t maxDecoders, std::uint32_t slotBytes, Layout* out) noexcept {
  const std::uint64_t slot = base::AlignUp64(slotBytes, kStateAlignment);
  if (slot > UINT32_MAX) return false;
  const std::uint64_t slots = slot * maxDecoders;  // < 2^64: both factors < 2^32
  const std::uint64_t links = base::AlignUp64(static_cast<std::uint64_t>(maxDecoders) * 4u, kStateAlignment);
  const std::uint64_t words = base::AlignUp64(static_cast<std::uint64_t>(maxDecoders) * 8u, kStateAlignment);
  out->slotBytes = slot;
  out->slotsBytes = slots;
  out->nextOffset = slots;
  out->wordOffset = slots + links;
  out->total = slots + links + words;
  if (out->total < slots || out->total > SIZE_MAX) return false;
  return true;
}

}  // namespace

const char* DecoderPoolStatusName(DecoderPoolStatus s) noexcept {
  switch (s) {
    case DecoderPoolStatus::Ok: return "ok";
    case DecoderPoolStatus::Full: return "decoder pool full";
    case DecoderPoolStatus::StateTooLarge: return "codec state larger than a decoder slot";
    case DecoderPoolStatus::NotInitialized: return "decoder pool not initialised";
    case DecoderPoolStatus::InvalidArgument: return "invalid argument (null output slot)";
  }
  return "?";
}

DecoderPool::~DecoderPool() { Shutdown(); }

std::size_t DecoderPool::RequiredBytes(std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept {
  Layout l{};
  if (!ComputeLayout(maxDecoders, slotBytes, &l)) return 0;
  return static_cast<std::size_t>(l.total);
}

bool DecoderPool::Setup(void* memory, std::size_t bytes, std::uint32_t maxDecoders,
                        std::uint32_t slotBytes) noexcept {
  Layout l{};
  if (!ComputeLayout(maxDecoders, slotBytes, &l)) return false;
  if (bytes < l.total) return false;
  if (maxDecoders > 0u) {
    if (memory == nullptr || (reinterpret_cast<std::uintptr_t>(memory) % kStateAlignment) != 0u) return false;
    if (maxDecoders == kNil) return false;  // kNil marks the end of the free list
  }
  auto* base = static_cast<std::uint8_t*>(memory);
  slots_ = maxDecoders > 0u ? base : nullptr;
  initializedEmpty_ = maxDecoders == 0u;
  capacity_ = maxDecoders;
  slotBytes_ = static_cast<std::uint32_t>(l.slotBytes);
  poolBytes_ = static_cast<std::size_t>(l.total);
  if (maxDecoders > 0u) {
    next_ = reinterpret_cast<std::atomic<std::uint32_t>*>(base + l.nextOffset);
    word_ = reinterpret_cast<std::atomic<std::uint64_t>*>(base + l.wordOffset);
    for (std::uint32_t i = 0; i < maxDecoders; ++i) {
      new (&next_[i]) std::atomic<std::uint32_t>(i + 1u < maxDecoders ? i + 1u : kNil);
      new (&word_[i]) std::atomic<std::uint64_t>(0u);  // generation 0, free
    }
  }
  head_.store(Pack(0, maxDecoders > 0u ? 0u : kNil), std::memory_order_release);
  inUse_.store(0, std::memory_order_relaxed);
  peakInUse_.store(0, std::memory_order_relaxed);
  stateBytesInUse_.store(0, std::memory_order_relaxed);
  acquired_.store(0, std::memory_order_relaxed);
  rejectedFull_.store(0, std::memory_order_relaxed);
  rejectedTooLarge_.store(0, std::memory_order_relaxed);
  badReleases_.store(0, std::memory_order_relaxed);
  return true;
}

bool DecoderPool::Init(void* memory, std::size_t bytes, std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept {
  Shutdown();
  return Setup(memory, bytes, maxDecoders, slotBytes);
}

bool DecoderPool::Init(IAllocator& allocator, std::uint32_t maxDecoders, std::uint32_t slotBytes) noexcept {
  Shutdown();
  const std::size_t bytes = RequiredBytes(maxDecoders, slotBytes);
  if (bytes == 0u) return Setup(nullptr, 0, maxDecoders, slotBytes);
  void* memory = allocator.Allocate(bytes, kStateAlignment, MemTag::DecoderPool);
  if (memory == nullptr) return false;
  if (!Setup(memory, bytes, maxDecoders, slotBytes)) {
    allocator.Free(memory, MemTag::DecoderPool);
    return false;
  }
  owned_ = memory;
  allocator_ = &allocator;
  return true;
}

void DecoderPool::Shutdown() noexcept {
  if (owned_ != nullptr && allocator_ != nullptr) allocator_->Free(owned_, MemTag::DecoderPool);
  owned_ = nullptr;
  allocator_ = nullptr;
  slots_ = nullptr;
  next_ = nullptr;
  word_ = nullptr;
  capacity_ = 0;
  slotBytes_ = 0;
  poolBytes_ = 0;
  initializedEmpty_ = false;
  head_.store(Pack(0, kNil), std::memory_order_relaxed);
}

DecoderPoolStatus DecoderPool::Acquire(std::uint32_t stateBytes, DecoderSlot* out) noexcept {
  if (out == nullptr) return DecoderPoolStatus::InvalidArgument;  // takes no slot
  *out = DecoderSlot{};
  if (!Initialized()) return DecoderPoolStatus::NotInitialized;
  if (stateBytes > slotBytes_) {
    rejectedTooLarge_.fetch_add(1, std::memory_order_relaxed);
    return DecoderPoolStatus::StateTooLarge;
  }
  std::uint64_t h = head_.load(std::memory_order_acquire);
  for (;;) {
    const std::uint32_t top = IndexOf(h);
    if (top == kNil) {
      rejectedFull_.fetch_add(1, std::memory_order_relaxed);
      return DecoderPoolStatus::Full;
    }
    const std::uint32_t nxt = next_[top].load(std::memory_order_relaxed);
    if (head_.compare_exchange_weak(h, Pack(TagOf(h) + 1u, nxt), std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      // The popped slot is free, so its word is {generation, 0} and only this
      // thread writes it now (releases never write a free word).
      std::uint32_t gen = TagOf(word_[top].load(std::memory_order_relaxed)) + 1u;
      if (gen == 0u) gen = 1u;  // 0 never names an acquired slot
      word_[top].store(Pack(gen, stateBytes + 1u), std::memory_order_relaxed);  // stateBytes <= slotBytes_ < UINT32_MAX
      const std::uint32_t now = inUse_.fetch_add(1, std::memory_order_relaxed) + 1u;
      std::uint32_t peak = peakInUse_.load(std::memory_order_relaxed);
      while (now > peak && !peakInUse_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
      }
      stateBytesInUse_.fetch_add(stateBytes, std::memory_order_relaxed);
      acquired_.fetch_add(1, std::memory_order_relaxed);
      *out = DecoderSlot{top, gen, slots_ + static_cast<std::size_t>(top) * slotBytes_};
      return DecoderPoolStatus::Ok;
    }
  }
}

bool DecoderPool::Release(std::uint32_t index, std::uint32_t generation) noexcept {
  return ReleaseImpl(index, generation, true);
}

bool DecoderPool::Release(std::uint32_t index) noexcept { return ReleaseImpl(index, 0, false); }

bool DecoderPool::ReleaseImpl(std::uint32_t index, std::uint32_t generation, bool checkGeneration) noexcept {
  if (index >= capacity_ || word_ == nullptr) {
    badReleases_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Atomically: acquired (low != 0) with the expected generation -> free, same generation.
  std::uint64_t w = word_[index].load(std::memory_order_relaxed);
  for (;;) {
    if (IndexOf(w) == 0u || (checkGeneration && TagOf(w) != generation)) {
      badReleases_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (word_[index].compare_exchange_weak(w, Pack(TagOf(w), 0u), std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
      break;
    }
  }
  stateBytesInUse_.fetch_sub(IndexOf(w) - 1u, std::memory_order_relaxed);
  inUse_.fetch_sub(1, std::memory_order_relaxed);
  std::uint64_t h = head_.load(std::memory_order_relaxed);
  for (;;) {
    next_[index].store(IndexOf(h), std::memory_order_relaxed);
    if (head_.compare_exchange_weak(h, Pack(TagOf(h) + 1u, index), std::memory_order_release,
                                    std::memory_order_relaxed)) {
      return true;
    }
  }
}

std::uint32_t DecoderPool::Generation(std::uint32_t index) const noexcept {
  if (index >= capacity_ || word_ == nullptr) return 0;
  const std::uint64_t w = word_[index].load(std::memory_order_relaxed);
  return IndexOf(w) != 0u ? TagOf(w) : 0u;
}

void* DecoderPool::State(std::uint32_t index) const noexcept {
  if (index >= capacity_ || slots_ == nullptr) return nullptr;
  return slots_ + static_cast<std::size_t>(index) * slotBytes_;
}

DecoderPoolUsage DecoderPool::Usage() const noexcept {
  DecoderPoolUsage u{};
  u.capacity = capacity_;
  u.slotBytes = slotBytes_;
  u.poolBytes = poolBytes_;
  u.inUse = inUse_.load(std::memory_order_relaxed);
  u.peakInUse = peakInUse_.load(std::memory_order_relaxed);
  u.stateBytesInUse = stateBytesInUse_.load(std::memory_order_relaxed);
  u.acquired = acquired_.load(std::memory_order_relaxed);
  u.rejectedFull = rejectedFull_.load(std::memory_order_relaxed);
  u.rejectedTooLarge = rejectedTooLarge_.load(std::memory_order_relaxed);
  u.badReleases = badReleases_.load(std::memory_order_relaxed);
  return u;
}

}  // namespace snd::codec
