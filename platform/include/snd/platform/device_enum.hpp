// Output device enumeration (design.md 2.6, CONTRACTS.md D-64).
//
// DeviceDesc::device_index selects a device: 0 = the system default (and the
// backend follows default-device changes), k >= 1 = entry k-1 of
// EnumerateOutputDevices() for the same backend, in the order returned here.
// The order is the OS's order and may change when devices are plugged in or
// out, so a stored choice should keep the `id` and pass it as
// OutputDeviceOptions::deviceName.
//
//   ALSA:       snd_device_name_hint(-1, "pcm"), entries whose IOID is
//               output or unset; id = NAME ("default", "hw:CARD=...,DEV=0", ...)
//   WASAPI:     IMMDeviceEnumerator::EnumAudioEndpoints(eRender, ACTIVE);
//               id = IMMDevice::GetId (UTF-8), name = PKEY_Device_FriendlyName
//   Core Audio: macOS kAudioHardwarePropertyDevices with output streams;
//               id = kAudioDevicePropertyDeviceUID. iOS: one entry ("default",
//               the current route).
//   Silent timer: one entry ("silent").
//
// Non-RT (OS calls, may allocate inside the OS). Any thread.
#ifndef SND_PLATFORM_DEVICE_ENUM_HPP
#define SND_PLATFORM_DEVICE_ENUM_HPP

#include <cstdint>

#include "snd/platform/platform_output_device.hpp"

namespace snd::platform {

struct OutputDeviceInfo {
  char id[256] = {};    // pass as OutputDeviceOptions::deviceName (UTF-8, truncated if longer)
  char name[128] = {};  // human-readable (UTF-8, truncated)
  bool isDefault = false;  // the current system default output
};

// Writes up to `capacity` entries to `out` (may be null when capacity is 0)
// and returns the number of devices found (may exceed capacity). 0 when the
// backend is not compiled in or the OS reports nothing.
std::uint32_t EnumerateOutputDevices(OutputBackend backend, OutputDeviceInfo* out, std::uint32_t capacity) noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_DEVICE_ENUM_HPP
