#include "snd/io/stream_pool.hpp"

namespace snd::io {

bool StreamPool::Init(IAllocator& allocator, std::uint64_t poolBytes, std::uint32_t chunkBytes,
                      std::uint32_t maxOwners, std::size_t align) noexcept {
  Destroy();
  if (chunkBytes == 0u || !base::IsPowerOfTwo(align)) return false;
  if (align < base::kMinAlignment) align = base::kMinAlignment;
  const std::uint64_t stride = base::AlignUp(chunkBytes, align);
  const std::uint64_t count = poolBytes / stride;
  if (count == 0u || count >= kFree) return false;
  if (maxOwners >= kFree) return false;
  if (!blocks_.Init(allocator, chunkBytes, static_cast<std::uint32_t>(count), MemTag::StreamPool, align)) {
    return false;
  }
  owner_ = base::AllocateArray<std::atomic<std::uint32_t>>(allocator, static_cast<std::size_t>(count),
                                                           MemTag::StreamPool);
  held_ = base::AllocateArray<std::atomic<std::uint32_t>>(allocator, static_cast<std::size_t>(maxOwners) + 1u,
                                                          MemTag::StreamPool);
  if (owner_ == nullptr || held_ == nullptr) {
    alloc_ = &allocator;
    chunkCount_ = static_cast<std::uint32_t>(count);
    maxOwners_ = maxOwners;
    Destroy();
    return false;
  }
  for (std::uint64_t i = 0; i < count; ++i) owner_[i].store(kFree, std::memory_order_relaxed);
  alloc_ = &allocator;
  chunkCount_ = static_cast<std::uint32_t>(count);
  chunkBytes_ = chunkBytes;
  maxOwners_ = maxOwners;
  used_.value.store(0, std::memory_order_relaxed);
  peak_.store(0, std::memory_order_relaxed);
  failed_.store(0, std::memory_order_relaxed);
  badReleases_.store(0, std::memory_order_relaxed);
  return true;
}

void StreamPool::Destroy() noexcept {
  if (alloc_ != nullptr) {
    base::FreeArray(*alloc_, owner_, chunkCount_, MemTag::StreamPool);
    base::FreeArray(*alloc_, held_, static_cast<std::size_t>(maxOwners_) + 1u, MemTag::StreamPool);
  }
  blocks_.Destroy();
  owner_ = nullptr;
  held_ = nullptr;
  alloc_ = nullptr;
  chunkCount_ = 0;
  chunkBytes_ = 0;
  maxOwners_ = 0;
}

std::uint32_t StreamPool::Acquire(std::uint32_t owner) noexcept {
  if (chunkCount_ == 0u) return kInvalidChunk;
  const std::uint32_t chunk = blocks_.AcquireIndex();
  if (chunk == base::kInvalidIndex) {
    failed_.fetch_add(1u, std::memory_order_relaxed);
    return kInvalidChunk;
  }
  const std::uint32_t stored = owner < maxOwners_ ? owner : kNoOwner;
  owner_[chunk].store(stored, std::memory_order_relaxed);
  held_[Bucket(owner)].fetch_add(1u, std::memory_order_relaxed);
  const std::uint32_t used = used_.value.fetch_add(1u, std::memory_order_relaxed) + 1u;
  base::AtomicFetchMax(peak_, used);
  return chunk;
}

void StreamPool::Release(std::uint32_t chunk) noexcept {
  if (chunk >= chunkCount_) {
    badReleases_.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  // The exchange makes a double release harmless: only one caller sees the owner.
  const std::uint32_t owner = owner_[chunk].exchange(kFree, std::memory_order_acq_rel);
  if (owner == kFree) {
    badReleases_.fetch_add(1u, std::memory_order_relaxed);
    return;
  }
  held_[Bucket(owner)].fetch_sub(1u, std::memory_order_relaxed);
  used_.value.fetch_sub(1u, std::memory_order_relaxed);
  blocks_.ReleaseIndex(chunk);
}

std::uint32_t StreamPool::IndexOf(const void* p) const noexcept {
  if (p == nullptr || chunkCount_ == 0u || !blocks_.Owns(p)) return kInvalidChunk;
  const std::uint32_t i = blocks_.IndexOf(p);
  return blocks_.Block(i) == p ? i : kInvalidChunk;
}

bool StreamPool::IsBorrowed(std::uint32_t chunk) const noexcept {
  return chunk < chunkCount_ && owner_[chunk].load(std::memory_order_acquire) != kFree;
}

std::uint32_t StreamPool::OwnerOf(std::uint32_t chunk) const noexcept {
  if (chunk >= chunkCount_) return kNoOwner;
  const std::uint32_t o = owner_[chunk].load(std::memory_order_relaxed);
  return o == kFree ? kNoOwner : o;
}

std::uint32_t StreamPool::HeldBy(std::uint32_t owner) const noexcept {
  if (held_ == nullptr) return 0u;
  return held_[Bucket(owner)].load(std::memory_order_relaxed);
}

std::uint32_t StreamPool::Holdings(std::uint32_t* counts, std::uint32_t capacity) const noexcept {
  if (counts == nullptr || held_ == nullptr) return 0u;
  const std::uint32_t n = capacity < maxOwners_ ? capacity : maxOwners_;
  for (std::uint32_t i = 0; i < n; ++i) counts[i] = held_[i].load(std::memory_order_relaxed);
  return n;
}

StreamPoolStats StreamPool::Stats() const noexcept {
  StreamPoolStats s;
  s.chunkCount = chunkCount_;
  s.chunkBytes = chunkBytes_;
  s.used = Used();
  s.peak = peak_.load(std::memory_order_relaxed);
  s.failedAcquires = failed_.load(std::memory_order_relaxed);
  s.badReleases = badReleases_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace snd::io
