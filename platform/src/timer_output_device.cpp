#include "snd/platform/timer_output_device.hpp"

#include <cstring>
#include <new>

#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/types.h"

#include "common/device_callback.hpp"

namespace snd::platform {


TimerOutputDevice::TimerOutputDevice(IAllocator& allocator, const TimerDeviceConfig& config) noexcept
    : allocator_(allocator), config_(config) {}

TimerOutputDevice::~TimerOutputDevice() { Close(); }

bool TimerOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
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
  open_ = true;
  return true;
}

bool TimerOutputDevice::Start() {
  if (!open_) return false;
  if (started_) return true;
  running_.store(true, std::memory_order_release);
  ThreadDesc td;
  td.name = "snd-device";
  td.priority = config_.realtimeThread ? ThreadPriority::Realtime : ThreadPriority::Normal;
  td.minStackBytes = kRtThreadMinStackBytes;  // D-47: RT-priority thread like the audio thread
  if (!thread_.Start(td, &TimerOutputDevice::ThreadMain, this)) {
    running_.store(false, std::memory_order_relaxed);
    return false;
  }
  started_ = true;
  return true;
}

void TimerOutputDevice::Stop() {
  if (detail::CalledFromCallback(this)) {  // forbidden from the callback: joining ourselves would deadlock
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!started_) return;
  running_.store(false, std::memory_order_release);
  thread_.Join();  // returns after the last callback finished
  started_ = false;
}

void TimerOutputDevice::Close() {
  if (detail::CalledFromCallback(this)) {  // forbidden from the callback (would free the buffer in use)
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Stop();
  if (buffer_ != nullptr) allocator_.Free(buffer_, MemTag::Misc);
  buffer_ = nullptr;
  callback_ = nullptr;
  open_ = false;
}

std::uint32_t TimerOutputDevice::PeriodFrames() const { return periodFrames_; }
std::uint32_t TimerOutputDevice::SampleRate() const { return sampleRate_; }
std::uint32_t TimerOutputDevice::Channels() const { return channels_; }
double TimerOutputDevice::OutputLatencySeconds() const {
  if (config_.latencySeconds > 0.0) return config_.latencySeconds;
  return sampleRate_ != 0u ? static_cast<double>(periodFrames_) / static_cast<double>(sampleRate_) : 0.0;
}
void TimerOutputDevice::ThreadMain(void* self) noexcept { static_cast<TimerOutputDevice*>(self)->Run(); }

void TimerOutputDevice::Run() noexcept {
  ScopedFlushDenormals ftz;  // 2.4: device thread too
  detail::CallbackMarker mark(this);
  // Deadlines advance by exactly one period per callback, kept as an integer
  // part in ns plus a remainder in units of 1/sampleRate ns, so there is no
  // drift and no product that grows with the running time:
  //   periodNs = periodFrames * 1e9 / sampleRate = whole + frac / sampleRate
  // periodFrames * 1e9 < 2^32 * 1e9 < 2^62 fits in 64 bits for any input,
  // frac < sampleRate < 2^32, and `due` is an absolute monotonic time in ns
  // (2^64 ns = 584 years), so nothing overflows however long the device runs.
  const std::uint64_t rate = sampleRate_;
  const std::uint64_t num = static_cast<std::uint64_t>(periodFrames_) * 1000000000ull;
  const std::uint64_t whole = num / rate;
  const std::uint64_t frac = num % rate;
  std::uint64_t due = MonotonicNanoseconds();
  std::uint64_t carry = 0;  // < rate
  std::uint64_t written = 0;
  while (running_.load(std::memory_order_acquire)) {
    detail::DeliverFrames(callback_, callbackUser_, buffer_, periodFrames_, channels_, periodFrames_, statCallbacks_,
                          statFrames_);
    written += periodFrames_;
    PublishClock(written, periodFrames_, sampleRate_);
    due += whole;
    carry += frac;  // < 2 * rate <= 2^33
    if (carry >= rate) {
      carry -= rate;
      ++due;
    }
    const std::uint64_t now = MonotonicNanoseconds();
    if (now > due && now - due > whole + 1u) {
      // More than a period late (preempted / debugger): resynchronise instead
      // of firing a burst of catch-up callbacks; the next period starts now.
      late_.fetch_add(1, std::memory_order_relaxed);
      due = now;
      carry = 0;
      continue;
    }
    if (due > now) SleepUntil(due);
  }
}

}  // namespace snd::platform
