// Audio thread driven by an output device (design.md 2.2, 2.4, 2.6, CONTRACTS.md D-66).
//
// AudioDriver = DriverCore (driver_core.hpp) + the audio thread + the wiring
// to an IOutputDevice. It does not know what is rendered: core passes a
// RenderFn (one block of interleaved float32 at the device rate and channel
// count). Usage (core / api, on the owner thread):
//
//   platform::AudioDriverDesc d;
//   d.core.blockFrames = cfg.blockSize;  d.core.renderAheadBlocks = cfg.renderAheadBlocks;
//   d.core.render = &RenderBlock;  d.core.formatChanged = &OnFormat;  d.core.user = sys;
//   d.listener = &OnDeviceEvent;  d.listenerUser = sys;          // optional, forwarded
//   d.workgroupSource = platformDeviceOrNull;                    // join the OS workgroup
//   snd::DeviceDesc dd{cfg.sampleRate, cfg.outputChannels, 0, cfg.output.deviceIndex};
//   if (!driver.Start(*device, dd, d, allocator)) { ... SND_ERR_NO_DEVICE ... }
//   ...
//   driver.Stop();   // device stopped and closed, audio thread joined
//
// Start: sets the device listener, opens the device, sizes the ring from the
// device's format, starts the audio thread (Realtime, D-47 stack, FTZ/DAZ),
// which joins the workgroup, announces the format (FormatFn), prefills the
// ring to the target and only then the device is started. Stop: stops the
// device (no callback after it), wakes and joins the audio thread, closes
// the device and clears the listener.
//
// The audio thread renders inside SND_RT_SCOPE. It sleeps on a Semaphore
// that every device callback posts (2.2: the callback copies, then wakes the
// audio thread). Events: CHANGED updates the period / rate (DriverCore),
// RESUMED triggers a prefill wake; all events are forwarded to `listener`.
// Calling Stop() from the render function or the device callback is
// ignored and counted (MisuseCount), like the devices.
#ifndef SND_PLATFORM_AUDIO_DRIVER_HPP
#define SND_PLATFORM_AUDIO_DRIVER_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/ext/output_device.hpp"
#include "snd/platform/driver_core.hpp"
#include "snd/platform/platform_output_device.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"
#include "snd/platform/workgroup.hpp"

namespace snd::platform {

struct AudioDriverDesc {
  DriverCoreDesc core{};
  DeviceEventFn listener = nullptr;  // optional; device events are forwarded (any thread, post only)
  void* listenerUser = nullptr;
  // Optional: the device whose workgroup the audio thread joins (and rejoins
  // when its generation changes). Usually the same object as the device.
  const PlatformOutputDevice* workgroupSource = nullptr;
  bool realtimeThread = true;          // ThreadPriority::Realtime with fallback
  // Start fails if the first prefill takes longer. It still waits for the
  // render call in progress to return (the render function is never
  // abandoned mid-block); no further block is rendered after the timeout.
  std::uint32_t prefillTimeoutMs = 2000;
};

struct AudioDriverStats {
  DriverStats core{};
  std::uint64_t wakeups = 0;            // audio-thread wake-ups (semaphore)
  std::uint64_t events = 0;             // device events received
  std::uint64_t misuse = 0;             // Stop() from the audio thread / device callback
  PriorityResult priority = PriorityResult::Applied;
  WorkgroupJoinResult workgroup = WorkgroupJoinResult::NoWorkgroup;
};

class AudioDriver {
 public:
  AudioDriver() noexcept = default;
  ~AudioDriver();  // Stop()
  AudioDriver(const AudioDriver&) = delete;
  AudioDriver& operator=(const AudioDriver&) = delete;

  // Owner thread. false (and everything undone) if the device does not open
  // or start, the format is unusable, allocation fails or the prefill times out.
  bool Start(IOutputDevice& device, const DeviceDesc& deviceDesc, const AudioDriverDesc& desc,
             IAllocator& allocator) noexcept;
  // Owner thread. No-op if not started.
  void Stop() noexcept;
  // Owner thread. Resumes a device that stays SUSPENDED until the app asks
  // (iOS: an interruption that ended without ShouldResume, D-63): like
  // Start(), it waits (at most prefillTimeoutMs) until the audio thread has
  // filled the render-ahead, then calls the device's Start() again (2.6).
  // On time-out it waits for the block being rendered to return, renders no
  // more, leaves the device stopped and returns false (Resume may be
  // retried). The device posts RESUMED when it plays again. When the OS
  // resumes by itself (RESUMED without Resume(), e.g. iOS ShouldResume) the
  // prefill cannot be awaited: a short ring is handled as an underrun
  // (fade-in, lostFrames). The SUSPENDED event is forwarded to `listener` as
  // is; api calls this from snd_resume. false if not running or the device
  // refuses.
  bool Resume() noexcept;
  bool Running() const noexcept { return device_ != nullptr; }

  // Any thread.
  AudioDriverStats Stats() const noexcept;
  std::uint32_t LoadLevel() const noexcept { return core_.LoadLevel(); }
  std::uint64_t MisuseCount() const noexcept { return misuse_.load(std::memory_order_relaxed); }
  const DriverCore& Core() const noexcept { return core_; }

 private:
  static void DeviceCallbackThunk(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels);
  static void DeviceEventThunk(void* user, const DeviceEvent* event);
  static void ThreadMain(void* self) noexcept;
  void Run() noexcept;

  DriverCore core_;
  AudioDriverDesc desc_{};
  IOutputDevice* device_ = nullptr;
  std::uint32_t deviceRate_ = 0;    // at Start (thread time constraint)
  std::uint32_t devicePeriod_ = 0;
  Thread thread_;
  Semaphore wake_{0};
  Semaphore ready_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> prefillRequested_{false};
  std::atomic<bool> prefilled_{false};
  std::atomic<bool> resumeWaiting_{false};
  std::atomic<std::uint64_t> wakeups_{0};
  std::atomic<std::uint64_t> events_{0};
  std::atomic<std::uint64_t> misuse_{0};
  std::atomic<std::uint8_t> wgResult_{static_cast<std::uint8_t>(WorkgroupJoinResult::NoWorkgroup)};
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_AUDIO_DRIVER_HPP
