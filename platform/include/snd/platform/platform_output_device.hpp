// Common base of the platform output devices (design.md 2.2, 2.6, CONTRACTS.md D-25, D-61..D-66).
//
// Every backend in this module (silent timer, ALSA, WASAPI, Core Audio)
// implements snd::IOutputDevice through PlatformOutputDevice, which adds what
// the contract interface does not carry: the backend id, statistics, the
// output-position clock, the OS workgroup (workgroup.hpp) and one shared
// implementation of the event listener.
//
// Shared behaviour of all backends (D-61..D-63):
//   - The DeviceCallback runs with FTZ/DAZ set and inside SND_RT_SCOPE (the
//     test kit flags allocation, locks and IO in it). The backend itself does
//     not allocate, lock or do IO around the call either; OS calls that may
//     block (snd_pcm_writei, IAudioRenderClient::ReleaseBuffer) are outside
//     the RT scope, on the device thread.
//   - `frames` passed to the callback is 1..PeriodFrames(). An OS request for
//     more frames is split into several callbacks; the OS may also ask for
//     fewer (WASAPI padding, Core Audio slices). Callers that need a fixed
//     block size put the AudioDriver (audio_driver.hpp) in between.
//   - `channels` passed to the callback is the device format at that moment.
//     After a route change it may differ from the previous call; a CHANGED
//     event with the new values is posted before the first callback in the
//     new format.
//   - Stop() returns after the last callback has finished. Stop()/Close()
//     called from inside the callback (or from the device thread) are
//     ignored and counted in Stats().misuseFromCallback; the device keeps
//     running.
//   - Events are posted from the device thread or an OS notification thread.
//     The listener runs under a short internal lock: it must only post
//     (never block, never call back into the device).
#ifndef SND_PLATFORM_PLATFORM_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_PLATFORM_OUTPUT_DEVICE_HPP

#include <atomic>
#include <cstdint>

#include "snd/base/mutex.hpp"
#include "snd/ext/output_device.hpp"
#include "snd/platform/workgroup.hpp"

namespace snd::platform {

enum class OutputBackend : std::uint32_t {
  Default = 0,      // best backend compiled for this OS (see ResolveOutputBackend)
  SilentTimer = 1,  // TimerOutputDevice: no OS audio, timer driven
  Alsa = 2,         // Linux (SND_OUTPUT_ALSA)
  Wasapi = 3,       // Windows
  CoreAudio = 4,    // macOS (AUHAL / DefaultOutput) and iOS (RemoteIO + AVAudioSession)
};

// iOS AVAudioSession category (ignored elsewhere).
enum class SessionCategory : std::uint8_t {
  Playback = 0,     // AVAudioSessionCategoryPlayback: ignores the silent switch, interrupts others
  Ambient = 1,      // AVAudioSessionCategoryAmbient: mixes with others, obeys the silent switch
  SoloAmbient = 2,  // AVAudioSessionCategorySoloAmbient: the iOS default
};

// Backend-specific settings. Everything is optional; zero / null = default.
struct OutputDeviceOptions {
  // Device selected by name instead of DeviceDesc::device_index (D-64):
  //   ALSA: PCM name ("default", "hw:0,0", "plughw:1,0", "pulse", ...)
  //   WASAPI: endpoint id string (IMMDevice::GetId) in UTF-8
  //   Core Audio (macOS): device UID (kAudioDevicePropertyDeviceUID)
  // The id field of OutputDeviceInfo (device_enum.hpp) is this string.
  const char* deviceName = nullptr;
  // Device thread at ThreadPriority::Realtime (ALSA, silent timer). WASAPI
  // always registers with MMCSS; Core Audio's thread belongs to the OS.
  bool realtimeThread = true;
  // After LOST, try to open the device again every `reopenIntervalMs` while
  // started (CHANGED is posted when it succeeds). Default output devices
  // follow the system default instead (D-63).
  bool reopenOnLoss = true;
  std::uint32_t reopenIntervalMs = 500;
  // ALSA: periods in the hardware buffer (0 = 3).
  std::uint32_t bufferPeriods = 0;
  // ALSA: configuration text used INSTEAD of the system configuration
  // (snd_pcm_open_lconf), e.g. "pcm.out { type null }". The text is parsed at
  // Open. Used by tests and by embedded setups without /usr/share/alsa.
  const char* alsaConfig = nullptr;
  // WASAPI: exclusive mode (IAudioClient::Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE)).
  bool exclusive = false;
  // iOS: session category and whether to activate the session at Open.
  SessionCategory sessionCategory = SessionCategory::Playback;
  bool manageSession = true;
};

struct DeviceStats {
  std::uint64_t callbacks = 0;        // DeviceCallback invocations
  std::uint64_t framesDelivered = 0;  // frames produced by the callback
  std::uint64_t xruns = 0;            // OS-side underruns recovered (ALSA -EPIPE, WASAPI glitches)
  std::uint64_t suspends = 0;         // SUSPENDED events posted
  std::uint64_t losses = 0;           // LOST events posted
  std::uint64_t reopens = 0;          // successful re-opens after a loss / default change
  std::uint64_t misuseFromCallback = 0;  // Stop()/Close() from the callback, ignored
};

// Output position (D-62). framesWritten counts frames handed to the OS since
// Open; framesPlayed is the OS's estimate of frames that have left the
// device (written - queued delay, clamped at 0); timestampNs is
// MonotonicNanoseconds() when the estimate was taken.
struct OutputClockSample {
  std::uint64_t framesWritten = 0;
  std::uint64_t framesPlayed = 0;
  std::uint64_t timestampNs = 0;
  std::uint32_t sampleRate = 0;
};

const char* OutputBackendName(OutputBackend b) noexcept;

class PlatformOutputDevice : public IOutputDevice {
 public:
  ~PlatformOutputDevice() override = default;

  virtual OutputBackend Backend() const noexcept = 0;

  // IOutputDevice. Shared implementation (D-25): the listener is replaced
  // under the event lock, so it may be changed while the device runs.
  void SetEventListener(DeviceEventFn fn, void* user) override;

  // Any thread.
  DeviceStats Stats() const noexcept;
  // false before the first callback after Open.
  bool OutputClock(OutputClockSample* out) const noexcept;
  // The workgroup RT threads should join (workgroup.hpp) and its generation;
  // the generation changes whenever the workgroup is replaced (device change).
  Workgroup CurrentWorkgroup(std::uint32_t* generation = nullptr) const noexcept;
  std::uint32_t WorkgroupGeneration() const noexcept { return wgGeneration_.load(std::memory_order_acquire); }

 protected:
  PlatformOutputDevice() noexcept = default;

  // Posts an event to the listener (device thread / OS notification thread; non-RT).
  void PostEvent(DeviceEventKind kind, std::uint32_t channels, std::uint32_t rate, std::uint32_t period) noexcept;
  // Single writer (the device thread or the OS render thread). RT-safe.
  void PublishClock(std::uint64_t written, std::uint64_t queued, std::uint32_t rate) noexcept;
  void ResetClock() noexcept;
  // Non-RT. Publishes a new workgroup (generation + 1).
  void PublishWorkgroup(const Workgroup& group) noexcept;

  // Counters (relaxed atomics; any thread reads them through Stats()).
  std::atomic<std::uint64_t> statCallbacks_{0};
  std::atomic<std::uint64_t> statFrames_{0};
  std::atomic<std::uint64_t> statXruns_{0};
  std::atomic<std::uint64_t> statSuspends_{0};
  std::atomic<std::uint64_t> statLosses_{0};
  std::atomic<std::uint64_t> statReopens_{0};
  std::atomic<std::uint64_t> statMisuse_{0};

 private:
  base::Mutex eventLock_;
  DeviceEventFn listener_ = nullptr;
  void* listenerUser_ = nullptr;

  // Seqlock over the clock sample (single writer, any reader).
  std::atomic<std::uint32_t> clockSeq_{0};
  std::atomic<std::uint64_t> clockWritten_{0};
  std::atomic<std::uint64_t> clockPlayed_{0};
  std::atomic<std::uint64_t> clockNs_{0};
  std::atomic<std::uint32_t> clockRate_{0};

  // Workgroup: replaced only by PublishWorkgroup (non-RT, one writer at a time).
  std::atomic<std::uint32_t> wgSeq_{0};
  std::atomic<std::uint32_t> wgGeneration_{0};
  std::atomic<std::uint8_t> wgKind_{0};
  std::atomic<void*> wgNative_{nullptr};
  std::atomic<std::uint32_t> wgTask_{0};
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_PLATFORM_OUTPUT_DEVICE_HPP
