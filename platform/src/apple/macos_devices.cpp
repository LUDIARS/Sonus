#include "apple/macos_devices.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstring>

#include "backends.hpp"
#include "common/owned_string.hpp"

namespace snd::platform::detail::coreaudio {

namespace {
AudioObjectPropertyAddress Address(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) noexcept {
  AudioObjectPropertyAddress a;
  a.mSelector = sel;
  a.mScope = scope;
  a.mElement = kElementMain;
  return a;
}

template <class T>
bool GetValue(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T* out) noexcept {
  const AudioObjectPropertyAddress a = Address(sel, scope);
  UInt32 size = sizeof(T);
  return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out) == noErr && size == sizeof(T);
}

bool CopyCfString(AudioObjectID obj, AudioObjectPropertySelector sel, char* out, std::uint32_t bytes) noexcept {
  if (bytes == 0u) return false;
  out[0] = '\0';
  CFStringRef s = nullptr;
  if (!GetValue(obj, sel, kAudioObjectPropertyScopeGlobal, &s) || s == nullptr) return false;
  char tmp[1024];
  const bool ok = CFStringGetCString(s, tmp, sizeof(tmp), kCFStringEncodingUTF8) != 0;
  CFRelease(s);
  if (ok) CopyTruncatedUtf8(out, bytes, tmp);
  return ok;
}

std::uint32_t OutputChannels(AudioObjectID dev) noexcept {
  const AudioObjectPropertyAddress a = Address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0u) return 0;
  alignas(16) unsigned char storage[4096];
  if (size > sizeof(storage)) return 0;
  auto* list = reinterpret_cast<AudioBufferList*>(storage);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
  std::uint32_t channels = 0;
  for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
  return channels;
}
}  // namespace

AudioObjectID MacDefaultOutputDevice() noexcept {
  AudioObjectID id = 0;
  if (!GetValue(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                &id)) {
    return 0;
  }
  return id;
}

std::uint32_t MacOutputDevices(AudioObjectID* out, std::uint32_t capacity) noexcept {
  const AudioObjectPropertyAddress a = Address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return 0;
  AudioObjectID all[256];
  if (size > sizeof(all)) size = sizeof(all);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, all) != noErr) return 0;
  const std::uint32_t n = size / sizeof(AudioObjectID);
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (OutputChannels(all[i]) == 0u) continue;  // input-only device
    if (count < capacity && out != nullptr) out[count] = all[i];
    ++count;
  }
  return count;
}

bool MacFindDeviceByUid(const char* uid, AudioObjectID* out) noexcept {
  AudioObjectID list[256];
  const std::uint32_t n = MacOutputDevices(list, 256);
  char buf[256];
  for (std::uint32_t i = 0; i < n && i < 256u; ++i) {
    if (MacDeviceUid(list[i], buf, sizeof(buf)) && std::strcmp(buf, uid) == 0) {
      *out = list[i];
      return true;
    }
  }
  return false;
}

bool MacQueryDevice(AudioObjectID dev, MacDeviceInfo* out) noexcept {
  MacDeviceInfo info;
  Float64 rate = 0.0;
  if (!GetValue(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate) || rate <= 0.0) {
    return false;
  }
  info.nominalRate = static_cast<std::uint32_t>(rate + 0.5);
  info.outputChannels = OutputChannels(dev);
  UInt32 v = 0;
  if (GetValue(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &v)) info.bufferFrames = v;
  std::uint32_t latency = info.bufferFrames;
  if (GetValue(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &v)) latency += v;
  if (GetValue(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &v)) latency += v;
  {
    const AudioObjectPropertyAddress a = Address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    AudioStreamID streams[16];
    UInt32 size = sizeof(streams);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, streams) == noErr && size >= sizeof(AudioStreamID)) {
      if (GetValue(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &v)) latency += v;
    }
  }
  info.latencyFrames = latency;
  UInt32 alive = 0;
  info.alive = GetValue(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, &alive) && alive != 0u;
  *out = info;
  return true;
}

bool MacDeviceUid(AudioObjectID dev, char* out, std::uint32_t bytes) noexcept {
  return CopyCfString(dev, kAudioDevicePropertyDeviceUID, out, bytes);
}

bool MacDeviceName(AudioObjectID dev, char* out, std::uint32_t bytes) noexcept {
  return CopyCfString(dev, kAudioObjectPropertyName, out, bytes);
}

}  // namespace snd::platform::detail::coreaudio

namespace snd::platform::detail {

std::uint32_t EnumerateCoreAudioDevices(OutputDeviceInfo* out, std::uint32_t capacity) noexcept {
  AudioObjectID list[256];
  const std::uint32_t n = coreaudio::MacOutputDevices(list, 256);
  const AudioObjectID def = coreaudio::MacDefaultOutputDevice();
  for (std::uint32_t i = 0; i < n && i < capacity && i < 256u; ++i) {
    OutputDeviceInfo& info = out[i];
    info = OutputDeviceInfo{};
    coreaudio::MacDeviceUid(list[i], info.id, sizeof(info.id));
    if (!coreaudio::MacDeviceName(list[i], info.name, sizeof(info.name))) {
      CopyTruncatedUtf8(info.name, sizeof(info.name), info.id);
    }
    info.isDefault = list[i] == def;
  }
  return n;
}

}  // namespace snd::platform::detail
