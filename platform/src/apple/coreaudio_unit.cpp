#include "apple/coreaudio_unit.hpp"

#include <os/object.h>
#include <os/workgroup.h>

namespace snd::platform::detail::coreaudio {

OSStatus OutputUnit::Create(UnitKind kind) noexcept {
  Dispose();
  AudioComponentDescription d{};
  d.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
  (void)kind;
  d.componentSubType = kAudioUnitSubType_RemoteIO;
#else
  d.componentSubType = kind == UnitKind::HalOutput ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput;
#endif
  d.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &d);
  if (component == nullptr) return kAudioUnitErr_InvalidElement;
  return AudioComponentInstanceNew(component, &unit_);
}

void OutputUnit::Dispose() noexcept {
  if (unit_ == nullptr) return;
  Stop();
  Uninitialize();
  AudioComponentInstanceDispose(unit_);
  unit_ = nullptr;
}

#if !TARGET_OS_IPHONE
OSStatus OutputUnit::SetDevice(AudioObjectID device) noexcept {
  AudioDeviceID id = device;
  return AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id,
                              sizeof(id));
}

AudioObjectID OutputUnit::CurrentDevice() const noexcept {
  AudioDeviceID id = 0;
  UInt32 size = sizeof(id);
  if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, &size) !=
      noErr) {
    return 0;
  }
  return id;
}

OSStatus OutputUnit::SetDeviceBufferFrames(std::uint32_t frames) noexcept {
  UInt32 v = frames;
  return AudioUnitSetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &v, sizeof(v));
}
#endif

OSStatus OutputUnit::SetClientFormat(std::uint32_t sampleRate, std::uint32_t channels) noexcept {
  AudioStreamBasicDescription f{};
  f.mSampleRate = static_cast<Float64>(sampleRate);
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;  // float | packed | native endian, interleaved
  f.mBytesPerPacket = 4u * channels;
  f.mFramesPerPacket = 1;
  f.mBytesPerFrame = 4u * channels;
  f.mChannelsPerFrame = channels;
  f.mBitsPerChannel = 32;
  // Input scope of the output element: the format we hand to the unit.
  return AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f));
}

OSStatus OutputUnit::SetRenderCallback(AURenderCallback fn, void* user) noexcept {
  AURenderCallbackStruct cb{};
  cb.inputProc = fn;
  cb.inputProcRefCon = user;
  return AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
}

OSStatus OutputUnit::SetMaxFramesPerSlice(std::uint32_t frames) noexcept {
  UInt32 v = frames;
  return AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &v,
                              sizeof(v));
}

std::uint32_t OutputUnit::MaxFramesPerSlice() const noexcept {
  UInt32 v = 0;
  UInt32 size = sizeof(v);
  if (AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &v, &size) !=
      noErr) {
    return 0;
  }
  return v;
}

double OutputUnit::UnitLatencySeconds() const noexcept {
  Float64 v = 0.0;
  UInt32 size = sizeof(v);
  if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &v, &size) != noErr) return 0.0;
  return v;
}

OSStatus OutputUnit::Initialize() noexcept {
  if (initialized_) return noErr;
  const OSStatus s = AudioUnitInitialize(unit_);
  initialized_ = s == noErr;
  return s;
}

void OutputUnit::Uninitialize() noexcept {
  if (!initialized_) return;
  AudioUnitUninitialize(unit_);
  initialized_ = false;
}

OSStatus OutputUnit::Start() noexcept {
  if (started_) return noErr;
  const OSStatus s = AudioOutputUnitStart(unit_);
  started_ = s == noErr;
  return s;
}

void OutputUnit::Stop() noexcept {
  if (!started_) return;
  AudioOutputUnitStop(unit_);
  started_ = false;
}

void* OutputUnit::CopyWorkgroup() const noexcept {
  if (unit_ == nullptr) return nullptr;
  if (__builtin_available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
    os_workgroup_t wg = nullptr;
    UInt32 size = sizeof(wg);
    if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_OSWorkgroup, kAudioUnitScope_Global, 0, &wg, &size) !=
            noErr ||
        wg == nullptr) {
      return nullptr;
    }
    return wg;  // retained by the property getter
  }
  return nullptr;
}

}  // namespace snd::platform::detail::coreaudio
