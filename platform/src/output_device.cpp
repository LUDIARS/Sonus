#include "snd/platform/output_device.hpp"

#include <cstring>
#include <new>

#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/types.h"

namespace snd::platform {

namespace {
// The device whose callback the calling thread is running (Stop/Close guard).
thread_local const TimerOutputDevice* tInCallback = nullptr;
}  // namespace


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
  if (tInCallback == this) {  // forbidden from the callback: joining ourselves would deadlock
    misuseFromCallback_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!started_) return;
  running_.store(false, std::memory_order_release);
  thread_.Join();  // returns after the last callback finished
  started_ = false;
}

void TimerOutputDevice::Close() {
  if (tInCallback == this) {  // forbidden from the callback (would free the buffer in use)
    misuseFromCallback_.fetch_add(1, std::memory_order_relaxed);
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
void TimerOutputDevice::SetEventListener(DeviceEventFn fn, void* user) {
  // The silent timer device never changes route; the listener is kept for
  // API symmetry (stage-2 backends report CHANGED / SUSPENDED / ... here).
  listener_ = fn;
  listenerUser_ = user;
}

void TimerOutputDevice::ThreadMain(void* self) noexcept { static_cast<TimerOutputDevice*>(self)->Run(); }

void TimerOutputDevice::Run() noexcept {
  ScopedFlushDenormals ftz;  // 2.4: device thread too
  tInCallback = this;
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
  while (running_.load(std::memory_order_acquire)) {
    callback_(callbackUser_, buffer_, periodFrames_, channels_);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(periodFrames_, std::memory_order_relaxed);
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
  tInCallback = nullptr;
}

IOutputDevice* CreateOutputDevice(OutputBackend backend, IAllocator& allocator) noexcept {
  switch (backend) {
    case OutputBackend::Default:
    case OutputBackend::SilentTimer: {
      void* p = allocator.Allocate(sizeof(TimerOutputDevice), alignof(TimerOutputDevice) > 16 ? alignof(TimerOutputDevice) : 16,
                                   MemTag::Misc);
      if (p == nullptr) return nullptr;
      return ::new (p) TimerOutputDevice(allocator);
    }
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

}  // namespace snd::platform
