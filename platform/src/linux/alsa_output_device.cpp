#include "linux/alsa_output_device.hpp"

#include <cerrno>
#include <cstring>
#include <new>

#include "backends.hpp"
#include "common/device_callback.hpp"
#include "common/stream_policy.hpp"
#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/platform/device_enum.hpp"
#include "snd/types.h"

namespace snd::platform::detail {

namespace {
constexpr int kWaitTimeoutMs = 100;  // bounds how long Stop() waits for the device thread
}  // namespace

AlsaOutputDevice::AlsaOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept
    : allocator_(allocator),
      realtimeThread_(options.realtimeThread),
      reopenOnLoss_(options.reopenOnLoss),
      reopenIntervalMs_(options.reopenIntervalMs != 0u ? options.reopenIntervalMs : 500u) {
  request_.periods = options.bufferPeriods != 0u ? options.bufferPeriods : 3u;
}

AlsaOutputDevice::~AlsaOutputDevice() { Close(); }

bool AlsaOutputDevice::Init(const OutputDeviceOptions& options) noexcept {
  return optionName_.Assign(allocator_, options.deviceName) && config_.Assign(allocator_, options.alsaConfig);
}

bool AlsaOutputDevice::EnsureBuffers() noexcept {
  std::size_t samples = 0;
  if (!base::CheckedMul(stream_.periodFrames, stream_.channels, &samples)) return false;
  if (samples <= bufSamples_ && floatBuf_ != nullptr) return true;
  if (floatBuf_ != nullptr) allocator_.Free(floatBuf_, MemTag::Misc);
  if (convBuf_ != nullptr) allocator_.Free(convBuf_, MemTag::Misc);
  floatBuf_ = static_cast<float*>(allocator_.Allocate(samples * sizeof(float), 64, MemTag::Misc));
  convBuf_ = allocator_.Allocate(samples * sizeof(float), 64, MemTag::Misc);  // >= any integer format
  if (floatBuf_ == nullptr || convBuf_ == nullptr) {
    if (floatBuf_ != nullptr) allocator_.Free(floatBuf_, MemTag::Misc);
    if (convBuf_ != nullptr) allocator_.Free(convBuf_, MemTag::Misc);
    floatBuf_ = nullptr;
    convBuf_ = nullptr;
    bufSamples_ = 0;
    return false;
  }
  std::memset(floatBuf_, 0, samples * sizeof(float));
  bufSamples_ = samples;
  return true;
}

bool AlsaOutputDevice::OpenAndConfigure() noexcept {
  snd_pcm_t* pcm = nullptr;
  if (OpenAlsaPcm(resolvedName_.Get(), config_.Get(), &pcm) < 0) return false;
  AlsaStream s;
  if (ConfigureAlsaPcm(pcm, request_, &s) < 0) {
    snd_pcm_close(pcm);
    return false;
  }
  pcm_ = pcm;
  stream_ = s;
  if (!EnsureBuffers()) {
    ClosePcm();
    return false;
  }
  rate_.store(s.sampleRate, std::memory_order_relaxed);
  channels_.store(s.channels, std::memory_order_relaxed);
  period_.store(s.periodFrames, std::memory_order_relaxed);
  buffer_.store(s.bufferFrames, std::memory_order_relaxed);
  return true;
}

void AlsaOutputDevice::ClosePcm() noexcept {
  if (pcm_ != nullptr) snd_pcm_close(pcm_);
  pcm_ = nullptr;
}

bool AlsaOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
  if (open_ || cb == nullptr || desc.channels > SND_MAX_CHANNELS) return false;
  // A private configuration has no enumeration order: device_index must be 0
  // (select its PCMs with deviceName).
  if (!config_.Empty() && desc.device_index != 0u) return false;
  // Device name (D-64): option > device_index (enumeration order) > "default".
  const char* name = optionName_.Get();
  OutputDeviceInfo info;
  if (optionName_.Empty()) {
    if (desc.device_index == 0u) {
      name = "default";
    } else {
      // Enumeration order is the ALSA hint order (D-64); the first 64 entries are selectable.
      OutputDeviceInfo list[64];
      const std::uint32_t n = EnumerateAlsaDevices(list, 64u);
      const std::int32_t at = ResolveDeviceIndex(desc.device_index, n < 64u ? n : 64u);
      if (at < 0) return false;
      info = list[at];
      name = info.id;
    }
  }
  if (!resolvedName_.Assign(allocator_, name)) return false;
  request_.sampleRate = desc.sample_rate;
  request_.channels = desc.channels;
  request_.periodFrames = desc.period_frames;
  fatal_ = false;
  if (!OpenAndConfigure()) return false;
  callback_ = cb;
  callbackUser_ = user;
  written_ = 0;
  ResetClock();
  open_ = true;
  return true;
}

bool AlsaOutputDevice::Start() {
  if (!open_) return false;
  if (started_) return true;
  running_.store(true, std::memory_order_release);
  ThreadDesc td;
  td.name = "snd-alsa";
  td.priority = realtimeThread_ ? ThreadPriority::Realtime : ThreadPriority::Normal;
  td.minStackBytes = kRtThreadMinStackBytes;  // D-47
  if (!thread_.Start(td, &AlsaOutputDevice::ThreadMain, this)) {
    running_.store(false, std::memory_order_relaxed);
    return false;
  }
  started_ = true;
  return true;
}

void AlsaOutputDevice::Stop() {
  if (CalledFromCallback(this)) {  // from the callback / device thread: joining ourselves would deadlock
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!started_) return;
  running_.store(false, std::memory_order_release);
  thread_.Join();  // returns after the last callback finished
  if (pcm_ != nullptr) snd_pcm_drop(pcm_);
  started_ = false;
}

void AlsaOutputDevice::Close() {
  if (CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Stop();
  ClosePcm();
  if (floatBuf_ != nullptr) allocator_.Free(floatBuf_, MemTag::Misc);
  if (convBuf_ != nullptr) allocator_.Free(convBuf_, MemTag::Misc);
  floatBuf_ = nullptr;
  convBuf_ = nullptr;
  bufSamples_ = 0;
  callback_ = nullptr;
  open_ = false;
}

double AlsaOutputDevice::OutputLatencySeconds() const {
  // Everything queued in the hardware buffer is ahead of the listener.
  return FramesToSeconds(buffer_.load(std::memory_order_relaxed), rate_.load(std::memory_order_relaxed));
}

void AlsaOutputDevice::ThreadMain(void* self) noexcept { static_cast<AlsaOutputDevice*>(self)->Run(); }

bool AlsaOutputDevice::WriteFrames(const void* data, std::uint32_t frames) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  const std::size_t frameBytes = static_cast<std::size_t>(BytesPerSample(stream_.format)) * stream_.channels;
  std::uint32_t left = frames;
  // Always try once (non-blocking): frames the callback produced are written
  // even when Stop() is pending; only waiting for room stops at Stop().
  while (left > 0u) {
    const snd_pcm_sframes_t w = snd_pcm_writei(pcm_, p, left);
    if (w >= 0) {
      left -= static_cast<std::uint32_t>(w);
      p += static_cast<std::size_t>(w) * frameBytes;
      continue;
    }
    if (ClassifyAlsaError(static_cast<int>(w)) == StreamErrorAction::Retry) {
      if (!running_.load(std::memory_order_acquire)) break;
      const int r = snd_pcm_wait(pcm_, kWaitTimeoutMs);
      if (r < 0) {
        HandleError(r);
        return false;
      }
      continue;
    }
    HandleError(static_cast<int>(w));
    return false;
  }
  written_ += frames - left;
  return true;
}

bool AlsaOutputDevice::PrefillSilence() noexcept {
  // At most one buffer of silence (the null plugin always reports a full
  // buffer free), then start explicitly if the threshold was not reached.
  std::memset(convBuf_, 0, static_cast<std::size_t>(stream_.periodFrames) * stream_.channels *
                               BytesPerSample(stream_.format));
  const std::uint32_t periods = (stream_.bufferFrames + stream_.periodFrames - 1u) / stream_.periodFrames;
  for (std::uint32_t i = 0; i < periods && pcm_ != nullptr; ++i) {
    const snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
    if (avail < 0) {
      HandleError(static_cast<int>(avail));
      return false;
    }
    if (static_cast<std::uint64_t>(avail) < stream_.periodFrames) break;
    if (!WriteFrames(convBuf_, stream_.periodFrames)) return false;
  }
  if (pcm_ != nullptr && snd_pcm_state(pcm_) == SND_PCM_STATE_PREPARED) {
    const int r = snd_pcm_start(pcm_);
    if (r < 0) {
      HandleError(r);
      return false;
    }
  }
  return pcm_ != nullptr;
}

void AlsaOutputDevice::UpdateClock() noexcept {
  snd_pcm_sframes_t delay = 0;
  if (snd_pcm_delay(pcm_, &delay) < 0 || delay < 0) delay = 0;
  PublishClock(written_, static_cast<std::uint64_t>(delay), stream_.sampleRate);
}

void AlsaOutputDevice::HandleError(int err) noexcept {
  switch (ClassifyAlsaError(err)) {
    case StreamErrorAction::None:
    case StreamErrorAction::Retry:
      return;
    case StreamErrorAction::RecoverXrun:
      statXruns_.fetch_add(1, std::memory_order_relaxed);
      if (snd_pcm_prepare(pcm_) < 0) break;  // treat as lost
      PrefillSilence();
      return;
    case StreamErrorAction::Suspended: {
      PostEvent(DeviceEventKind::Suspended, 0, 0, 0);
      int r = snd_pcm_resume(pcm_);
      while (r == -EAGAIN && running_.load(std::memory_order_acquire)) {
        SleepFor(50000000ull);  // the driver resumes asynchronously
        r = snd_pcm_resume(pcm_);
      }
      if (r < 0) r = snd_pcm_prepare(pcm_);  // hardware without resume support
      if (r < 0) break;
      PostEvent(DeviceEventKind::Resumed, stream_.channels, stream_.sampleRate, stream_.periodFrames);
      PrefillSilence();
      return;
    }
    case StreamErrorAction::Fatal:
      fatal_ = true;
      break;
    case StreamErrorAction::Lost:
      break;
  }
  // Device gone (or recovery failed): report and close; Run() reopens.
  ClosePcm();
  PostEvent(DeviceEventKind::Lost, 0, 0, 0);
}

void AlsaOutputDevice::WaitForReopen() noexcept {
  // Sleep in slices so Stop() is not delayed by the interval.
  const std::uint64_t deadline = MonotonicNanoseconds() + static_cast<std::uint64_t>(reopenIntervalMs_) * 1000000ull;
  while (running_.load(std::memory_order_acquire) && MonotonicNanoseconds() < deadline) SleepFor(10000000ull);
  if (!running_.load(std::memory_order_acquire) || !reopenOnLoss_ || fatal_) return;
  if (!OpenAndConfigure()) return;
  statReopens_.fetch_add(1, std::memory_order_relaxed);
  // CHANGED before the first callback in the (possibly) new format (D-61).
  PostEvent(DeviceEventKind::Changed, stream_.channels, stream_.sampleRate, stream_.periodFrames);
  PrefillSilence();
}

void AlsaOutputDevice::Run() noexcept {
  ScopedFlushDenormals ftz;  // 2.4 (DeliverFrames sets it per callback too)
  CallbackMarker mark(this);
  if (pcm_ != nullptr) {
    const int r = snd_pcm_prepare(pcm_);
    if (r < 0) HandleError(r);
  }
  if (pcm_ != nullptr) PrefillSilence();
  while (running_.load(std::memory_order_acquire)) {
    if (pcm_ == nullptr) {
      WaitForReopen();
      continue;
    }
    const snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_);
    if (avail < 0) {
      HandleError(static_cast<int>(avail));
      continue;
    }
    const std::uint32_t period = stream_.periodFrames;
    if (static_cast<std::uint64_t>(avail) < period) {
      const int r = snd_pcm_wait(pcm_, kWaitTimeoutMs);
      if (r < 0) HandleError(r);
      continue;
    }
    DeliverFrames(callback_, callbackUser_, floatBuf_, period, stream_.channels, period, statCallbacks_, statFrames_);
    const void* out = floatBuf_;
    if (stream_.format != SampleFormat::Float32) {
      ConvertFromFloat(floatBuf_, convBuf_, static_cast<std::size_t>(period) * stream_.channels, stream_.format);
      out = convBuf_;
    }
    if (WriteFrames(out, period) && pcm_ != nullptr) UpdateClock();
  }
}

PlatformOutputDevice* CreateAlsaOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept {
  AlsaOutputDevice* d = NewDevice<AlsaOutputDevice>(allocator, allocator, options);
  if (d == nullptr) return nullptr;
  if (!d->Init(options)) {
    d->~AlsaOutputDevice();
    allocator.Free(d, MemTag::Misc);
    return nullptr;
  }
  return d;
}

}  // namespace snd::platform::detail
