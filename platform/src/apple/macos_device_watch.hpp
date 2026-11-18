// macOS device-change listeners (D-63). AudioObjectAddPropertyListener on
// the system object (default output device, device list) and on the current
// device (alive, nominal rate, buffer size). The HAL calls the listener on
// its notification thread; it only forwards a bit to the device (which
// dispatches the work to its serial queue) under a short lock that Stop()
// also takes, so no forward starts after Stop() returns.
#ifndef SND_PLATFORM_SRC_APPLE_MACOS_DEVICE_WATCH_HPP
#define SND_PLATFORM_SRC_APPLE_MACOS_DEVICE_WATCH_HPP

#include <CoreAudio/CoreAudio.h>

#include <cstdint>

#include "snd/base/mutex.hpp"

namespace snd::platform::detail::coreaudio {

enum MacWatchBits : std::uint32_t {
  kMacDefaultChanged = 1u << 0,  // kAudioHardwarePropertyDefaultOutputDevice
  kMacDevicesChanged = 1u << 1,  // kAudioHardwarePropertyDevices
  kMacDeviceDied = 1u << 2,      // kAudioDevicePropertyDeviceIsAlive
  kMacDeviceFormat = 1u << 3,    // kAudioDevicePropertyNominalSampleRate / BufferFrameSize
};

using MacWatchFn = void (*)(void* user, std::uint32_t bits);

class MacDeviceWatch {
 public:
  MacDeviceWatch() noexcept = default;
  ~MacDeviceWatch() { Stop(); }
  MacDeviceWatch(const MacDeviceWatch&) = delete;
  MacDeviceWatch& operator=(const MacDeviceWatch&) = delete;

  void Start(AudioObjectID device, MacWatchFn fn, void* user) noexcept;
  void Retarget(AudioObjectID device) noexcept;  // moves the per-device listeners
  void Stop() noexcept;

 private:
  static OSStatus Listener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress* addresses,
                           void* user);
  void AddDevice(AudioObjectID device) noexcept;
  void RemoveDevice() noexcept;

  base::Mutex lock_;
  MacWatchFn fn_ = nullptr;
  void* user_ = nullptr;
  AudioObjectID device_ = 0;
  bool system_ = false;
};

}  // namespace snd::platform::detail::coreaudio

#endif  // SND_PLATFORM_SRC_APPLE_MACOS_DEVICE_WATCH_HPP
