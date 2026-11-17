// Shared callback invocation for every backend (CONTRACTS.md D-61, D-62).
//
// DeliverFrames calls the DeviceCallback for `frames` frames in pieces of at
// most `maxChunk` frames, each piece with FTZ/DAZ and inside SND_RT_SCOPE,
// and counts callbacks and frames. CallbackMarker marks the calling thread
// as "inside this device's callback / device thread" so Stop()/Close() can
// detect being called from there (they must not join or free).
#ifndef SND_PLATFORM_SRC_COMMON_DEVICE_CALLBACK_HPP
#define SND_PLATFORM_SRC_COMMON_DEVICE_CALLBACK_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/types.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::platform::detail {

// The device object whose callback (or device thread) the calling thread runs.
const void*& CurrentCallbackDevice() noexcept;

class CallbackMarker {
 public:
  explicit CallbackMarker(const void* device) noexcept : prev_(CurrentCallbackDevice()) {
    CurrentCallbackDevice() = device;
  }
  ~CallbackMarker() { CurrentCallbackDevice() = prev_; }
  CallbackMarker(const CallbackMarker&) = delete;
  CallbackMarker& operator=(const CallbackMarker&) = delete;

 private:
  const void* prev_;
};

inline bool CalledFromCallback(const void* device) noexcept { return CurrentCallbackDevice() == device; }

// Number of pieces DeliverFrames uses for `frames` (maxChunk 0 = one piece).
constexpr std::uint32_t ChunkCount(std::uint32_t frames, std::uint32_t maxChunk) noexcept {
  if (frames == 0u) return 0u;
  if (maxChunk == 0u || maxChunk >= frames) return 1u;
  return (frames + maxChunk - 1u) / maxChunk;
}

inline void DeliverFrames(DeviceCallback cb, void* user, float* interleaved, std::uint32_t frames,
                          std::uint32_t channels, std::uint32_t maxChunk, std::atomic<std::uint64_t>& callbacks,
                          std::atomic<std::uint64_t>& framesOut) noexcept {
  if (cb == nullptr || frames == 0u || channels == 0u) return;
  ScopedFlushDenormals ftz;  // 2.4: per callback (Core Audio does not own its thread)
  SND_RT_SCOPE();
  const std::uint32_t chunk = maxChunk == 0u ? frames : maxChunk;
  std::uint32_t done = 0;
  while (done < frames) {
    const std::uint32_t n = frames - done < chunk ? frames - done : chunk;
    cb(user, interleaved + static_cast<std::uint64_t>(done) * channels, n, channels);
    done += n;
    callbacks.store(callbacks.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
  framesOut.store(framesOut.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
}

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_COMMON_DEVICE_CALLBACK_HPP
