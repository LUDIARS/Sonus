// Silent timer-driven output device (design.md 2.2, CONTRACTS.md D-25).
//
// TimerOutputDevice calls the DeviceCallback once per period in real time
// (absolute deadlines on the monotonic clock, no drift) and discards the
// samples. It is the device for headless runs (servers, CI) and the fallback
// a caller may choose explicitly when no OS device opens (it is never chosen
// silently, D-61).
//
// Threads (IOutputDevice contract): Open/Start/Stop/Close/SetEventListener on
// the owner thread; queries any thread after Open. The callback runs on the
// device's own thread (priority: ThreadPriority::Realtime with fallback),
// with FTZ/DAZ and inside SND_RT_SCOPE like every backend.
// Stop() returns after the last callback has finished.
//
// Calling Stop() or Close() from inside the device callback is FORBIDDEN (it
// would join the calling thread / free the buffer being filled). The device
// detects it, does nothing (no join, no free), returns, and counts it in
// MisuseFromCallback(); the device keeps running.
//
// Timing: deadlines advance by exactly one period per callback in integer
// nanoseconds plus a remainder (no drift, no overflow for any run time). If
// the thread wakes more than one period late it resynchronises (LateWakeups)
// instead of firing catch-up callbacks.
#ifndef SND_PLATFORM_TIMER_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_TIMER_OUTPUT_DEVICE_HPP

#include <atomic>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/platform/platform_output_device.hpp"
#include "snd/platform/thread.hpp"

namespace snd::platform {

struct TimerDeviceConfig {
  std::uint32_t sampleRate = 48000;  // used when the DeviceDesc asks for 0
  std::uint32_t channels = 2;
  std::uint32_t periodFrames = 256;
  double latencySeconds = 0.0;     // reported latency; 0 = one period
  bool realtimeThread = true;      // ask for SCHED_FIFO (falls back silently)
};

class TimerOutputDevice final : public PlatformOutputDevice {
 public:
  // `allocator` provides the period buffer (MemTag::Misc) at Open.
  explicit TimerOutputDevice(IAllocator& allocator, const TimerDeviceConfig& config = TimerDeviceConfig{}) noexcept;
  ~TimerOutputDevice() override;
  TimerOutputDevice(const TimerOutputDevice&) = delete;
  TimerOutputDevice& operator=(const TimerOutputDevice&) = delete;

  // IOutputDevice. Open fails if already open, cb is null, or channels > SND_MAX_CHANNELS.
  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;  // false if not open or the thread could not be created
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override;
  std::uint32_t SampleRate() const override;
  std::uint32_t Channels() const override;
  double OutputLatencySeconds() const override;
  OutputBackend Backend() const noexcept override { return OutputBackend::SilentTimer; }

  // --- statistics (any thread) ---
  std::uint64_t Callbacks() const noexcept { return statCallbacks_.load(std::memory_order_relaxed); }
  std::uint64_t FramesDelivered() const noexcept { return statFrames_.load(std::memory_order_relaxed); }
  // Periods where the timer woke more than one period late and resynchronised.
  std::uint64_t LateWakeups() const noexcept { return late_.load(std::memory_order_relaxed); }
  // Stop()/Close() calls made from inside the callback and ignored.
  std::uint64_t MisuseFromCallback() const noexcept { return statMisuse_.load(std::memory_order_relaxed); }
  PriorityResult ThreadPriorityResult() const noexcept { return thread_.Priority(); }
  bool IsOpen() const noexcept { return open_; }
  bool IsStarted() const noexcept { return started_; }

 private:
  static void ThreadMain(void* self) noexcept;
  void Run() noexcept;

  IAllocator& allocator_;
  TimerDeviceConfig config_;
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;
  float* buffer_ = nullptr;
  std::uint32_t sampleRate_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t periodFrames_ = 0;
  bool open_ = false;
  bool started_ = false;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> late_{0};
  Thread thread_;
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_TIMER_OUTPUT_DEVICE_HPP
