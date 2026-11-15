#include "snd/platform/output_device.hpp"

#include <cstring>
#include <new>

#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/types.h"

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
  const std::size_t bytes = sizeof(float) * static_cast<std::size_t>(period) * ch;
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
  if (!thread_.Start(td, &TimerOutputDevice::ThreadMain, this)) {
    running_.store(false, std::memory_order_relaxed);
    return false;
  }
  started_ = true;
  return true;
}

void TimerOutputDevice::Stop() {
  if (!started_) return;
  running_.store(false, std::memory_order_release);
  thread_.Join();  // returns after the last callback finished
  started_ = false;
}

void TimerOutputDevice::Close() {
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
  const std::uint64_t periodNs =
      static_cast<std::uint64_t>(periodFrames_) * 1000000000ull / static_cast<std::uint64_t>(sampleRate_);
  // Exact deadlines: period k is due at start + k * period (frames-based, no drift).
  const std::uint64_t start = MonotonicNanoseconds();
  std::uint64_t k = 0;
  while (running_.load(std::memory_order_acquire)) {
    callback_(callbackUser_, buffer_, periodFrames_, channels_);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(periodFrames_, std::memory_order_relaxed);
    ++k;
    std::uint64_t due = start + (k * static_cast<std::uint64_t>(periodFrames_) * 1000000000ull) /
                                    static_cast<std::uint64_t>(sampleRate_);
    const std::uint64_t now = MonotonicNanoseconds();
    if (now > due + periodNs) {
      // More than a period late (preempted / debugger): resynchronise instead
      // of firing a burst of catch-up callbacks.
      late_.fetch_add(1, std::memory_order_relaxed);
      k = (now - start) * static_cast<std::uint64_t>(sampleRate_) /
          (static_cast<std::uint64_t>(periodFrames_) * 1000000000ull);
      continue;
    }
    if (due > now) SleepUntil(due);
  }
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
