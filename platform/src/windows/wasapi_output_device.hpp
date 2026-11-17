// WASAPI output device (Windows, CONTRACTS.md D-63).
//
// Threading: every COM object lives on the device thread, which is created
// at Open and runs until Close. It initialises COM (MTA), registers with
// MMCSS ("Pro Audio", published as the device's workgroup, D-65), creates
// the enumerator, registers the IMMNotificationClient and opens the stream;
// Open/Start/Stop/Close send it a command and wait for the answer, so the
// owner thread never touches COM. Queries read atomics.
//
// Stream (PlanWasapiStream, stream_policy.hpp):
//   shared, request == mix format, IAudioClient3 present ->
//       IAudioClient3::InitializeSharedAudioStream with the engine period
//       closest above the request (ChooseEnginePeriod) — the low-latency path;
//   shared, request == mix format, no IAudioClient3 -> IAudioClient::Initialize;
//   shared, different rate / channels -> IAudioClient::Initialize with
//       AUTOCONVERTPCM | SRC_DEFAULT_QUALITY: WASAPI converts, the callback
//       always sees the requested format (the rate conversion is WASAPI's,
//       not the caller's);
//   exclusive (OutputDeviceOptions::exclusive) -> IAudioClient::Initialize
//       (EXCLUSIVE, EVENTCALLBACK) with the first supported of float32,
//       int32, int24-in-32, int16 (converted from float on the device
//       thread), re-initialised with the aligned period on
//       AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED.
// All modes are event driven (AUDCLNT_STREAMFLAGS_EVENTCALLBACK). Shared
// mode fills `buffer - padding` frames per event, exclusive mode one buffer;
// the callback gets them in pieces of at most PeriodFrames() (D-61).
//
// Device changes (D-63): with device_index 0 and no deviceName the device
// follows the default console render endpoint: OnDefaultDeviceChanged
// re-opens the stream on the new default and posts CHANGED. Errors
// AUDCLNT_E_DEVICE_INVALIDATED / RESOURCES_INVALIDATED / SERVICE_NOT_RUNNING
// (unplugged, format changed in the control panel, audio service restart)
// and a device that leaves DEVICE_STATE_ACTIVE post LOST; while started the
// device thread re-opens every reopenIntervalMs (and at once when a device
// is added) and posts CHANGED when it succeeds. An empty buffer at a wake-up
// (padding 0 while running) is counted as an xrun.
#ifndef SND_PLATFORM_SRC_WINDOWS_WASAPI_OUTPUT_DEVICE_HPP
#define SND_PLATFORM_SRC_WINDOWS_WASAPI_OUTPUT_DEVICE_HPP

#include <atomic>
#include <cstdint>

#include "common/owned_string.hpp"
#include "common/sample_format.hpp"
#include "common/stream_policy.hpp"
#include "snd/platform/platform_output_device.hpp"
#include "snd/platform/thread.hpp"
#include "windows/com_ptr.hpp"
#include "windows/wasapi_notification_client.hpp"
#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

class WasapiOutputDevice final : public PlatformOutputDevice {
 public:
  WasapiOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept;
  ~WasapiOutputDevice() override;
  WasapiOutputDevice(const WasapiOutputDevice&) = delete;
  WasapiOutputDevice& operator=(const WasapiOutputDevice&) = delete;

  bool Init(const OutputDeviceOptions& options) noexcept;  // copies strings, creates the events

  bool Open(const DeviceDesc& desc, DeviceCallback cb, void* user) override;
  bool Start() override;
  void Stop() override;
  void Close() override;
  std::uint32_t PeriodFrames() const override { return period_.load(std::memory_order_relaxed); }
  std::uint32_t SampleRate() const override { return rate_.load(std::memory_order_relaxed); }
  std::uint32_t Channels() const override { return channels_.load(std::memory_order_relaxed); }
  double OutputLatencySeconds() const override { return latency_.load(std::memory_order_relaxed); }
  OutputBackend Backend() const noexcept override { return OutputBackend::Wasapi; }

 private:
  enum class Command : std::uint32_t { None = 0, Start = 1, Stop = 2, Quit = 3 };

  static void ThreadMain(void* self) noexcept;
  void Run() noexcept;
  bool SendCommand(Command c) noexcept;  // owner thread: signal and wait for the answer
  void HandleCommand() noexcept;

  // Device thread.
  bool OpenStream() noexcept;
  bool InitializeShared(std::uint32_t mixRate, std::uint32_t mixChannels) noexcept;
  bool InitializeExclusive() noexcept;
  void ReleaseStream() noexcept;
  bool StartStream() noexcept;
  void StopStream() noexcept;
  void RenderAvailable() noexcept;
  void HandleError(HRESULT hr) noexcept;
  void Reopen() noexcept;
  void CheckDeviceState() noexcept;
  bool ResolveDeviceId() noexcept;  // device_index -> id (fixed device)

  IAllocator& allocator_;
  OwnedString optionName_;
  bool exclusive_ = false;
  bool reopenOnLoss_ = true;
  std::uint32_t reopenIntervalMs_ = 500;

  // Request (owner thread writes before sending a command).
  DeviceDesc request_{};
  DeviceCallback callback_ = nullptr;
  void* callbackUser_ = nullptr;

  // Device thread state.
  ComPtr<IMMDeviceEnumerator> enumerator_;
  ComPtr<IMMDevice> device_;
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClock> clock_;
  NotificationClient notify_;
  bool notifyRegistered_ = false;
  wchar_t deviceId_[512] = {};   // fixed device (empty = follow the default)
  bool followDefault_ = true;
  SampleFormat format_ = SampleFormat::Float32;
  std::uint32_t bufferFrames_ = 0;
  std::uint64_t clockFrequency_ = 0;
  float* scratch_ = nullptr;     // exclusive mode with an integer format
  std::size_t scratchSamples_ = 0;
  std::uint64_t written_ = 0;     // frames handed to WASAPI since Open (incl. silent prefill)
  std::uint64_t streamBase_ = 0;  // written_ when the current stream started (IAudioClock origin)
  bool streamRunning_ = false;   // client Start()ed
  bool wantRunning_ = false;     // owner asked Start (survives re-opens)
  bool lost_ = false;
  std::uint64_t nextReopenNs_ = 0;
  HANDLE audioEvent_ = nullptr;  // SetEventHandle (auto-reset)
  HANDLE notifyEvent_ = nullptr; // NotificationClient (auto-reset)
  HANDLE commandEvent_ = nullptr;
  HANDLE answerEvent_ = nullptr;
  std::atomic<std::uint32_t> command_{0};
  std::atomic<bool> answer_{false};

  // Owner thread state.
  bool open_ = false;
  bool started_ = false;
  Thread thread_;

  // Queries.
  std::atomic<std::uint32_t> rate_{0};
  std::atomic<std::uint32_t> channels_{0};
  std::atomic<std::uint32_t> period_{0};
  std::atomic<double> latency_{0.0};
};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_WASAPI_OUTPUT_DEVICE_HPP
