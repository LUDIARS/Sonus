// AVAudioSession for the iOS Core Audio device (D-63). C++ interface; the
// implementation is Objective-C++ with ARC (ios_audio_session.mm, compiled
// with -fobjc-arc). Non-RT: owner thread and the device's control queue.
//
// Notifications observed (NSNotificationCenter, delivered on the posting
// thread; forwarded as bits to the device, which handles them on its serial
// queue):
//   AVAudioSessionInterruptionNotification      began -> kIosInterruptBegan, ended -> kIosInterruptEnded
//   AVAudioSessionRouteChangeNotification       kIosRouteChanged (reason in LastRouteReason())
//   AVAudioSessionMediaServicesWereLostNotification   kIosMediaLost
//   AVAudioSessionMediaServicesWereResetNotification  kIosMediaReset (everything must be rebuilt)
#ifndef SND_PLATFORM_SRC_APPLE_IOS_AUDIO_SESSION_HPP
#define SND_PLATFORM_SRC_APPLE_IOS_AUDIO_SESSION_HPP

#include <atomic>
#include <cstdint>

#include "snd/platform/platform_output_device.hpp"

namespace snd::platform::detail::coreaudio {

enum IosSessionBits : std::uint32_t {
  kIosInterruptBegan = 1u << 0,
  kIosInterruptEnded = 1u << 1,
  kIosRouteChanged = 1u << 2,
  kIosMediaLost = 1u << 3,
  kIosMediaReset = 1u << 4,
};

using IosSessionFn = void (*)(void* user, std::uint32_t bits);

struct IosSessionInfo {
  std::uint32_t sampleRate = 0;      // AVAudioSession.sampleRate
  std::uint32_t outputChannels = 0;  // outputNumberOfChannels
  std::uint32_t ioBufferFrames = 0;  // IOBufferDuration x sampleRate
  double outputLatency = 0.0;        // outputLatency (s)
};

class IosAudioSession {
 public:
  IosAudioSession() noexcept = default;
  ~IosAudioSession();
  IosAudioSession(const IosAudioSession&) = delete;
  IosAudioSession& operator=(const IosAudioSession&) = delete;

  // setCategory, preferred rate and IO buffer duration (best effort), setActive:YES.
  bool Activate(SessionCategory category, std::uint32_t preferredRate, std::uint32_t preferredFrames) noexcept;
  bool Reactivate() noexcept;  // setActive:YES after an interruption
  void Deactivate() noexcept;  // setActive:NO (NotifyOthersOnDeactivation)
  IosSessionInfo Query() const noexcept;

  bool StartObserving(IosSessionFn fn, void* user) noexcept;
  void StopObserving() noexcept;  // no forward starts after this returns

  std::uint32_t LastRouteReason() const noexcept { return routeReason_.load(std::memory_order_acquire); }
  bool LastInterruptionShouldResume() const noexcept { return shouldResume_.load(std::memory_order_acquire); }

  // Called by the Objective-C observer (any thread).
  void Forward(std::uint32_t bits, std::uint32_t routeReason, bool shouldResume) noexcept;

 private:
  void* observer_ = nullptr;  // CFBridgingRetain'ed SndSessionObserver
  IosSessionFn sinkFn_ = nullptr;
  void* sinkUser_ = nullptr;
  std::atomic<std::uint32_t> routeReason_{0};
  std::atomic<bool> shouldResume_{false};
};

}  // namespace snd::platform::detail::coreaudio

#endif  // SND_PLATFORM_SRC_APPLE_IOS_AUDIO_SESSION_HPP
