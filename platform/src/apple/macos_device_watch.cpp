#include "apple/macos_device_watch.hpp"

#include "apple/macos_devices.hpp"

namespace snd::platform::detail::coreaudio {

namespace {
constexpr AudioObjectPropertyAddress kSystemProps[] = {
    {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kElementMain},
    {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kElementMain},
};
constexpr AudioObjectPropertyAddress kDeviceProps[] = {
    {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kElementMain},
    {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kElementMain},
    {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kElementMain},
};
}  // namespace

OSStatus MacDeviceWatch::Listener(AudioObjectID, UInt32 count, const AudioObjectPropertyAddress* addresses,
                                  void* user) {
  auto* self = static_cast<MacDeviceWatch*>(user);
  std::uint32_t bits = 0;
  for (UInt32 i = 0; i < count; ++i) {
    switch (addresses[i].mSelector) {
      case kAudioHardwarePropertyDefaultOutputDevice: bits |= kMacDefaultChanged; break;
      case kAudioHardwarePropertyDevices: bits |= kMacDevicesChanged; break;
      case kAudioDevicePropertyDeviceIsAlive: bits |= kMacDeviceDied; break;
      case kAudioDevicePropertyNominalSampleRate:
      case kAudioDevicePropertyBufferFrameSize: bits |= kMacDeviceFormat; break;
      default: break;
    }
  }
  if (bits == 0u) return noErr;
  base::LockGuard g(self->lock_);
  if (self->fn_ != nullptr) self->fn_(self->user_, bits);
  return noErr;
}

void MacDeviceWatch::AddDevice(AudioObjectID device) noexcept {
  device_ = device;
  if (device_ == 0) return;
  for (const AudioObjectPropertyAddress& a : kDeviceProps) AudioObjectAddPropertyListener(device_, &a, &Listener, this);
}

void MacDeviceWatch::RemoveDevice() noexcept {
  if (device_ == 0) return;
  for (const AudioObjectPropertyAddress& a : kDeviceProps) {
    AudioObjectRemovePropertyListener(device_, &a, &Listener, this);
  }
  device_ = 0;
}

void MacDeviceWatch::Start(AudioObjectID device, MacWatchFn fn, void* user) noexcept {
  Stop();
  {
    base::LockGuard g(lock_);
    fn_ = fn;
    user_ = user;
  }
  for (const AudioObjectPropertyAddress& a : kSystemProps) {
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, &Listener, this);
  }
  system_ = true;
  AddDevice(device);
}

void MacDeviceWatch::Retarget(AudioObjectID device) noexcept {
  if (device == device_) return;
  RemoveDevice();
  AddDevice(device);
}

void MacDeviceWatch::Stop() noexcept {
  {
    base::LockGuard g(lock_);
    fn_ = nullptr;  // no forward starts after this
    user_ = nullptr;
  }
  RemoveDevice();
  if (system_) {
    for (const AudioObjectPropertyAddress& a : kSystemProps) {
      AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, &Listener, this);
    }
  }
  system_ = false;
}

}  // namespace snd::platform::detail::coreaudio
