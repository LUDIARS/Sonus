#include "snd/mocks/null_output_device.hpp"

#include <algorithm>

namespace snd::mock {

NullOutputDevice::NullOutputDevice(const NullDeviceConfig& config)
    : config_(config),
      sampleRate_(config.sampleRate),
      channels_(config.channels),
      periodFrames_(config.periodFrames) {}

NullOutputDevice::~NullOutputDevice() = default;

bool NullOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
  if (config_.failOpen || open_ || cb == nullptr) return false;
  sampleRate_ = config_.sampleRate;
  channels_ = config_.channels;
  periodFrames_ = config_.periodFrames;
  if (config_.honorRequest) {
    if (desc.sample_rate != 0) sampleRate_ = desc.sample_rate;
    if (desc.channels != 0) channels_ = std::min<std::uint32_t>(desc.channels, SND_MAX_CHANNELS);
    if (desc.period_frames != 0) periodFrames_ = desc.period_frames;
  }
  callback_ = cb;
  callbackUser_ = user;
  open_ = true;
  ++openCount_;
  period_.assign(static_cast<std::size_t>(periodFrames_) * channels_, config_.fillValue);
  return true;
}

bool NullOutputDevice::Start() {
  if (!open_) return false;
  started_ = true;
  return true;
}

void NullOutputDevice::Stop() { started_ = false; }

void NullOutputDevice::Close() {
  started_ = false;
  open_ = false;
  callback_ = nullptr;
  callbackUser_ = nullptr;
}

std::uint32_t NullOutputDevice::PeriodFrames() const { return periodFrames_; }
std::uint32_t NullOutputDevice::SampleRate() const { return sampleRate_; }
std::uint32_t NullOutputDevice::Channels() const { return channels_; }
double NullOutputDevice::OutputLatencySeconds() const { return config_.latencySeconds; }

void NullOutputDevice::SetEventListener(DeviceEventFn fn, void* user) {
  listener_ = fn;
  listenerUser_ = user;
}

std::uint32_t NullOutputDevice::Pump(std::uint32_t periods) {
  if (!open_ || !started_ || callback_ == nullptr) return 0;
  const std::size_t samples = static_cast<std::size_t>(periodFrames_) * channels_;
  for (std::uint32_t p = 0; p < periods; ++p) {
    period_.assign(samples, config_.fillValue);
    callback_(callbackUser_, period_.data(), periodFrames_, channels_);
    if (captureEnabled_) capture_.insert(capture_.end(), period_.begin(), period_.end());
    ++periods_;
    frames_ += periodFrames_;
  }
  return periods;
}

void NullOutputDevice::EmitEvent(const DeviceEvent& event) {
  if (event.kind == SND_DEVICE_EVENT_CHANGED) {
    if (event.channels != 0) channels_ = std::min<std::uint32_t>(event.channels, SND_MAX_CHANNELS);
    if (event.sample_rate != 0) sampleRate_ = event.sample_rate;
    if (event.period_frames != 0) periodFrames_ = event.period_frames;
  }
  if (listener_ != nullptr) listener_(listenerUser_, &event);
}

std::uint64_t NullOutputDevice::CapturedFrames() const {
  return channels_ == 0 ? 0 : capture_.size() / channels_;
}

std::vector<float> NullOutputDevice::CapturedChannel(std::uint32_t channel) const {
  std::vector<float> out;
  if (channel >= channels_) return out;
  const std::size_t frames = capture_.size() / channels_;
  out.reserve(frames);
  for (std::size_t i = 0; i < frames; ++i) out.push_back(capture_[i * channels_ + channel]);
  return out;
}

}  // namespace snd::mock
