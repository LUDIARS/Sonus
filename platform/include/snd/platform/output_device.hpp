// Output devices (design.md 2.2, 2.6, CONTRACTS.md D-25, D-61..D-66).
//
// Backends (one file set per OS under platform/src/<os>/, chosen at build time):
//   SilentTimer  every OS   timer_output_device.hpp
//   Alsa         Linux      snd_pcm_* on a dedicated thread (SND_OUTPUT_ALSA, default AUTO)
//   Wasapi       Windows    shared-mode event-driven (IAudioClient3 low latency when
//                           possible), exclusive mode by option, MMCSS, IMMNotificationClient
//   CoreAudio    macOS/iOS  AudioUnit render callback (DefaultOutput / HALOutput / RemoteIO),
//                           AudioObject listeners (macOS), AVAudioSession (iOS)
//
// OutputBackend::Default resolves to the best compiled backend of the OS
// (ResolveOutputBackend). It never falls back to the silent timer by itself:
// if the OS device does not open, Open() fails and the caller decides (D-61).
//
// Shared behaviour of every backend: platform_output_device.hpp.
#ifndef SND_PLATFORM_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_OUTPUT_DEVICE_HPP

#include "snd/ext/allocator.hpp"
#include "snd/platform/device_enum.hpp"
#include "snd/platform/platform_output_device.hpp"
#include "snd/platform/timer_output_device.hpp"

namespace snd::platform {

// True if `backend` is compiled into this build (Default and SilentTimer always are).
bool OutputBackendAvailable(OutputBackend backend) noexcept;
// Default -> the OS backend (Linux: Alsa if compiled, else SilentTimer;
// Windows: Wasapi; Apple: CoreAudio; other OSes: SilentTimer). Others unchanged.
OutputBackend ResolveOutputBackend(OutputBackend backend) noexcept;

// Non-RT factory. Returns nullptr if the backend is not compiled in or on
// allocation failure. `options` may be null (all defaults); the strings it
// points to are copied. The device lives in `allocator` memory
// (MemTag::Misc) until DestroyOutputDevice; buffers it allocates at Open
// come from the same allocator.
PlatformOutputDevice* CreateOutputDevice(OutputBackend backend, IAllocator& allocator,
                                         const OutputDeviceOptions* options = nullptr) noexcept;
void DestroyOutputDevice(IOutputDevice* device, IAllocator& allocator) noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_OUTPUT_DEVICE_HPP
