// Backend constructors and enumerators, one set per OS (chosen by CMake:
// SND_PLATFORM_HAVE_ALSA, _WIN32, __APPLE__). Internal to the platform module.
#ifndef SND_PLATFORM_SRC_BACKENDS_HPP
#define SND_PLATFORM_SRC_BACKENDS_HPP

#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/platform/device_enum.hpp"
#include "snd/platform/platform_output_device.hpp"

#if defined(_WIN32)
#define SND_PLATFORM_HAVE_WASAPI 1
#endif
#if defined(__APPLE__)
#define SND_PLATFORM_HAVE_COREAUDIO 1
#endif

namespace snd::platform::detail {

#if defined(SND_PLATFORM_HAVE_ALSA)
PlatformOutputDevice* CreateAlsaOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
std::uint32_t EnumerateAlsaDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept;
#endif
#if defined(SND_PLATFORM_HAVE_WASAPI)
PlatformOutputDevice* CreateWasapiOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
std::uint32_t EnumerateWasapiDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept;
#endif
#if defined(SND_PLATFORM_HAVE_COREAUDIO)
PlatformOutputDevice* CreateCoreAudioOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
std::uint32_t EnumerateCoreAudioDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept;
#endif

// Allocates and constructs T in allocator memory (MemTag::Misc); null on failure.
template <class T, class... Args>
T* NewDevice(IAllocator& allocator, Args&&... args) noexcept {
  void* p = allocator.Allocate(sizeof(T), alignof(T) > 16 ? alignof(T) : 16, MemTag::Misc);
  if (p == nullptr) return nullptr;
  return ::new (p) T(static_cast<Args&&>(args)...);
}

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_BACKENDS_HPP
