// Output ring between the audio thread and the device callback (design.md 2.2).
//
// Single producer (the audio thread writes rendered blocks) and single
// consumer (the device callback copies out). Indices count float SAMPLES
// (not frames) and grow monotonically (64 bits: never wrap in practice), so
// the ring can hold interleaved data of any channel count; DriverCore
// changes the channel count with a handshake (driver_core.hpp), the ring
// itself is format agnostic. Capacity is a power of two in samples.
//
// RT-safe after Bind: no allocation, no lock. Memory order: Write's release
// store of the write index happens-before the reader's acquire load, so the
// samples are visible; Read's release store of the read index lets the
// writer reuse the space.
#ifndef SND_PLATFORM_OUTPUT_RING_HPP
#define SND_PLATFORM_OUTPUT_RING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/base/atomic.hpp"

namespace snd::platform {

class OutputRing {
 public:
  OutputRing() noexcept = default;
  OutputRing(const OutputRing&) = delete;
  OutputRing& operator=(const OutputRing&) = delete;

  // Setup only (no reader or writer running). capacitySamples: power of two.
  bool Bind(float* storage, std::uint64_t capacitySamples) noexcept {
    if (storage == nullptr || !base::IsPowerOfTwo(capacitySamples)) return false;
    data_ = storage;
    capacity_ = capacitySamples;
    mask_ = capacitySamples - 1u;
    write_.value.store(0, std::memory_order_relaxed);
    read_.value.store(0, std::memory_order_relaxed);
    return true;
  }
  std::uint64_t Capacity() const noexcept { return capacity_; }

  // --- writer (one thread) ---
  std::uint64_t WriteIndex() const noexcept { return write_.value.load(std::memory_order_relaxed); }
  // Samples queued as seen by the writer (an upper bound: the reader may have taken more).
  std::uint64_t QueuedForWriter() const noexcept {
    return WriteIndex() - read_.value.load(std::memory_order_acquire);
  }
  std::uint64_t ReadIndexAcquire() const noexcept { return read_.value.load(std::memory_order_acquire); }
  // Copies `samples` and publishes them. Caller ensures they fit (Capacity - QueuedForWriter).
  void Write(const float* src, std::uint64_t samples) noexcept {
    const std::uint64_t w = WriteIndex();
    CopyIn(w, src, samples);
    write_.value.store(w + samples, std::memory_order_release);
  }

  // --- reader (one thread) ---
  std::uint64_t ReadIndex() const noexcept { return read_.value.load(std::memory_order_relaxed); }
  // Acquire-loads the write index: everything below it is readable.
  std::uint64_t AcquireWriteIndex() const noexcept { return write_.value.load(std::memory_order_acquire); }
  // Copies `samples` starting at the read index and releases them.
  void Read(float* dst, std::uint64_t samples) noexcept {
    const std::uint64_t r = ReadIndex();
    CopyOut(r, dst, samples);
    read_.value.store(r + samples, std::memory_order_release);
  }
  // Drops everything below `index` (index <= the acquired write index).
  void SkipTo(std::uint64_t index) noexcept { read_.value.store(index, std::memory_order_release); }

 private:
  void CopyIn(std::uint64_t at, const float* src, std::uint64_t n) noexcept {
    const std::uint64_t off = at & mask_;
    const std::uint64_t first = n < capacity_ - off ? n : capacity_ - off;
    std::memcpy(data_ + off, src, static_cast<std::size_t>(first) * sizeof(float));
    if (n > first) std::memcpy(data_, src + first, static_cast<std::size_t>(n - first) * sizeof(float));
  }
  void CopyOut(std::uint64_t at, float* dst, std::uint64_t n) const noexcept {
    const std::uint64_t off = at & mask_;
    const std::uint64_t first = n < capacity_ - off ? n : capacity_ - off;
    std::memcpy(dst, data_ + off, static_cast<std::size_t>(first) * sizeof(float));
    if (n > first) std::memcpy(dst + first, data_, static_cast<std::size_t>(n - first) * sizeof(float));
  }

  float* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t mask_ = 0;
  base::CachePadded<std::atomic<std::uint64_t>> write_;
  base::CachePadded<std::atomic<std::uint64_t>> read_;
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_OUTPUT_RING_HPP
