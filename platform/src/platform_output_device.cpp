#include "snd/platform/platform_output_device.hpp"

#include <atomic>

#include "snd/platform/clock.hpp"

namespace snd::platform {

const char* OutputBackendName(OutputBackend b) noexcept {
  switch (b) {
    case OutputBackend::Default: return "default";
    case OutputBackend::SilentTimer: return "silent-timer";
    case OutputBackend::Alsa: return "alsa";
    case OutputBackend::Wasapi: return "wasapi";
    case OutputBackend::CoreAudio: return "coreaudio";
    case OutputBackend::Manual: return "manual";
  }
  return "?";
}

void PlatformOutputDevice::SetEventListener(DeviceEventFn fn, void* user) {
  base::LockGuard g(eventLock_);
  listener_ = fn;
  listenerUser_ = user;
}

void PlatformOutputDevice::PostEvent(DeviceEventKind kind, std::uint32_t channels, std::uint32_t rate,
                                     std::uint32_t period) noexcept {
  if (kind == DeviceEventKind::Suspended) statSuspends_.fetch_add(1, std::memory_order_relaxed);
  if (kind == DeviceEventKind::Lost) statLosses_.fetch_add(1, std::memory_order_relaxed);
  DeviceEvent e{static_cast<std::uint32_t>(kind), channels, rate, period};
  base::LockGuard g(eventLock_);
  if (listener_ != nullptr) listener_(listenerUser_, &e);
}

DeviceStats PlatformOutputDevice::Stats() const noexcept {
  DeviceStats s;
  s.callbacks = statCallbacks_.load(std::memory_order_relaxed);
  s.framesDelivered = statFrames_.load(std::memory_order_relaxed);
  s.xruns = statXruns_.load(std::memory_order_relaxed);
  s.suspends = statSuspends_.load(std::memory_order_relaxed);
  s.losses = statLosses_.load(std::memory_order_relaxed);
  s.reopens = statReopens_.load(std::memory_order_relaxed);
  s.misuseFromCallback = statMisuse_.load(std::memory_order_relaxed);
  return s;
}

void PlatformOutputDevice::PublishClock(std::uint64_t written, std::uint64_t queued, std::uint32_t rate) noexcept {
  // Odd count first; the RMW's acquire half keeps the field stores below after it (no fence:
  // TSan does not model fences).
  const std::uint32_t s = clockSeq_.fetch_add(1u, std::memory_order_acq_rel);
  clockWritten_.store(written, std::memory_order_release);
  clockPlayed_.store(written > queued ? written - queued : 0u, std::memory_order_release);
  clockNs_.store(MonotonicNanoseconds(), std::memory_order_release);
  clockRate_.store(rate, std::memory_order_release);
  clockSeq_.store(s + 2u, std::memory_order_release);
}

void PlatformOutputDevice::ResetClock() noexcept {
  // Odd count first; the RMW's acquire half keeps the field stores below after it (no fence:
  // TSan does not model fences).
  const std::uint32_t s = clockSeq_.fetch_add(1u, std::memory_order_acq_rel);
  clockWritten_.store(0, std::memory_order_release);
  clockPlayed_.store(0, std::memory_order_release);
  clockNs_.store(0, std::memory_order_release);
  clockRate_.store(0, std::memory_order_release);
  clockSeq_.store(s + 2u, std::memory_order_release);
}

bool PlatformOutputDevice::OutputClock(OutputClockSample* out) const noexcept {
  if (out == nullptr) return false;
  // Seqlock reader: retry until a consistent sample is read (the single writer holds the
  // odd count only for a few stores), yielding while it writes. Never a false "no clock".
  for (;;) {
    const std::uint32_t s1 = clockSeq_.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) {
      YieldThread();
      continue;
    }
    OutputClockSample v;
    // Acquire loads: the second read of the count below cannot move above them.
    v.framesWritten = clockWritten_.load(std::memory_order_acquire);
    v.framesPlayed = clockPlayed_.load(std::memory_order_acquire);
    v.timestampNs = clockNs_.load(std::memory_order_acquire);
    v.sampleRate = clockRate_.load(std::memory_order_acquire);
    if (clockSeq_.load(std::memory_order_relaxed) != s1) {
      YieldThread();
      continue;
    }
    *out = v;
    return v.timestampNs != 0u;
  }
}

void PlatformOutputDevice::PublishWorkgroup(const Workgroup& group) noexcept {
  const std::uint32_t s = wgSeq_.load(std::memory_order_relaxed);
  wgSeq_.store(s + 1u, std::memory_order_relaxed);
  wgKind_.store(static_cast<std::uint8_t>(group.kind), std::memory_order_release);
  wgNative_.store(group.native, std::memory_order_release);
  wgTask_.store(group.taskIndex, std::memory_order_release);
  wgSeq_.store(s + 2u, std::memory_order_release);
  wgGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

Workgroup PlatformOutputDevice::CurrentWorkgroup(std::uint32_t* generation) const noexcept {
  Workgroup g;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::uint32_t gen = wgGeneration_.load(std::memory_order_acquire);
    const std::uint32_t s1 = wgSeq_.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) continue;
    Workgroup v;
    v.kind = static_cast<WorkgroupKind>(wgKind_.load(std::memory_order_acquire));
    v.native = wgNative_.load(std::memory_order_acquire);
    v.taskIndex = wgTask_.load(std::memory_order_acquire);
    if (wgSeq_.load(std::memory_order_relaxed) != s1) continue;
    if (generation != nullptr) *generation = gen;
    return v;
  }
  if (generation != nullptr) *generation = wgGeneration_.load(std::memory_order_acquire);
  return g;
}

}  // namespace snd::platform
