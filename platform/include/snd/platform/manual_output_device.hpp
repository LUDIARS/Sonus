// Silent output device advanced by its owner (design.md 2.2, CONTRACTS.md D-102).
//
// ManualOutputDevice has no thread and no timer: Advance() runs the
// DeviceCallback for whole periods on the calling thread and optionally copies
// what the callback wrote. It is the built-in device of
// Config.output_kind = SND_OUTPUT_MANUAL (snd_advance_output): tests and
// offline tools run the runtime at their own pace, a period at a time.
//
// Threads (IOutputDevice contract): Open/Start/Stop/Close/SetEventListener on
// the owner thread; queries any thread after Open. Advance() is called by one
// thread at a time (not concurrently with Stop/Close); the callback runs on
// that thread, with FTZ/DAZ and inside SND_RT_SCOPE like every backend.
// Stop() returns at once (no callback runs outside Advance). Calling Stop(),
// Close() or Advance() from inside the callback is refused and counted
// (MisuseFromCallback), like the other backends.
//
// `before` (optional) runs before each period on the advancing thread, outside
// the RT scope: the composition root waits there for the audio thread's
// render-ahead (snd_advance_output).
#ifndef SND_PLATFORM_MANUAL_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_MANUAL_OUTPUT_DEVICE_HPP

#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/platform/platform_output_device.hpp"

namespace snd::platform {

struct ManualDeviceConfig {
  std::uint32_t sampleRate = 48000;  // used when the DeviceDesc asks for 0
  std::uint32_t channels = 2;
  std::uint32_t periodFrames = 256;
  double latencySeconds = 0.0;       // reported latency; 0 = one period
};

class ManualOutputDevice final : public PlatformOutputDevice {
 public:
  using BeforePeriodFn = void (*)(void* user) noexcept;

  // `allocator` provides the period buffer (MemTag::Misc) at Open.
  explicit ManualOutputDevice(IAllocator& allocator, const ManualDeviceConfig& config = ManualDeviceConfig{}) noexcept;
  ~ManualOutputDevice() override;
  ManualOutputDevice(const ManualOutputDevice&) = delete;
  ManualOutputDevice& operator=(const ManualOutputDevice&) = delete;

  // IOutputDevice. Open fails if already open, cb is null, or channels > SND_MAX_CHANNELS.
  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;  // false if not open
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override;
  std::uint32_t SampleRate() const override;
  std::uint32_t Channels() const override;
  double OutputLatencySeconds() const override;
  OutputBackend Backend() const noexcept override { return OutputBackend::Manual; }

  // Owner side. Runs `periods` periods if open and started; returns the periods run.
  // out: nullptr, or room for periods * PeriodFrames() * Channels() samples (interleaved).
  std::uint32_t Advance(std::uint32_t periods, float* out) noexcept;
  // Non-RT, before Start. Called before every period of Advance.
  void SetBeforePeriod(BeforePeriodFn fn, void* user) noexcept;

  std::uint64_t FramesDelivered() const noexcept { return statFrames_.load(std::memory_order_relaxed); }
  std::uint64_t MisuseFromCallback() const noexcept { return statMisuse_.load(std::memory_order_relaxed); }
  bool IsOpen() const noexcept { return open_; }
  bool IsStarted() const noexcept { return started_; }

 private:
  IAllocator& allocator_;
  ManualDeviceConfig config_;
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;
  BeforePeriodFn before_ = nullptr;
  void* beforeUser_ = nullptr;
  float* buffer_ = nullptr;
  std::uint32_t sampleRate_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t periodFrames_ = 0;
  std::uint64_t written_ = 0;
  bool open_ = false;
  bool started_ = false;
};

}  // namespace snd::platform

#endif  // SND_PLATFORM_MANUAL_OUTPUT_DEVICE_HPP
