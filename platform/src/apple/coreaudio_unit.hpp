// Thin wrapper over the output AudioUnit (D-63): DefaultOutput / HALOutput
// on macOS, RemoteIO on iOS. Non-RT (owner thread / control queue).
#ifndef SND_PLATFORM_SRC_APPLE_COREAUDIO_UNIT_HPP
#define SND_PLATFORM_SRC_APPLE_COREAUDIO_UNIT_HPP

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#include <cstdint>

namespace snd::platform::detail::coreaudio {

enum class UnitKind : std::uint8_t {
  DefaultOutput = 0,  // macOS: follows the system default output device by itself
  HalOutput = 1,      // macOS: a chosen device (kAudioOutputUnitProperty_CurrentDevice)
  RemoteIo = 2,       // iOS: the current route of the AVAudioSession
};

class OutputUnit {
 public:
  OutputUnit() noexcept = default;
  ~OutputUnit() { Dispose(); }
  OutputUnit(const OutputUnit&) = delete;
  OutputUnit& operator=(const OutputUnit&) = delete;

  OSStatus Create(UnitKind kind) noexcept;  // AudioComponentFindNext + AudioComponentInstanceNew
  void Dispose() noexcept;                  // Stop + Uninitialize + AudioComponentInstanceDispose
  bool Valid() const noexcept { return unit_ != nullptr; }
  AudioUnit Get() const noexcept { return unit_; }

#if !TARGET_OS_IPHONE
  OSStatus SetDevice(AudioObjectID device) noexcept;  // before Initialize
  AudioObjectID CurrentDevice() const noexcept;       // 0 if unknown
  // Device IO buffer (frames at the device rate), forwarded by AUHAL to the device.
  OSStatus SetDeviceBufferFrames(std::uint32_t frames) noexcept;
#endif
  // Client format of the render callback: interleaved native float32.
  OSStatus SetClientFormat(std::uint32_t sampleRate, std::uint32_t channels) noexcept;
  OSStatus SetRenderCallback(AURenderCallback fn, void* user) noexcept;
  OSStatus SetMaxFramesPerSlice(std::uint32_t frames) noexcept;
  std::uint32_t MaxFramesPerSlice() const noexcept;
  double UnitLatencySeconds() const noexcept;  // kAudioUnitProperty_Latency (sample-rate conversion)

  OSStatus Initialize() noexcept;
  void Uninitialize() noexcept;
  OSStatus Start() noexcept;  // AudioOutputUnitStart
  void Stop() noexcept;       // AudioOutputUnitStop

  // Retained os_workgroup_t of the IO thread (kAudioOutputUnitProperty_OSWorkgroup,
  // macOS 11 / iOS 14), or null. The caller os_release()s it.
  void* CopyWorkgroup() const noexcept;

 private:
  AudioUnit unit_ = nullptr;
  bool initialized_ = false;
  bool started_ = false;
};

}  // namespace snd::platform::detail::coreaudio

#endif  // SND_PLATFORM_SRC_APPLE_COREAUDIO_UNIT_HPP
