#include "snd/base/allocator.hpp"

#include <cstdlib>
#include <cstring>

#include "snd/base/atomic.hpp"
#include "snd/internal/rt_check.hpp"

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace snd::base {

void* MallocAllocator::Allocate(std::size_t bytes, std::size_t align, MemTag) {
  SND_RT_NOTE(Alloc);
  if (bytes == 0u || !IsPowerOfTwo(align)) return nullptr;
  if (align < kMinAlignment) align = kMinAlignment;
#if defined(_WIN32)
  return _aligned_malloc(bytes, align);
#else
  void* p = nullptr;
  if (posix_memalign(&p, align, bytes) != 0) return nullptr;
  return p;
#endif
}

void MallocAllocator::Free(void* p, MemTag) {
  if (p == nullptr) return;
  SND_RT_NOTE(Free);
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

IAllocator& DefaultAllocator() noexcept {
  static MallocAllocator instance;
  return instance;
}

// --- AccountingAllocator -------------------------------------------------------
namespace {
constexpr std::uint32_t kHeaderMagic = 0x534E4441u;  // "ADNS"
struct BlockHeader {  // stored in the 16 bytes right before the payload
  std::uint64_t bytes;
  std::uint32_t offset;  // payload - block start
  std::uint32_t magic;
};
static_assert(sizeof(BlockHeader) == 16);
}  // namespace

std::uint32_t AccountingAllocator::Index(MemTag tag) noexcept {
  const auto i = static_cast<std::uint32_t>(tag);
  return i < kMemTagCount ? i : static_cast<std::uint32_t>(MemTag::Misc);
}

void* AccountingAllocator::Allocate(std::size_t bytes, std::size_t align, MemTag tag) {
  SND_RT_NOTE(Alloc);
  Counters& c = counters_[Index(tag)];
  if (bytes == 0u || !IsPowerOfTwo(align)) {
    c.failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  if (align < kMinAlignment) align = kMinAlignment;
  const std::size_t offset = align;  // >= 16, keeps the payload aligned
  if (bytes > (~static_cast<std::size_t>(0)) - offset) {
    c.failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  // Two counters: `reserved` enforces the budget and includes requests still
  // waiting for the upstream allocator; `used` (and therefore `peak`) only
  // counts successful allocations. The reservation is a CAS that is made only
  // if the request fits, so an oversized request never occupies budget, and a
  // request that fails upstream never shows up in `used` / `peak`.
  const std::uint64_t budget = c.budget.load(std::memory_order_relaxed);
  if (budget != 0u) {
    std::uint64_t cur = c.reserved.load(std::memory_order_relaxed);
    for (;;) {
      if (bytes > budget || cur > budget - bytes) {
        c.failures.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
      if (c.reserved.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed)) break;
    }
  } else {
    c.reserved.fetch_add(bytes, std::memory_order_relaxed);
  }
  auto* block = static_cast<std::uint8_t*>(upstream_.Allocate(bytes + offset, align, tag));
  if (block == nullptr) {
    c.reserved.fetch_sub(bytes, std::memory_order_relaxed);
    c.failures.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  std::uint8_t* payload = block + offset;
  BlockHeader h{bytes, static_cast<std::uint32_t>(offset), kHeaderMagic};
  std::memcpy(payload - sizeof(BlockHeader), &h, sizeof(h));
  const std::uint64_t after = c.used.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  AtomicFetchMax(c.peak, after);
  c.allocations.fetch_add(1, std::memory_order_relaxed);
  return payload;
}

void AccountingAllocator::Free(void* p, MemTag tag) {
  if (p == nullptr) return;
  SND_RT_NOTE(Free);
  auto* payload = static_cast<std::uint8_t*>(p);
  BlockHeader h;
  std::memcpy(&h, payload - sizeof(BlockHeader), sizeof(h));
  Counters& c = counters_[Index(tag)];
  c.used.fetch_sub(h.bytes, std::memory_order_relaxed);
  c.allocations.fetch_sub(1, std::memory_order_relaxed);
  upstream_.Free(payload - h.offset, tag);
  c.reserved.fetch_sub(h.bytes, std::memory_order_relaxed);  // budget freed after the memory is
}

void AccountingAllocator::SetBudget(MemTag tag, std::uint64_t bytes) noexcept {
  counters_[Index(tag)].budget.store(bytes, std::memory_order_relaxed);
}

MemTagUsage AccountingAllocator::Usage(MemTag tag) const noexcept {
  const Counters& c = counters_[Index(tag)];
  MemTagUsage u;
  u.used = c.used.load(std::memory_order_relaxed);
  u.peak = c.peak.load(std::memory_order_relaxed);
  u.budget = c.budget.load(std::memory_order_relaxed);
  u.allocations = c.allocations.load(std::memory_order_relaxed);
  u.failures = c.failures.load(std::memory_order_relaxed);
  return u;
}

std::uint64_t AccountingAllocator::TotalUsed() const noexcept {
  std::uint64_t t = 0;
  for (const Counters& c : counters_) t += c.used.load(std::memory_order_relaxed);
  return t;
}

void AccountingAllocator::FillStats(std::uint64_t* used, std::uint64_t* budget) const noexcept {
  for (std::uint32_t i = 0; i < kMemTagCount; ++i) {
    if (used != nullptr) used[i] = counters_[i].used.load(std::memory_order_relaxed);
    if (budget != nullptr) budget[i] = counters_[i].budget.load(std::memory_order_relaxed);
  }
}

}  // namespace snd::base
