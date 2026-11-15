// IOutputDevice (design.md 2.2, 2.6, 8.1, 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_OUTPUT_DEVICE_HPP
#define SND_EXT_OUTPUT_DEVICE_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Threads: Open/Start/Stop/Close/SetEventListener on the owner thread.
// The query functions may be called from any thread after Open.
// The device invokes the DeviceCallback on its own thread with interleaved
// float32 frames; the callback copies from the output ring and wakes the
// audio thread (2.2). Devices whose API has no callback drive it from a timer.
// Device changes, suspend/resume and focus changes are reported through the
// event listener from any thread; the listener only posts (never blocks).
struct IOutputDevice {
  virtual ~IOutputDevice() = default;
  virtual bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) = 0;
  virtual bool Start() = 0;
  virtual void Stop() = 0;   // returns after the last callback has finished
  virtual void Close() = 0;
  virtual std::uint32_t PeriodFrames() const = 0;
  virtual std::uint32_t SampleRate() const = 0;
  virtual std::uint32_t Channels() const = 0;
  virtual double OutputLatencySeconds() const = 0;  // beat notification correction (8.1)
  virtual void SetEventListener(DeviceEventFn fn, void* user) = 0;  // change, suspend
};

namespace bridge {
inline std::int32_t SND_CALL DevOpen(void* u, const snd_device_desc* d, snd_device_callback cb,
                                     void* cu) {
  return static_cast<IOutputDevice*>(u)->Open(*d, cb, cu) ? 1 : 0;
}
inline std::int32_t SND_CALL DevStart(void* u) { return static_cast<IOutputDevice*>(u)->Start() ? 1 : 0; }
inline void SND_CALL DevStop(void* u) { static_cast<IOutputDevice*>(u)->Stop(); }
inline void SND_CALL DevClose(void* u) { static_cast<IOutputDevice*>(u)->Close(); }
inline std::uint32_t SND_CALL DevPeriod(void* u) { return static_cast<IOutputDevice*>(u)->PeriodFrames(); }
inline std::uint32_t SND_CALL DevRate(void* u) { return static_cast<IOutputDevice*>(u)->SampleRate(); }
inline std::uint32_t SND_CALL DevChannels(void* u) { return static_cast<IOutputDevice*>(u)->Channels(); }
inline double SND_CALL DevLatency(void* u) {
  return static_cast<IOutputDevice*>(u)->OutputLatencySeconds();
}
inline void SND_CALL DevListener(void* u, snd_device_event_fn fn, void* fu) {
  static_cast<IOutputDevice*>(u)->SetEventListener(fn, fu);
}
}  // namespace bridge

inline snd_output_device ToC(IOutputDevice& d) noexcept {
  return snd_output_device{&d,
                           &bridge::DevOpen,
                           &bridge::DevStart,
                           &bridge::DevStop,
                           &bridge::DevClose,
                           &bridge::DevPeriod,
                           &bridge::DevRate,
                           &bridge::DevChannels,
                           &bridge::DevLatency,
                           &bridge::DevListener};
}

class COutputDevice final : public IOutputDevice {
 public:
  explicit COutputDevice(const snd_output_device& c) noexcept : c_(c) {}
  bool Open(const DeviceDesc& d, DeviceCallback cb, void* user) override {
    return c_.open(c_.user, &d, cb, user) != 0;
  }
  bool Start() override { return c_.start(c_.user) != 0; }
  void Stop() override { c_.stop(c_.user); }
  void Close() override { c_.close(c_.user); }
  std::uint32_t PeriodFrames() const override { return c_.period_frames(c_.user); }
  std::uint32_t SampleRate() const override { return c_.sample_rate(c_.user); }
  std::uint32_t Channels() const override { return c_.channels(c_.user); }
  double OutputLatencySeconds() const override { return c_.output_latency_seconds(c_.user); }
  void SetEventListener(DeviceEventFn fn, void* user) override { c_.set_event_listener(c_.user, fn, user); }

 private:
  snd_output_device c_;
};

}  // namespace snd

#endif  // SND_EXT_OUTPUT_DEVICE_HPP
