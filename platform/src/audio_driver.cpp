#include "snd/platform/audio_driver.hpp"

#include "snd/internal/rt_check.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::platform {

namespace {
// The driver whose audio thread or device callback the calling thread runs.
thread_local const AudioDriver* tInDriver = nullptr;

struct DriverMarker {
  explicit DriverMarker(const AudioDriver* d) noexcept : prev(tInDriver) { tInDriver = d; }
  ~DriverMarker() { tInDriver = prev; }
  DriverMarker(const DriverMarker&) = delete;
  DriverMarker& operator=(const DriverMarker&) = delete;
  const AudioDriver* prev;
};
}  // namespace

AudioDriver::~AudioDriver() { Stop(); }

void AudioDriver::DeviceCallbackThunk(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels) {
  auto* self = static_cast<AudioDriver*>(user);
  DriverMarker mark(self);
  self->core_.OnDeviceCallback(interleaved, frames, channels);
  self->wake_.Post();  // 2.2: copy, then wake the audio thread
}

void AudioDriver::DeviceEventThunk(void* user, const DeviceEvent* event) {
  auto* self = static_cast<AudioDriver*>(user);
  if (event == nullptr) return;
  self->events_.fetch_add(1, std::memory_order_relaxed);
  const auto kind = static_cast<DeviceEventKind>(event->kind);
  if (kind == DeviceEventKind::Changed) {
    self->core_.OnDeviceChanged(event->sample_rate, event->period_frames);
    self->wake_.Post();
  } else if (kind == DeviceEventKind::Resumed) {
    // 2.6: fill the render-ahead before the device consumes again.
    self->prefillRequested_.store(true, std::memory_order_release);
    self->wake_.Post();
  }
  if (self->desc_.listener != nullptr) self->desc_.listener(self->desc_.listenerUser, event);
}

void AudioDriver::ThreadMain(void* self) noexcept { static_cast<AudioDriver*>(self)->Run(); }

void AudioDriver::Run() noexcept {
  ScopedFlushDenormals ftz;  // 2.4
  DriverMarker mark(this);
  WorkgroupMembership membership;
  std::uint32_t generation = 0;
  if (desc_.workgroupSource != nullptr) {
    const WorkgroupJoinResult r = membership.Join(desc_.workgroupSource->CurrentWorkgroup(&generation));
    wgResult_.store(static_cast<std::uint8_t>(r), std::memory_order_relaxed);
  }
  {
    SND_RT_SCOPE();
    core_.Service(true);
  }
  prefilled_.store(true, std::memory_order_release);
  ready_.Post();
  for (;;) {
    wake_.Wait();
    // Drain the extra permits BEFORE looking at running_: Stop() stores false
    // and then posts, so a drained post implies its store is visible.
    while (wake_.TryWait()) {
    }
    if (!running_.load(std::memory_order_acquire)) break;
    wakeups_.fetch_add(1, std::memory_order_relaxed);
    if (desc_.workgroupSource != nullptr && desc_.workgroupSource->WorkgroupGeneration() != generation) {
      const WorkgroupJoinResult r = membership.Join(desc_.workgroupSource->CurrentWorkgroup(&generation));
      wgResult_.store(static_cast<std::uint8_t>(r), std::memory_order_relaxed);
    }
    const bool prefill = prefillRequested_.exchange(false, std::memory_order_acq_rel);
    SND_RT_SCOPE();
    core_.Service(prefill);
  }
  membership.Leave();
}

bool AudioDriver::Start(IOutputDevice& device, const DeviceDesc& deviceDesc, const AudioDriverDesc& desc,
                        IAllocator& allocator) noexcept {
  if (device_ != nullptr || tInDriver == this) return false;
  desc_ = desc;
  prefillRequested_.store(false, std::memory_order_relaxed);
  prefilled_.store(false, std::memory_order_relaxed);
  while (wake_.TryWait()) {
  }
  while (ready_.TryWait()) {
  }
  device.SetEventListener(&AudioDriver::DeviceEventThunk, this);
  if (!device.Open(deviceDesc, &AudioDriver::DeviceCallbackThunk, this)) {
    device.SetEventListener(nullptr, nullptr);
    return false;
  }
  if (!core_.Init(allocator, desc.core, device.SampleRate(), device.Channels(), device.PeriodFrames())) {
    device.Close();
    device.SetEventListener(nullptr, nullptr);
    return false;
  }
  running_.store(true, std::memory_order_release);
  ThreadDesc td;
  td.name = "snd-audio";
  td.priority = desc.realtimeThread ? ThreadPriority::Realtime : ThreadPriority::Normal;
  td.minStackBytes = kRtThreadMinStackBytes;  // D-47
  bool ok = thread_.Start(td, &AudioDriver::ThreadMain, this);
  if (ok) ok = ready_.WaitFor(static_cast<std::uint64_t>(desc.prefillTimeoutMs) * 1000000ull);
  if (ok) ok = device.Start();
  if (!ok) {
    running_.store(false, std::memory_order_release);
    wake_.Post();
    thread_.Join();
    device.Close();
    device.SetEventListener(nullptr, nullptr);
    core_.Release();
    return false;
  }
  device_ = &device;
  return true;
}

void AudioDriver::Stop() noexcept {
  if (tInDriver == this) {  // from the render function or the device callback: would deadlock
    misuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (device_ == nullptr) return;
  device_->Stop();  // no callback after this returns
  running_.store(false, std::memory_order_release);
  wake_.Post();
  thread_.Join();
  device_->Close();
  device_->SetEventListener(nullptr, nullptr);
  core_.Release();
  device_ = nullptr;
}

AudioDriverStats AudioDriver::Stats() const noexcept {
  AudioDriverStats s;
  s.core = core_.Stats();
  s.wakeups = wakeups_.load(std::memory_order_relaxed);
  s.events = events_.load(std::memory_order_relaxed);
  s.misuse = misuse_.load(std::memory_order_relaxed);
  s.priority = thread_.Priority();
  s.workgroup = static_cast<WorkgroupJoinResult>(wgResult_.load(std::memory_order_relaxed));
  return s;
}

}  // namespace snd::platform
