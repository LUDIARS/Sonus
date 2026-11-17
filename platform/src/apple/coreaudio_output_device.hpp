// Core Audio output device (macOS and iOS, CONTRACTS.md D-63).
//
// The AudioUnit's render callback (on the OS IO thread) calls the
// DeviceCallback directly in pieces of at most PeriodFrames(), with FTZ/DAZ
// and inside SND_RT_SCOPE. The client format is interleaved float32 at the
// requested rate and channel count (0 = the device's / session's); the unit
// converts to the hardware (rate and channel mapping are Core Audio's).
//
//   macOS, device_index 0 and no deviceName: kAudioUnitSubType_DefaultOutput,
//          which follows the system default output by itself;
//   macOS, a chosen device (UID or index): kAudioUnitSubType_HALOutput +
//          kAudioOutputUnitProperty_CurrentDevice;
//   iOS:   kAudioUnitSubType_RemoteIO with the AVAudioSession activated
//          (category from OutputDeviceOptions, preferred rate and IO buffer).
//
// Device changes are handled on a private serial dispatch queue (the
// listeners and notifications only post a bit), under the control lock that
// Open/Start/Stop/Close also take:
//   macOS default changed -> refresh period / latency / workgroup, re-set the
//         client format if the channel count follows the device, CHANGED;
//   macOS chosen device died -> LOST; it comes back when a device with the
//         same UID appears again (reopenOnLoss) -> CHANGED;
//   macOS nominal rate / buffer size changed -> CHANGED (client format kept);
//   iOS interruption began -> SUSPENDED (the system stopped the unit);
//       ended -> setActive:YES, restart if started -> RESUMED;
//   iOS route change -> CHANGED / LOST by reason (RouteChangeEvent);
//   iOS media services reset -> LOST, rebuild the session and the unit -> CHANGED.
// The workgroup (D-65) is the unit's kAudioOutputUnitProperty_OSWorkgroup,
// re-read after every change. Replaced workgroups stay retained until Close
// so threads that joined them never see a freed object.
#ifndef SND_PLATFORM_SRC_APPLE_COREAUDIO_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_SRC_APPLE_COREAUDIO_OUTPUT_DEVICE_HPP

#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdint>

#include "apple/coreaudio_unit.hpp"
#include "common/owned_string.hpp"
#include "snd/base/mutex.hpp"
#include "snd/platform/platform_output_device.hpp"

#if TARGET_OS_IPHONE
#include "apple/ios_audio_session.hpp"
#else
#include "apple/macos_device_watch.hpp"
#endif

namespace snd::platform::detail::coreaudio {

class CoreAudioOutputDevice final : public PlatformOutputDevice {
 public:
  CoreAudioOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
  ~CoreAudioOutputDevice() override;
  CoreAudioOutputDevice(const CoreAudioOutputDevice&) = delete;
  CoreAudioOutputDevice& operator=(const CoreAudioOutputDevice&) = delete;

  bool Init(const OutputDeviceOptions& options) noexcept;

  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override { return period_.load(std::memory_order_relaxed); }
  std::uint32_t SampleRate() const override { return rate_.load(std::memory_order_relaxed); }
  std::uint32_t Channels() const override { return channels_.load(std::memory_order_relaxed); }
  double OutputLatencySeconds() const override { return latency_.load(std::memory_order_relaxed); }
  OutputBackend Backend() const noexcept override { return OutputBackend::CoreAudio; }

 private:
  static OSStatus RenderThunk(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                              UInt32 frames, AudioBufferList* io);
  OSStatus Render(AudioUnitRenderActionFlags* flags, UInt32 frames, AudioBufferList* io) noexcept;

  static void OnOsEvent(void* self, std::uint32_t bits);  // listener / notification thread
  static void ProcessEventsThunk(void* self);              // control queue
  void ProcessEvents() noexcept;

  // Under control_.
  bool BuildUnit() noexcept;        // create, configure, initialize (not started)
  void RefreshRoute() noexcept;     // period, latency, workgroup from the current device / session
  bool Reconfigure(std::uint32_t channels) noexcept;  // new client channel count (stop, re-init, restart)
  void StopUnit() noexcept;         // AudioOutputUnitStop + wait for the last render to finish
  bool StartUnit() noexcept;
  void RefreshWorkgroup() noexcept;
  void ReleaseWorkgroups() noexcept;

  IAllocator& allocator_;
  OwnedString optionName_;
  bool reopenOnLoss_ = true;
#if TARGET_OS_IPHONE
  SessionCategory category_ = SessionCategory::Playback;
  bool manageSession_ = true;
#endif

  base::Mutex control_;
  dispatch_queue_t queue_ = nullptr;
  std::atomic<std::uint32_t> pendingBits_{0};

  OutputUnit unit_;
  DeviceDesc request_{};
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;
  std::uint32_t clientChannels_ = 0;
  bool followDefault_ = true;
  bool open_ = false;
  bool wantRunning_ = false;
  bool lost_ = false;
#if TARGET_OS_IPHONE
  IosAudioSession session_;
  bool sessionActive_ = false;
#else
  MacDeviceWatch watch_;
  AudioObjectID device_ = 0;  // chosen device (followDefault_ == false) / current default
  char deviceUid_[256] = {};  // UID of the chosen device (to find it again after a loss)
#endif

  // Render thread <-> control.
  std::atomic<bool> running_{false};
  std::atomic<std::uint32_t> active_{0};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint32_t> latencyFrames_{0};

  // Workgroups: current + replaced ones (released at Close).
  static constexpr std::uint32_t kMaxRetiredWorkgroups = 16;
  void* workgroups_[kMaxRetiredWorkgroups] = {};
  std::uint32_t workgroupCount_ = 0;

  std::atomic<std::uint32_t> rate_{0};
  std::atomic<std::uint32_t> channels_{0};
  std::atomic<std::uint32_t> period_{0};
  std::atomic<double> latency_{0.0};
};

}  // namespace snd::platform::detail::coreaudio

#endif  // SND_PLATFORM_SRC_APPLE_COREAUDIO_OUTPUT_DEVICE_HPP
