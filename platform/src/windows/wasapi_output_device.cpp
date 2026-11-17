#include "windows/wasapi_output_device.hpp"

#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <new>

#include "backends.hpp"
#include "common/device_callback.hpp"
#include "snd/base/atomic.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"
#include "snd/types.h"
#include "windows/com_scope.hpp"
#include "windows/wasapi_format.hpp"
#include "windows/wasapi_guids.hpp"
#include "windows/wide_string.hpp"

namespace snd::platform::detail::wasapi {

namespace {
bool IsFloat32(const WAVEFORMATEX* f) noexcept {
  if (f->wBitsPerSample != 32u) return false;
  if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubtypeIeeeFloat) != 0;
  }
  return false;
}
}  // namespace

WasapiOutputDevice::WasapiOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept
    : allocator_(allocator),
      exclusive_(options.exclusive),
      reopenOnLoss_(options.reopenOnLoss),
      reopenIntervalMs_(options.reopenIntervalMs != 0u ? options.reopenIntervalMs : 500u) {}

WasapiOutputDevice::~WasapiOutputDevice() {
  Close();
  for (HANDLE h : {audioEvent_, notifyEvent_, commandEvent_, answerEvent_}) {
    if (h != nullptr) CloseHandle(h);
  }
}

bool WasapiOutputDevice::Init(const OutputDeviceOptions& options) noexcept {
  if (!optionName_.Assign(allocator_, options.deviceName)) return false;
  audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  notifyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  commandEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  answerEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return audioEvent_ != nullptr && notifyEvent_ != nullptr && commandEvent_ != nullptr && answerEvent_ != nullptr;
}

// --- owner thread ---------------------------------------------------------------------

bool WasapiOutputDevice::SendCommand(Command c) noexcept {
  command_.store(static_cast<std::uint32_t>(c), std::memory_order_release);
  SetEvent(commandEvent_);
  WaitForSingleObject(answerEvent_, INFINITE);
  return answer_.load(std::memory_order_acquire);
}

bool WasapiOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
  if (open_ || cb == nullptr || desc.channels > SND_MAX_CHANNELS) return false;
  request_ = desc;
  callback_ = cb;
  callbackUser_ = user;
  written_ = 0;
  lost_ = false;
  wantRunning_ = false;
  ResetClock();
  ResetEvent(answerEvent_);
  ThreadDesc td;
  td.name = "snd-wasapi";
  td.priority = ThreadPriority::Realtime;  // MMCSS "Pro Audio" is applied on the thread itself
  td.minStackBytes = kRtThreadMinStackBytes;
  if (!thread_.Start(td, &WasapiOutputDevice::ThreadMain, this)) return false;
  WaitForSingleObject(answerEvent_, INFINITE);  // the stream is open (or failed)
  if (!answer_.load(std::memory_order_acquire)) {
    thread_.Join();  // the thread has cleaned up and exited
    return false;
  }
  open_ = true;
  return true;
}

bool WasapiOutputDevice::Start() {
  if (!open_) return false;
  if (started_) return true;
  started_ = SendCommand(Command::Start);
  return started_;
}

void WasapiOutputDevice::Stop() {
  if (CalledFromCallback(this)) {  // the device thread would wait for itself
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!started_) return;
  SendCommand(Command::Stop);  // answered after the stream stopped: no callback after this
  started_ = false;
}

void WasapiOutputDevice::Close() {
  if (CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!open_) return;
  SendCommand(Command::Quit);
  thread_.Join();
  if (scratch_ != nullptr) allocator_.Free(scratch_, MemTag::Misc);
  scratch_ = nullptr;
  scratchSamples_ = 0;
  callback_ = nullptr;
  open_ = false;
  started_ = false;
}

// --- device thread ----------------------------------------------------------------------

void WasapiOutputDevice::ThreadMain(void* self) noexcept { static_cast<WasapiOutputDevice*>(self)->Run(); }

bool WasapiOutputDevice::ResolveDeviceId() noexcept {
  deviceId_[0] = L'\0';
  followDefault_ = true;
  if (!optionName_.Empty()) {
    if (!Utf8ToWide(optionName_.Get(), deviceId_, sizeof(deviceId_) / sizeof(deviceId_[0]))) return false;
    followDefault_ = false;
    return true;
  }
  if (request_.device_index == 0u) return true;
  ComPtr<IMMDeviceCollection> list;
  if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.Put()))) return false;
  UINT count = 0;
  if (FAILED(list->GetCount(&count))) return false;
  const std::int32_t at = ResolveDeviceIndex(request_.device_index, count);
  if (at < 0) return false;
  ComPtr<IMMDevice> dev;
  LPWSTR id = nullptr;
  if (FAILED(list->Item(static_cast<UINT>(at), dev.Put())) || FAILED(dev->GetId(&id))) return false;
  wcsncpy(deviceId_, id, sizeof(deviceId_) / sizeof(deviceId_[0]) - 1u);
  CoTaskMemFree(id);
  followDefault_ = false;
  return true;
}

bool WasapiOutputDevice::InitializeShared(std::uint32_t mixRate, std::uint32_t mixChannels) noexcept {
  const WasapiPlan plan = PlanWasapiStream(request_.sample_rate, request_.channels, false, SND_WASAPI_HAVE_CLIENT3 != 0,
                                           mixRate, mixChannels);
  if (plan.channels == 0u || plan.channels > SND_MAX_CHANNELS) return false;
  // The mix format itself when it is the requested format and float
  // (IAudioClient3's low-latency path requires the mix format); else ours.
  WAVEFORMATEXTENSIBLE fmt = MakeFormat(plan.sampleRate, plan.channels, SampleFormat::Float32);
  bool useMix = false;
  {
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client_->GetMixFormat(&mix)) && mix != nullptr) {
      const std::size_t size = sizeof(WAVEFORMATEX) + mix->cbSize;
      if (plan.mode != WasapiMode::SharedAutoConvert && IsFloat32(mix) && size <= sizeof(fmt)) {
        std::memcpy(&fmt, mix, size);
        useMix = true;
      }
      CoTaskMemFree(mix);
    }
  }
  WasapiMode mode = plan.mode;
  if (!useMix && mode != WasapiMode::SharedAutoConvert) mode = WasapiMode::SharedAutoConvert;  // mix is not float
  std::uint32_t period = 0;
  bool done = false;
#if SND_WASAPI_HAVE_CLIENT3
  if (mode == WasapiMode::SharedLowLatency) {
    ComPtr<IAudioClient3> c3;
    if (SUCCEEDED(client_->QueryInterface(kIidIAudioClient3, c3.PutVoid()))) {
      UINT32 def = 0, fundamental = 0, minP = 0, maxP = 0;
      if (SUCCEEDED(c3->GetSharedModeEnginePeriod(&fmt.Format, &def, &fundamental, &minP, &maxP))) {
        const std::uint32_t p = ChooseEnginePeriod(request_.period_frames, plan.sampleRate, fundamental, minP, maxP);
        if (p != 0u && SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p, &fmt.Format,
                                                                 nullptr))) {
          period = p;
          done = true;
        }
      }
    }
    if (!done) mode = WasapiMode::SharedNative;  // the client is still uninitialised
  }
#endif
  if (!done) {
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (FAILED(client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) return false;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (mode == WasapiMode::SharedAutoConvert) {
      flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    // Shared mode: periodicity must be 0; a 0 buffer duration asks for the minimum.
    const REFERENCE_TIME buffer =
        request_.period_frames != 0u ? FramesToHns(2ull * request_.period_frames, plan.sampleRate) : 0;
    if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &fmt.Format, nullptr))) return false;
    period = static_cast<std::uint32_t>(HnsToFrames(defaultPeriod, plan.sampleRate));
  }
  format_ = SampleFormat::Float32;
  rate_.store(plan.sampleRate, std::memory_order_relaxed);
  channels_.store(plan.channels, std::memory_order_relaxed);
  period_.store(period, std::memory_order_relaxed);
  return true;
}

bool WasapiOutputDevice::InitializeExclusive() noexcept {
  WAVEFORMATEX* mix = nullptr;
  if (FAILED(client_->GetMixFormat(&mix)) || mix == nullptr) return false;
  const std::uint32_t rate = request_.sample_rate != 0u ? request_.sample_rate : mix->nSamplesPerSec;
  const std::uint32_t channels = request_.channels != 0u ? request_.channels : mix->nChannels;
  CoTaskMemFree(mix);
  if (channels == 0u || channels > SND_MAX_CHANNELS) return false;
  WAVEFORMATEXTENSIBLE fmt;
  bool found = false;
  SampleFormat chosen = SampleFormat::Float32;
  for (const ExclusiveCandidate& c : kExclusiveCandidates) {
    fmt = MakeFormat(rate, channels, c.format, c.validBits);
    if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
      chosen = c.format;
      found = true;
      break;
    }
  }
  if (!found) return false;
  REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
  if (FAILED(client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) return false;
  REFERENCE_TIME period = request_.period_frames != 0u ? FramesToHns(request_.period_frames, rate) : defaultPeriod;
  if (period < minimumPeriod) period = minimumPeriod;
  HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                   &fmt.Format, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    // Documented recovery: take the aligned buffer size, make a new client, retry.
    UINT32 frames = 0;
    if (FAILED(client_->GetBufferSize(&frames))) return false;
    period = FramesToHns(frames, rate);
    if (FAILED(device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, client_.PutVoid()))) return false;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &fmt.Format,
                             nullptr);
  }
  if (FAILED(hr)) return false;
  format_ = chosen;
  rate_.store(rate, std::memory_order_relaxed);
  channels_.store(channels, std::memory_order_relaxed);
  return true;
}

bool WasapiOutputDevice::OpenStream() noexcept {
  ReleaseStream();
  const HRESULT hr = followDefault_ ? enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device_.Put())
                                    : enumerator_->GetDevice(deviceId_, device_.Put());
  if (FAILED(hr)) return false;
  if (FAILED(device_->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr, client_.PutVoid()))) {
    ReleaseStream();
    return false;
  }
  bool ok;
  if (exclusive_) {
    ok = InitializeExclusive();
  } else {
    WAVEFORMATEX* mix = nullptr;
    ok = SUCCEEDED(client_->GetMixFormat(&mix)) && mix != nullptr;
    std::uint32_t mixRate = 0, mixChannels = 0;
    if (ok) {
      mixRate = mix->nSamplesPerSec;
      mixChannels = mix->nChannels;
      CoTaskMemFree(mix);
    }
    ok = ok && InitializeShared(mixRate, mixChannels);
  }
  UINT32 bufferFrames = 0;
  ok = ok && SUCCEEDED(client_->GetBufferSize(&bufferFrames)) && bufferFrames > 0u;
  ok = ok && SUCCEEDED(client_->SetEventHandle(audioEvent_));
  ok = ok && SUCCEEDED(client_->GetService(kIidIAudioRenderClient, render_.PutVoid()));
  if (!ok) {
    ReleaseStream();
    return false;
  }
  bufferFrames_ = bufferFrames;
  clockFrequency_ = 0;
  if (SUCCEEDED(client_->GetService(kIidIAudioClock, clock_.PutVoid()))) {
    UINT64 f = 0;
    if (SUCCEEDED(clock_->GetFrequency(&f))) clockFrequency_ = f;
  }
  const std::uint32_t rate = rate_.load(std::memory_order_relaxed);
  const std::uint32_t channels = channels_.load(std::memory_order_relaxed);
  std::uint32_t period = exclusive_ ? bufferFrames : period_.load(std::memory_order_relaxed);
  if (period == 0u || period > bufferFrames) period = bufferFrames;
  period_.store(period, std::memory_order_relaxed);
  REFERENCE_TIME streamLatency = 0;
  client_->GetStreamLatency(&streamLatency);
  latency_.store(FramesToSeconds(bufferFrames, rate) + static_cast<double>(streamLatency) / 1e7,
                 std::memory_order_relaxed);
  if (format_ != SampleFormat::Float32) {
    std::size_t samples = 0;
    if (!base::CheckedMul(bufferFrames, channels, &samples)) return false;
    if (samples > scratchSamples_) {
      if (scratch_ != nullptr) allocator_.Free(scratch_, MemTag::Misc);
      scratch_ = static_cast<float*>(allocator_.Allocate(samples * sizeof(float), 64, MemTag::Misc));
      scratchSamples_ = scratch_ != nullptr ? samples : 0u;
      if (scratch_ == nullptr) {
        ReleaseStream();
        return false;
      }
    }
  }
  return true;
}

void WasapiOutputDevice::ReleaseStream() noexcept {
  if (streamRunning_ && client_) client_->Stop();
  streamRunning_ = false;
  render_.Reset();
  clock_.Reset();
  client_.Reset();
  device_.Reset();
}

bool WasapiOutputDevice::StartStream() noexcept {
  if (!client_) return false;
  if (streamRunning_) return true;
  // Silence in the whole free buffer first (avoids a glitch at start).
  UINT32 padding = 0;
  if (!exclusive_ && FAILED(client_->GetCurrentPadding(&padding))) padding = bufferFrames_;
  const UINT32 n = bufferFrames_ > padding ? bufferFrames_ - padding : 0u;
  BYTE* data = nullptr;
  if (n > 0u && SUCCEEDED(render_->GetBuffer(n, &data))) render_->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
  streamBase_ = written_;
  written_ += n;
  const HRESULT hr = client_->Start();
  if (FAILED(hr)) {
    HandleError(hr);
    return false;
  }
  streamRunning_ = true;
  return true;
}

void WasapiOutputDevice::StopStream() noexcept {
  if (!streamRunning_ || !client_) {
    streamRunning_ = false;
    return;
  }
  client_->Stop();
  client_->Reset();  // drops what is queued; the clock position restarts at 0
  streamRunning_ = false;
}

void WasapiOutputDevice::RenderAvailable() noexcept {
  if (!client_ || !streamRunning_) return;
  UINT32 padding = 0;
  if (!exclusive_) {
    const HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      HandleError(hr);
      return;
    }
    if (padding == 0u) statXruns_.fetch_add(1, std::memory_order_relaxed);  // the engine ran dry
  }
  const UINT32 n = exclusive_ ? bufferFrames_ : bufferFrames_ - padding;
  if (n == 0u) return;
  BYTE* data = nullptr;
  HRESULT hr = render_->GetBuffer(n, &data);
  if (FAILED(hr)) {
    HandleError(hr);
    return;
  }
  const std::uint32_t channels = channels_.load(std::memory_order_relaxed);
  const std::uint32_t period = period_.load(std::memory_order_relaxed);
  if (format_ == SampleFormat::Float32) {
    DeliverFrames(callback_, callbackUser_, reinterpret_cast<float*>(data), n, channels, period, statCallbacks_,
                  statFrames_);
  } else {
    DeliverFrames(callback_, callbackUser_, scratch_, n, channels, period, statCallbacks_, statFrames_);
    ConvertFromFloat(scratch_, data, static_cast<std::size_t>(n) * channels, format_);
  }
  hr = render_->ReleaseBuffer(n, 0);
  if (FAILED(hr)) {
    HandleError(hr);
    return;
  }
  written_ += n;
  const std::uint32_t rate = rate_.load(std::memory_order_relaxed);
  std::uint64_t queued = static_cast<std::uint64_t>(padding) + n;
  if (clock_ && clockFrequency_ != 0u) {
    UINT64 pos = 0, qpc = 0;
    if (SUCCEEDED(clock_->GetPosition(&pos, &qpc))) {
      const std::uint64_t played = streamBase_ + pos / clockFrequency_ * rate + (pos % clockFrequency_) * rate / clockFrequency_;
      queued = written_ > played ? written_ - played : 0u;
    }
  }
  PublishClock(written_, queued, rate);
}

void WasapiOutputDevice::HandleError(HRESULT hr) noexcept {
  const StreamErrorAction a = ClassifyWasapiHresult(static_cast<std::uint32_t>(hr));
  if (a == StreamErrorAction::None || a == StreamErrorAction::Retry) return;
  ReleaseStream();
  if (a == StreamErrorAction::Fatal) reopenOnLoss_ = false;
  if (!lost_) PostEvent(DeviceEventKind::Lost, 0, 0, 0);
  lost_ = true;
  nextReopenNs_ = MonotonicNanoseconds() + static_cast<std::uint64_t>(reopenIntervalMs_) * 1000000ull;
}

void WasapiOutputDevice::Reopen() noexcept {
  if (!OpenStream()) {
    if (!lost_) PostEvent(DeviceEventKind::Lost, 0, 0, 0);
    lost_ = true;
    nextReopenNs_ = MonotonicNanoseconds() + static_cast<std::uint64_t>(reopenIntervalMs_) * 1000000ull;
    return;
  }
  lost_ = false;
  statReopens_.fetch_add(1, std::memory_order_relaxed);
  // CHANGED before the first callback in the (possibly) new format (D-61).
  PostEvent(DeviceEventKind::Changed, channels_.load(std::memory_order_relaxed), rate_.load(std::memory_order_relaxed),
            period_.load(std::memory_order_relaxed));
  if (wantRunning_) StartStream();
}

void WasapiOutputDevice::CheckDeviceState() noexcept {
  if (lost_) {
    if (wantRunning_ && (reopenOnLoss_ || followDefault_)) Reopen();  // a device was added: try now
    return;
  }
  if (!device_) return;
  DWORD state = 0;
  if (SUCCEEDED(device_->GetState(&state)) && state != DEVICE_STATE_ACTIVE) HandleError(AUDCLNT_E_DEVICE_INVALIDATED);
}

void WasapiOutputDevice::HandleCommand() noexcept {
  const auto c = static_cast<Command>(command_.exchange(0, std::memory_order_acq_rel));
  bool ok = true;
  switch (c) {
    case Command::Start:
      wantRunning_ = true;
      ok = lost_ ? true : StartStream();  // a lost device starts when it comes back
      if (!ok) wantRunning_ = false;
      break;
    case Command::Stop:
    case Command::Quit:
      wantRunning_ = false;
      StopStream();
      break;
    case Command::None:
      return;
  }
  answer_.store(ok, std::memory_order_release);
  SetEvent(answerEvent_);
}

void WasapiOutputDevice::Run() noexcept {
  CallbackMarker mark(this);  // Stop()/Close() from here are misuse
  ScopedFlushDenormals ftz;
  ComScope com;
  DWORD task = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  if (mmcss != nullptr) {
    Workgroup wg;
    wg.kind = WorkgroupKind::WindowsMmcss;
    wg.taskIndex = task;
    PublishWorkgroup(wg);
  }
  bool ok = com.Usable() && SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                                       kIidIMMDeviceEnumerator, enumerator_.PutVoid()));
  if (ok) {
    notify_.SetWakeEvent(notifyEvent_);
    notifyRegistered_ = SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(&notify_));
    ok = ResolveDeviceId() && OpenStream();
  }
  answer_.store(ok, std::memory_order_release);
  SetEvent(answerEvent_);
  bool quit = !ok;
  while (!quit) {
    HANDLE handles[3] = {commandEvent_, notifyEvent_, audioEvent_};
    const DWORD count = streamRunning_ ? 3u : 2u;
    DWORD timeout = INFINITE;
    if (wantRunning_ && lost_ && (reopenOnLoss_ || followDefault_)) timeout = 50;
    else if (streamRunning_) timeout = 2000;
    const DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (r == WAIT_OBJECT_0) {
      const bool quitting = command_.load(std::memory_order_acquire) == static_cast<std::uint32_t>(Command::Quit);
      HandleCommand();
      quit = quitting;
    } else if (r == WAIT_OBJECT_0 + 1u) {
      if (notify_.TakeDefaultChanged() && followDefault_) Reopen();  // follow the new default (D-63)
      if (notify_.TakeStateChanged()) CheckDeviceState();
    } else if (r == WAIT_OBJECT_0 + 2u) {
      RenderAvailable();
    }
    if (!quit && wantRunning_ && lost_ && (reopenOnLoss_ || followDefault_) &&
        MonotonicNanoseconds() >= nextReopenNs_) {
      Reopen();
    }
  }
  ReleaseStream();
  if (notifyRegistered_) enumerator_->UnregisterEndpointNotificationCallback(&notify_);
  notifyRegistered_ = false;
  notify_.SetWakeEvent(nullptr);
  enumerator_.Reset();
  PublishWorkgroup(Workgroup{});
  if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace snd::platform::detail::wasapi

namespace snd::platform::detail {

PlatformOutputDevice* CreateWasapiOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept {
  auto* d = NewDevice<wasapi::WasapiOutputDevice>(allocator, allocator, options);
  if (d == nullptr) return nullptr;
  if (!d->Init(options)) {
    d->~WasapiOutputDevice();
    allocator.Free(d, MemTag::Misc);
    return nullptr;
  }
  return d;
}

}  // namespace snd::platform::detail
