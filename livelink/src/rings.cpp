// SPSC channels of the live link.
#include "snd/livelink/rings.hpp"

namespace snd::livelink {

namespace {
std::uint32_t Pow2(std::uint32_t n) noexcept {
  std::uint32_t c = 64;
  while (c < n && c < (1u << 30)) c <<= 1;
  return c;
}
}  // namespace

// ---------------------------------------------------------------------------
// ByteRing
// ---------------------------------------------------------------------------
bool ByteRing::Init(IAllocator& a, std::uint32_t capacity) noexcept {
  Destroy();
  alloc_ = &a;
  capacity_ = Pow2(capacity);
  data_ = static_cast<std::uint8_t*>(a.Allocate(capacity_, 64, MemTag::LiveLink));
  if (data_ == nullptr) {
    capacity_ = 0;
    return false;
  }
  Reset();
  return true;
}

void ByteRing::Destroy() noexcept {
  if (alloc_ != nullptr && data_ != nullptr) alloc_->Free(data_, MemTag::LiveLink);
  data_ = nullptr;
  capacity_ = 0;
}

std::size_t ByteRing::Write(const void* src, std::size_t n) noexcept {
  const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  const std::size_t room = capacity_ - static_cast<std::size_t>(tail - head);
  if (n > room) n = room;
  const std::uint8_t* s = static_cast<const std::uint8_t*>(src);
  std::size_t done = 0;
  while (done < n) {
    const std::uint32_t at = static_cast<std::uint32_t>((tail + done) & (capacity_ - 1u));
    std::size_t run = capacity_ - at;
    if (run > n - done) run = n - done;
    std::memcpy(data_ + at, s + done, run);
    done += run;
  }
  tail_.store(tail + n, std::memory_order_release);
  return n;
}

std::size_t ByteRing::Read(void* dst, std::size_t n) noexcept {
  const std::uint64_t head = head_.load(std::memory_order_relaxed);
  const std::uint64_t tail = tail_.load(std::memory_order_acquire);
  const std::size_t have = static_cast<std::size_t>(tail - head);
  if (n > have) n = have;
  std::uint8_t* d = static_cast<std::uint8_t*>(dst);
  std::size_t done = 0;
  while (done < n) {
    const std::uint32_t at = static_cast<std::uint32_t>((head + done) & (capacity_ - 1u));
    std::size_t run = capacity_ - at;
    if (run > n - done) run = n - done;
    std::memcpy(d + done, data_ + at, run);
    done += run;
  }
  head_.store(head + n, std::memory_order_release);
  return n;
}

// ---------------------------------------------------------------------------
// MessageRing: [u32 bytes][u32 pad][payload, padded to 8]
// ---------------------------------------------------------------------------
bool MessageRing::Init(IAllocator& a, std::uint32_t capacity) noexcept {
  Destroy();
  alloc_ = &a;
  capacity_ = Pow2(capacity);
  data_ = static_cast<std::uint8_t*>(a.Allocate(capacity_, 64, MemTag::LiveLink));
  if (data_ == nullptr) {
    capacity_ = 0;
    return false;
  }
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  writeAt_ = writeEnd_ = 0;
  nextBytes_ = 0;
  return true;
}

void MessageRing::Destroy() noexcept {
  if (alloc_ != nullptr && data_ != nullptr) alloc_->Free(data_, MemTag::LiveLink);
  data_ = nullptr;
  capacity_ = 0;
}

void MessageRing::CopyIn(std::uint64_t at, const void* src, std::uint32_t n) noexcept {
  const std::uint8_t* s = static_cast<const std::uint8_t*>(src);
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t pos = static_cast<std::uint32_t>((at + done) & (capacity_ - 1u));
    std::uint32_t run = capacity_ - pos;
    if (run > n - done) run = n - done;
    std::memcpy(data_ + pos, s + done, run);
    done += run;
  }
}

void MessageRing::CopyOut(std::uint64_t at, void* dst, std::uint32_t n) const noexcept {
  std::uint8_t* d = static_cast<std::uint8_t*>(dst);
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t pos = static_cast<std::uint32_t>((at + done) & (capacity_ - 1u));
    std::uint32_t run = capacity_ - pos;
    if (run > n - done) run = n - done;
    std::memcpy(d + done, data_ + pos, run);
    done += run;
  }
}

bool MessageRing::Begin(std::uint32_t bytes) noexcept {
  if (data_ == nullptr) return false;
  const std::uint64_t total = 8u + ((std::uint64_t{bytes} + 7u) & ~std::uint64_t{7});
  const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  if (total > capacity_ - (tail - head)) return false;
  const std::uint32_t hdr[2] = {bytes, 0u};
  CopyIn(tail, hdr, 8u);
  writeAt_ = tail + 8u;
  writeEnd_ = tail + total;
  return true;
}

void MessageRing::Put(const void* src, std::uint32_t n) noexcept {
  if (n > writeEnd_ - writeAt_) n = static_cast<std::uint32_t>(writeEnd_ - writeAt_);
  CopyIn(writeAt_, src, n);
  writeAt_ += n;
}

void MessageRing::Commit() noexcept { tail_.store(writeEnd_, std::memory_order_release); }

bool MessageRing::Next(std::uint32_t* bytes) noexcept {
  if (data_ == nullptr) return false;
  const std::uint64_t head = head_.load(std::memory_order_relaxed);
  if (tail_.load(std::memory_order_acquire) == head) return false;
  std::uint32_t hdr[2];
  CopyOut(head, hdr, 8u);
  nextBytes_ = hdr[0];
  *bytes = hdr[0];
  return true;
}

void MessageRing::Copy(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept {
  if (offset > nextBytes_ || n > nextBytes_ - offset) return;
  CopyOut(head_.load(std::memory_order_relaxed) + 8u + offset, dst, n);
}

void MessageRing::Pop() noexcept {
  const std::uint64_t head = head_.load(std::memory_order_relaxed);
  head_.store(head + 8u + ((std::uint64_t{nextBytes_} + 7u) & ~std::uint64_t{7}), std::memory_order_release);
  nextBytes_ = 0;
}

// ---------------------------------------------------------------------------
// TripleSlot
// ---------------------------------------------------------------------------
bool TripleSlot::Init(IAllocator& a, std::uint32_t bytes) noexcept {
  Destroy();
  alloc_ = &a;
  bytes_ = bytes;
  for (auto& b : buffers_) {
    b = static_cast<std::uint8_t*>(a.Allocate(bytes != 0u ? bytes : 16u, 64, MemTag::LiveLink));
    if (b == nullptr) {
      Destroy();
      return false;
    }
    std::memset(b, 0, bytes != 0u ? bytes : 16u);
  }
  back_ = 0;
  front_ = 2;
  middle_.store(1u, std::memory_order_relaxed);
  return true;
}

void TripleSlot::Destroy() noexcept {
  for (auto& b : buffers_) {
    if (alloc_ != nullptr && b != nullptr) alloc_->Free(b, MemTag::LiveLink);
    b = nullptr;
  }
  bytes_ = 0;
}

void TripleSlot::Publish() noexcept {
  const std::uint32_t old = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
  back_ = old & 3u;
}

bool TripleSlot::Take() noexcept {
  if ((middle_.load(std::memory_order_relaxed) & kDirty) == 0u) return false;
  const std::uint32_t old = middle_.exchange(front_, std::memory_order_acq_rel);
  front_ = old & 3u;
  return true;
}

}  // namespace snd::livelink
