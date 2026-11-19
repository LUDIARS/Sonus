#include "snd/platform/manual_output_device.hpp"

#include <cstring>
#include <new>

#include "snd/base/atomic.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/types.h"

#include "common/device_callback.hpp"

namespace snd::platform {

ManualOutputDevice::ManualOutputDevice(IAllocator& allocator, const ManualDeviceConfig& config) noexcept
    : allocator_(allocator), config_(config) {}

ManualOutputDevice::~ManualOutputDevice() { Close(); }

bool ManualOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
  if (open_ || cb == nullptr) return false;
  const std::uint32_t rate = desc.sample_rate != 0u ? desc.sample_rate : config_.sampleRate;
  const std::uint32_t ch = desc.channels != 0u ? desc.channels : config_.channels;
  const std::uint32_t period = desc.period_frames != 0u ? desc.period_frames : config_.periodFrames;
  if (rate == 0u || ch == 0u || ch > SND_MAX_CHANNELS || period == 0u) return false;
  std::size_t samples = 0, bytes = 0;
  if (!base::CheckedMul(period, ch, &samples) || !base::CheckedMul(samples, sizeof(float), &bytes)) return false;
  buffer_ = static_cast<float*>(allocator_.Allocate(bytes, 64, MemTag::Misc));
  if (buffer_ == nullptr) return false;
  std::memset(buffer_, 0, bytes);
  sampleRate_ = rate;
  channels_ = ch;
  periodFrames_ = period;
  callback_ = cb;
  callbackUser_ = user;
  written_ = 0;
  ResetClock();
  open_ = true;
  return true;
}

bool ManualOutputDevice::Start() {
  if (!open_) return false;
  started_ = true;
  return true;
}

void ManualOutputDevice::Stop() {
  if (detail::CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  started_ = false;  // Advance is not concurrent with Stop: no callback is running
}

void ManualOutputDevice::Close() {
  if (detail::CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Stop();
  if (buffer_ != nullptr) allocator_.Free(buffer_, MemTag::Misc);
  buffer_ = nullptr;
  callback_ = nullptr;
  open_ = false;
}

std::uint32_t ManualOutputDevice::PeriodFrames() const { return periodFrames_; }
std::uint32_t ManualOutputDevice::SampleRate() const { return sampleRate_; }
std::uint32_t ManualOutputDevice::Channels() const { return channels_; }
double ManualOutputDevice::OutputLatencySeconds() const {
  if (config_.latencySeconds > 0.0) return config_.latencySeconds;
  return sampleRate_ != 0u ? static_cast<double>(periodFrames_) / static_cast<double>(sampleRate_) : 0.0;
}

void ManualOutputDevice::SetBeforePeriod(BeforePeriodFn fn, void* user) noexcept {
  before_ = fn;
  beforeUser_ = user;
}

std::uint32_t ManualOutputDevice::Advance(std::uint32_t periods, float* out) noexcept {
  if (detail::CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return 0u;
  }
  if (!open_ || !started_) return 0u;
  const std::size_t periodSamples = std::size_t{periodFrames_} * channels_;
  for (std::uint32_t p = 0; p < periods; ++p) {
    if (before_ != nullptr) before_(beforeUser_);
    {
      detail::CallbackMarker mark(this);
      detail::DeliverFrames(callback_, callbackUser_, buffer_, periodFrames_, channels_, periodFrames_, statCallbacks_,
                            statFrames_);
    }
    written_ += periodFrames_;
    PublishClock(written_, 0u, sampleRate_);
    if (out != nullptr) std::memcpy(out + p * periodSamples, buffer_, sizeof(float) * periodSamples);
  }
  return periods;
}

}  // namespace snd::platform
