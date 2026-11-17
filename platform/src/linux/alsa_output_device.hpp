// ALSA output device (Linux, CONTRACTS.md D-62, D-63).
//
// A dedicated device thread (Realtime with fallback, D-47 stack) writes one
// period at a time with snd_pcm_writei on a non-blocking PCM, sleeping in
// snd_pcm_wait (100 ms timeout so Stop() is never held up by a stalled
// device). writei rather than mmap: it works with every PCM plugin (dmix,
// pulse / pipewire ioplug, file, null, rate / plug conversion) while mmap
// is not offered by all of them, and the cost is one extra copy of one
// period (a few KiB) per wake-up. The callback renders into a float buffer;
// a device that refuses float gets S32 or S16, converted on the device
// thread outside the RT scope.
//
// Errors (ClassifyAlsaError): -EPIPE (xrun) -> snd_pcm_prepare, silence
// prefill, count; -ESTRPIPE (system suspend) -> SUSPENDED, snd_pcm_resume
// until it stops answering -EAGAIN (prepare if it fails), RESUMED; device
// gone (-ENODEV, -EBADFD, -EIO, ...) -> LOST, close, and with
// OutputDeviceOptions::reopenOnLoss try to open the same name again every
// reopenIntervalMs; success posts CHANGED with the new format.
#ifndef SND_PLATFORM_SRC_LINUX_ALSA_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_SRC_LINUX_ALSA_OUTPUT_DEVICE_HPP

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>

#include "common/owned_string.hpp"
#include "linux/alsa_pcm.hpp"
#include "snd/platform/platform_output_device.hpp"
#include "snd/platform/thread.hpp"

namespace snd::platform::detail {

class AlsaOutputDevice final : public PlatformOutputDevice {
 public:
  AlsaOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
  ~AlsaOutputDevice() override;
  AlsaOutputDevice(const AlsaOutputDevice&) = delete;
  AlsaOutputDevice& operator=(const AlsaOutputDevice&) = delete;

  // Copies the option strings. false on allocation failure.
  bool Init(const OutputDeviceOptions& options) noexcept;

  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override { return period_.load(std::memory_order_relaxed); }
  std::uint32_t SampleRate() const override { return rate_.load(std::memory_order_relaxed); }
  std::uint32_t Channels() const override { return channels_.load(std::memory_order_relaxed); }
  double OutputLatencySeconds() const override;
  OutputBackend Backend() const noexcept override { return OutputBackend::Alsa; }

 private:
  static void ThreadMain(void* self) noexcept;
  void Run() noexcept;
  bool OpenAndConfigure() noexcept;  // pcm_, stream_, buffers, atomics
  bool EnsureBuffers() noexcept;
  void ClosePcm() noexcept;
  bool PrefillSilence() noexcept;
  bool WriteFrames(const void* data, std::uint32_t frames) noexcept;  // false on error (handled)
  void HandleError(int err) noexcept;
  void WaitForReopen() noexcept;
  void UpdateClock() noexcept;

  IAllocator& allocator_;
  OwnedString optionName_;
  OwnedString config_;
  OwnedString resolvedName_;
  bool realtimeThread_ = true;
  bool reopenOnLoss_ = true;
  std::uint32_t reopenIntervalMs_ = 500;
  AlsaRequest request_{};

  snd_pcm_t* pcm_ = nullptr;
  AlsaStream stream_{};
  float* floatBuf_ = nullptr;
  void* convBuf_ = nullptr;
  std::size_t bufSamples_ = 0;  // capacity of both buffers in samples
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;
  std::uint64_t written_ = 0;
  bool open_ = false;
  bool started_ = false;
  bool fatal_ = false;
  std::atomic<bool> running_{false};
  std::atomic<std::uint32_t> rate_{0};
  std::atomic<std::uint32_t> channels_{0};
  std::atomic<std::uint32_t> period_{0};
  std::atomic<std::uint32_t> buffer_{0};
  Thread thread_;
};

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_LINUX_ALSA_OUTPUT_DEVICE_HPP
