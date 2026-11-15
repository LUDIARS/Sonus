// Silent output device driven by the test (design.md 12.2-6).
//
// Implements snd::IOutputDevice. Nothing is played: the test calls Pump() to
// run the device callback for N periods; every period's interleaved output is
// appended to an in-memory capture. Period size, sample rate, channels and
// reported latency are configurable; device events (route change, suspend,
// focus) can be injected with EmitEvent().
//
//   snd::mock::NullOutputDevice dev({.sampleRate = 48000, .channels = 2, .periodFrames = 256});
//   cfg.output.device = &dev;          // C++ API
//   ... dev.Pump(4); auto left = dev.CapturedChannel(0);
#ifndef SND_MOCKS_NULL_OUTPUT_DEVICE_HPP
#define SND_MOCKS_NULL_OUTPUT_DEVICE_HPP

#include <atomic>
#include <cstdint>
#include <vector>

#include "snd/ext/output_device.hpp"

namespace snd::mock {

struct NullDeviceConfig {
  std::uint32_t sampleRate = 48000;
  std::uint32_t channels = 2;
  std::uint32_t periodFrames = 256;
  double latencySeconds = 0.010;
  // true: non-zero values in the DeviceDesc passed to Open override the above.
  bool honorRequest = true;
  // Open() fails (simulates "no device", SND_ERR_NO_DEVICE).
  bool failOpen = false;
  // Value written into the period buffer before the callback; the callback must overwrite it.
  float fillValue = 0.0f;
};

class NullOutputDevice final : public IOutputDevice {
 public:
  explicit NullOutputDevice(const NullDeviceConfig& config = NullDeviceConfig{});
  ~NullOutputDevice() override;

  // IOutputDevice
  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override;
  std::uint32_t SampleRate() const override;
  std::uint32_t Channels() const override;
  double OutputLatencySeconds() const override;
  void SetEventListener(DeviceEventFn fn, void* user) override;

  // --- test control (test thread) ------------------------------------------
  // Runs the callback `periods` times if open and started. Returns periods run.
  std::uint32_t Pump(std::uint32_t periods = 1);
  // Delivers an event to the listener (any kind). CHANGED also applies the
  // event's non-zero channels / sampleRate / periodFrames to the device.
  void EmitEvent(const DeviceEvent& event);

  bool IsOpen() const { return open_; }
  bool IsStarted() const { return started_; }
  std::uint64_t PeriodsPumped() const { return periods_; }
  std::uint64_t FramesPumped() const { return frames_; }
  std::uint32_t OpenCount() const { return openCount_; }

  // Interleaved capture of everything pumped since the last ClearCapture().
  const std::vector<float>& Captured() const { return capture_; }
  std::uint64_t CapturedFrames() const;
  std::vector<float> CapturedChannel(std::uint32_t channel) const;
  void ClearCapture() { capture_.clear(); }
  void SetCaptureEnabled(bool enabled) { captureEnabled_ = enabled; }

 private:
  NullDeviceConfig config_;
  std::uint32_t sampleRate_;
  std::uint32_t channels_;
  std::uint32_t periodFrames_;
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;
  DeviceEventFn listener_ = nullptr;
  void* listenerUser_ = nullptr;
  bool open_ = false;
  bool started_ = false;
  bool captureEnabled_ = true;
  std::uint32_t openCount_ = 0;
  std::uint64_t periods_ = 0;
  std::uint64_t frames_ = 0;
  std::vector<float> period_;
  std::vector<float> capture_;
};

}  // namespace snd::mock

#endif  // SND_MOCKS_NULL_OUTPUT_DEVICE_HPP
