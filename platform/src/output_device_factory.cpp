#include <new>

#include "backends.hpp"
#include "common/owned_string.hpp"
#include "snd/platform/output_device.hpp"

namespace snd::platform {

bool OutputBackendAvailable(OutputBackend backend) noexcept {
  switch (backend) {
    case OutputBackend::Default:
    case OutputBackend::SilentTimer:
    case OutputBackend::Manual:
      return true;
    case OutputBackend::Alsa:
#if defined(SND_PLATFORM_HAVE_ALSA)
      return true;
#else
      return false;
#endif
    case OutputBackend::Wasapi:
#if defined(SND_PLATFORM_HAVE_WASAPI)
      return true;
#else
      return false;
#endif
    case OutputBackend::CoreAudio:
#if defined(SND_PLATFORM_HAVE_COREAUDIO)
      return true;
#else
      return false;
#endif
  }
  return false;
}

OutputBackend ResolveOutputBackend(OutputBackend backend) noexcept {
  if (backend != OutputBackend::Default) return backend;
#if defined(SND_PLATFORM_HAVE_WASAPI)
  return OutputBackend::Wasapi;
#elif defined(SND_PLATFORM_HAVE_COREAUDIO)
  return OutputBackend::CoreAudio;
#elif defined(SND_PLATFORM_HAVE_ALSA)
  return OutputBackend::Alsa;
#else
  return OutputBackend::SilentTimer;
#endif
}

PlatformOutputDevice* CreateOutputDevice(OutputBackend backend, IAllocator& allocator,
                                         const OutputDeviceOptions* options) noexcept {
  const OutputDeviceOptions defaults{};
  const OutputDeviceOptions& o = options != nullptr ? *options : defaults;
  switch (ResolveOutputBackend(backend)) {
    case OutputBackend::Default:
    case OutputBackend::SilentTimer: {
      TimerDeviceConfig tc;
      tc.realtimeThread = o.realtimeThread;
      return detail::NewDevice<TimerOutputDevice>(allocator, allocator, tc);
    }
    case OutputBackend::Manual:
      return detail::NewDevice<ManualOutputDevice>(allocator, allocator, ManualDeviceConfig{});
    case OutputBackend::Alsa:
#if defined(SND_PLATFORM_HAVE_ALSA)
      return detail::CreateAlsaOutputDevice(allocator, o);
#else
      return nullptr;
#endif
    case OutputBackend::Wasapi:
#if defined(SND_PLATFORM_HAVE_WASAPI)
      return detail::CreateWasapiOutputDevice(allocator, o);
#else
      return nullptr;
#endif
    case OutputBackend::CoreAudio:
#if defined(SND_PLATFORM_HAVE_COREAUDIO)
      return detail::CreateCoreAudioOutputDevice(allocator, o);
#else
      return nullptr;
#endif
  }
  return nullptr;
}

void DestroyOutputDevice(IOutputDevice* device, IAllocator& allocator) noexcept {
  if (device == nullptr) return;
  // Every backend derives from IOutputDevice only (single inheritance), so the
  // object starts at `device`. The virtual destructor runs the backend's.
  void* memory = static_cast<void*>(device);
  device->~IOutputDevice();
  allocator.Free(memory, MemTag::Misc);
}

std::uint32_t EnumerateOutputDevices(OutputBackend backend, OutputDeviceInfo* out, std::uint32_t capacity) noexcept {
  if (out == nullptr) capacity = 0;
  switch (ResolveOutputBackend(backend)) {
    case OutputBackend::Default:
    case OutputBackend::SilentTimer:
      if (capacity > 0u) {
        out[0] = OutputDeviceInfo{};
        detail::CopyTruncatedUtf8(out[0].id, sizeof(out[0].id), "silent");
        detail::CopyTruncatedUtf8(out[0].name, sizeof(out[0].name), "Silent timer device");
        out[0].isDefault = true;
      }
      return 1u;
    case OutputBackend::Manual:
      if (capacity > 0u) {
        out[0] = OutputDeviceInfo{};
        detail::CopyTruncatedUtf8(out[0].id, sizeof(out[0].id), "manual");
        detail::CopyTruncatedUtf8(out[0].name, sizeof(out[0].name), "Manual silent device");
        out[0].isDefault = true;
      }
      return 1u;
    case OutputBackend::Alsa:
#if defined(SND_PLATFORM_HAVE_ALSA)
      return detail::EnumerateAlsaDevices(out, capacity);
#else
      return 0u;
#endif
    case OutputBackend::Wasapi:
#if defined(SND_PLATFORM_HAVE_WASAPI)
      return detail::EnumerateWasapiDevices(out, capacity);
#else
      return 0u;
#endif
    case OutputBackend::CoreAudio:
#if defined(SND_PLATFORM_HAVE_COREAUDIO)
      return detail::EnumerateCoreAudioDevices(out, capacity);
#else
      return 0u;
#endif
  }
  return 0u;
}

}  // namespace snd::platform
