// macOS audio devices through the AudioObject API (D-63, D-64). Non-RT.
#ifndef SND_PLATFORM_SRC_APPLE_MACOS_DEVICES_HPP
#define SND_PLATFORM_SRC_APPLE_MACOS_DEVICES_HPP

#include <CoreAudio/CoreAudio.h>

#include <cstdint>

namespace snd::platform::detail::coreaudio {

// kAudioObjectPropertyElementMain (macOS 12) == kAudioObjectPropertyElementMaster == 0.
inline constexpr AudioObjectPropertyElement kElementMain = 0;

struct MacDeviceInfo {
  std::uint32_t nominalRate = 0;     // kAudioDevicePropertyNominalSampleRate
  std::uint32_t outputChannels = 0;  // sum of kAudioDevicePropertyStreamConfiguration (output)
  std::uint32_t bufferFrames = 0;    // kAudioDevicePropertyBufferFrameSize
  // device latency + safety offset + buffer + first output stream latency (frames at nominalRate)
  std::uint32_t latencyFrames = 0;
  bool alive = false;                // kAudioDevicePropertyDeviceIsAlive
};

AudioObjectID MacDefaultOutputDevice() noexcept;  // 0 if none
// Output-capable devices in kAudioHardwarePropertyDevices order (the
// enumeration order of D-64). Returns the count; writes up to `capacity`.
std::uint32_t MacOutputDevices(AudioObjectID* out, std::uint32_t capacity) noexcept;
bool MacFindDeviceByUid(const char* uid, AudioObjectID* out) noexcept;
bool MacQueryDevice(AudioObjectID device, MacDeviceInfo* out) noexcept;
// UTF-8 copies of kAudioDevicePropertyDeviceUID / kAudioObjectPropertyName.
bool MacDeviceUid(AudioObjectID device, char* out, std::uint32_t bytes) noexcept;
bool MacDeviceName(AudioObjectID device, char* out, std::uint32_t bytes) noexcept;

}  // namespace snd::platform::detail::coreaudio

#endif  // SND_PLATFORM_SRC_APPLE_MACOS_DEVICES_HPP
