// Lock-free single-producer / single-consumer channels used by the live link
// (design.md 2.1 "生産者ごとの SPSC"). Producer and consumer are fixed threads
// (the audio thread and the live link thread, or the two ends of an in-memory
// pipe). Every producer-side function is wait-free and RT-safe: no allocation,
// no lock, no system call. Memory comes from an IAllocator at Init (non-RT).
//
//   ByteRing      raw bytes (partial writes and reads), for in-memory pipes.
//   MessageRing   length-prefixed messages (all or nothing), for records and
//                 profiler events of variable size.
//   TripleSlot    latest-value mailbox of a fixed-size block of bytes: the
//                 producer overwrites the back buffer and publishes; the
//                 consumer takes the newest one. Never blocks either side.
#ifndef SND_LIVELINK_RINGS_HPP
#define SND_LIVELINK_RINGS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/ext/allocator.hpp"

namespace snd::livelink {

class ByteRing {
 public:
  ByteRing() noexcept = default;
  ~ByteRing() { Destroy(); }
  ByteRing(const ByteRing&) = delete;
  ByteRing& operator=(const ByteRing&) = delete;

  // capacity: rounded up to a power of two.
  bool Init(IAllocator& a, std::uint32_t capacity) noexcept;
  void Destroy() noexcept;
  // Only while neither side uses the ring.
  void Reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  std::size_t Write(const void* src, std::size_t n) noexcept;  // producer: bytes taken
  std::size_t Read(void* dst, std::size_t n) noexcept;         // consumer: bytes read
  std::size_t Readable() const noexcept {
    return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
  }
  std::size_t Writable() const noexcept { return capacity_ - Readable(); }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  IAllocator* alloc_ = nullptr;
  std::uint8_t* data_ = nullptr;
  std::uint32_t capacity_ = 0;
  alignas(64) std::atomic<std::uint64_t> head_{0};  // consumer
  alignas(64) std::atomic<std::uint64_t> tail_{0};  // producer
};

class MessageRing {
 public:
  MessageRing() noexcept = default;
  ~MessageRing() { Destroy(); }
  MessageRing(const MessageRing&) = delete;
  MessageRing& operator=(const MessageRing&) = delete;

  bool Init(IAllocator& a, std::uint32_t capacity) noexcept;
  void Destroy() noexcept;

  // Producer: a message of `bytes` is written with Put (sequentially) after a
  // successful Begin, and becomes visible at Commit. Begin fails (nothing
  // changes) when it does not fit; then the producer counts a drop.
  bool Begin(std::uint32_t bytes) noexcept;
  void Put(const void* src, std::uint32_t n) noexcept;
  void Commit() noexcept;
  bool Push(const void* src, std::uint32_t n) noexcept {
    if (!Begin(n)) return false;
    Put(src, n);
    Commit();
    return true;
  }

  // Consumer: size of the next message (false if none), copy it out, release it.
  bool Next(std::uint32_t* bytes) noexcept;
  void Copy(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept;  // within the next message
  void Pop() noexcept;
  std::uint64_t UsedApprox() const noexcept {
    return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
  }
  std::uint32_t Capacity() const noexcept { return capacity_; }

 private:
  void CopyIn(std::uint64_t at, const void* src, std::uint32_t n) noexcept;
  void CopyOut(std::uint64_t at, void* dst, std::uint32_t n) const noexcept;
  IAllocator* alloc_ = nullptr;
  std::uint8_t* data_ = nullptr;
  std::uint32_t capacity_ = 0;
  // producer private
  std::uint64_t writeAt_ = 0, writeEnd_ = 0;
  // consumer private
  std::uint32_t nextBytes_ = 0;
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
};

class TripleSlot {
 public:
  TripleSlot() noexcept = default;
  ~TripleSlot() { Destroy(); }
  TripleSlot(const TripleSlot&) = delete;
  TripleSlot& operator=(const TripleSlot&) = delete;

  bool Init(IAllocator& a, std::uint32_t bytes) noexcept;
  void Destroy() noexcept;

  // Producer: fill Back() (Bytes() bytes), then Publish().
  std::uint8_t* Back() noexcept { return buffers_[back_]; }
  void Publish() noexcept;
  // Consumer: true if a newer buffer was taken; Front() is then valid until the next Take.
  bool Take() noexcept;
  const std::uint8_t* Front() const noexcept { return buffers_[front_]; }
  std::uint32_t Bytes() const noexcept { return bytes_; }

 private:
  static constexpr std::uint32_t kDirty = 4u;
  IAllocator* alloc_ = nullptr;
  std::uint8_t* buffers_[3] = {};
  std::uint32_t bytes_ = 0;
  std::uint32_t back_ = 0;   // producer private
  std::uint32_t front_ = 2;  // consumer private
  alignas(64) std::atomic<std::uint32_t> middle_{1u};
};

}  // namespace snd::livelink

#endif  // SND_LIVELINK_RINGS_HPP
