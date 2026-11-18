#include "apple/coreaudio_output_device.hpp"

#include <os/object.h>
#include <os/workgroup.h>

#include <cstring>
#include <new>

#include "backends.hpp"
#include "common/device_callback.hpp"
#include "common/stream_policy.hpp"
#include "snd/platform/clock.hpp"
#include "snd/types.h"

#if !TARGET_OS_IPHONE
#include "apple/macos_devices.hpp"
#endif

namespace snd::platform::detail::coreaudio {

namespace {
// Large enough for the iOS lock screen (Apple recommends 4096 there).
constexpr std::uint32_t kMaxFramesPerSlice = 4096;

std::uint32_t ScaleFrames(std::uint32_t frames, std::uint32_t fromRate, std::uint32_t toRate) noexcept {
  if (fromRate == 0u || toRate == 0u || fromRate == toRate) return frames;
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frames) * toRate + fromRate - 1u) / fromRate);
}
}  // namespace

CoreAudioOutputDevice::CoreAudioOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept
    : allocator_(allocator), reopenOnLoss_(options.reopenOnLoss) {
#if TARGET_OS_IPHONE
  category_ = options.sessionCategory;
  manageSession_ = options.manageSession;
  (void)reopenOnLoss_;  // iOS rebuilds after a media-services reset regardless
#endif
}

CoreAudioOutputDevice::~CoreAudioOutputDevice() {
  Close();
  if (queue_ != nullptr) dispatch_release(queue_);
}

bool CoreAudioOutputDevice::Init(const OutputDeviceOptions& options) noexcept {
  if (!optionName_.Assign(allocator_, options.deviceName)) return false;
  queue_ = dispatch_queue_create("snd.coreaudio.control", DISPATCH_QUEUE_SERIAL);
  return queue_ != nullptr;
}

// --- render thread (RT) -------------------------------------------------------------

OSStatus CoreAudioOutputDevice::RenderThunk(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                                            UInt32, UInt32 frames, AudioBufferList* io) {
  return static_cast<CoreAudioOutputDevice*>(ref)->Render(flags, frames, io);
}

OSStatus CoreAudioOutputDevice::Render(AudioUnitRenderActionFlags* flags, UInt32 frames, AudioBufferList* io) noexcept {
  // Stop() clears running_ and then waits for active_ to drop to 0 (both
  // seq_cst), so a render that saw running_ == true is always waited for.
  active_.fetch_add(1, std::memory_order_seq_cst);
  const bool run = running_.load(std::memory_order_seq_cst);
  if (!run || io == nullptr || io->mNumberBuffers < 1u || io->mBuffers[0].mData == nullptr ||
      io->mBuffers[0].mNumberChannels == 0u) {
    if (io != nullptr) {
      for (UInt32 i = 0; i < io->mNumberBuffers; ++i) {
        if (io->mBuffers[i].mData != nullptr) std::memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
      }
    }
    if (flags != nullptr) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    active_.fetch_sub(1, std::memory_order_release);
    return noErr;
  }
  AudioBuffer& b = io->mBuffers[0];  // interleaved client format: one buffer
  const std::uint32_t channels = b.mNumberChannels;
  const std::uint32_t capacity = b.mDataByteSize / (4u * channels);
  const std::uint32_t n = frames < capacity ? frames : capacity;
  {
    CallbackMarker mark(this);
    DeliverFrames(callback_, callbackUser_, static_cast<float*>(b.mData), n, channels,
                  period_.load(std::memory_order_relaxed), statCallbacks_, statFrames_);
  }
  const std::uint64_t w = written_.load(std::memory_order_relaxed) + n;
  written_.store(w, std::memory_order_relaxed);
  PublishClock(w, latencyFrames_.load(std::memory_order_relaxed), rate_.load(std::memory_order_relaxed));
  active_.fetch_sub(1, std::memory_order_release);
  return noErr;
}

// --- control ----------------------------------------------------------------------------

void CoreAudioOutputDevice::StopUnit() noexcept {
  running_.store(false, std::memory_order_seq_cst);
  unit_.Stop();
  while (active_.load(std::memory_order_seq_cst) != 0u) SleepFor(100000);  // last render finishing
}

bool CoreAudioOutputDevice::StartUnit() noexcept {
  running_.store(true, std::memory_order_seq_cst);
  if (unit_.Start() != noErr) {
    running_.store(false, std::memory_order_seq_cst);
    return false;
  }
  return true;
}

void CoreAudioOutputDevice::ReleaseWorkgroups() noexcept {
  PublishWorkgroup(Workgroup{});
  for (std::uint32_t i = 0; i < workgroupCount_; ++i) {
    if (workgroups_[i] != nullptr) os_release(static_cast<os_workgroup_t>(workgroups_[i]));
    workgroups_[i] = nullptr;
  }
  workgroupCount_ = 0;
}

void CoreAudioOutputDevice::RefreshWorkgroup() noexcept {
  void* wg = unit_.CopyWorkgroup();
  if (wg == nullptr) return;
  if (workgroupCount_ > 0u && workgroups_[workgroupCount_ - 1u] == wg) {
    os_release(static_cast<os_workgroup_t>(wg));  // unchanged
    return;
  }
  if (workgroupCount_ == kMaxRetiredWorkgroups) {
    // Full: release the oldest (threads that joined it hold their own
    // reference, workgroup.hpp) and keep the new one.
    os_release(static_cast<os_workgroup_t>(workgroups_[0]));
    for (std::uint32_t i = 1; i < workgroupCount_; ++i) workgroups_[i - 1u] = workgroups_[i];
    --workgroupCount_;
  }
  workgroups_[workgroupCount_++] = wg;
  Workgroup g;
  g.kind = WorkgroupKind::AppleOsWorkgroup;
  g.native = wg;
  PublishWorkgroup(g);
}

void CoreAudioOutputDevice::RefreshRoute() noexcept {
  const std::uint32_t rate = rate_.load(std::memory_order_relaxed);
  std::uint32_t period = 0;
  double latency = unit_.UnitLatencySeconds();
  std::uint32_t latencyFrames = 0;
#if TARGET_OS_IPHONE
  const IosSessionInfo s = session_.Query();
  period = ScaleFrames(s.ioBufferFrames, s.sampleRate, rate);
  latency += s.outputLatency + FramesToSeconds(s.ioBufferFrames, s.sampleRate);
  latencyFrames = static_cast<std::uint32_t>(latency * rate + 0.5);
#else
  const AudioObjectID dev = unit_.CurrentDevice();
  if (dev != 0) {
    if (followDefault_) watch_.Retarget(dev);
    device_ = dev;
  }
  MacDeviceInfo info;
  if (device_ != 0 && MacQueryDevice(device_, &info)) {
    period = ScaleFrames(info.bufferFrames, info.nominalRate, rate);
    latency += FramesToSeconds(info.latencyFrames, info.nominalRate);
    latencyFrames = ScaleFrames(info.latencyFrames, info.nominalRate, rate);
  }
#endif
  const std::uint32_t slice = unit_.MaxFramesPerSlice();
  if (period == 0u) period = slice != 0u ? slice : 512u;
  period_.store(period, std::memory_order_relaxed);
  latency_.store(latency, std::memory_order_relaxed);
  latencyFrames_.store(latencyFrames, std::memory_order_relaxed);
  RefreshWorkgroup();
}

bool CoreAudioOutputDevice::BuildUnit() noexcept {
#if TARGET_OS_IPHONE
  if (unit_.Create(UnitKind::RemoteIo) != noErr) return false;
  const IosSessionInfo s = session_.Query();
  const std::uint32_t hwRate = s.sampleRate != 0u ? s.sampleRate : 48000u;
  const std::uint32_t hwChannels = s.outputChannels != 0u ? s.outputChannels : 2u;
#else
  if (unit_.Create(followDefault_ ? UnitKind::DefaultOutput : UnitKind::HalOutput) != noErr) return false;
  if (!followDefault_ && unit_.SetDevice(device_) != noErr) return false;
  const AudioObjectID dev = followDefault_ ? unit_.CurrentDevice() : device_;
  MacDeviceInfo info;
  if (dev == 0 || !MacQueryDevice(dev, &info)) return false;
  device_ = dev;
  const std::uint32_t hwRate = info.nominalRate;
  const std::uint32_t hwChannels = info.outputChannels != 0u ? info.outputChannels : 2u;
  if (request_.period_frames != 0u) {
    // Device IO size in device frames; best effort (the device may clamp it).
    const std::uint32_t fromRate = request_.sample_rate != 0u ? request_.sample_rate : hwRate;
    unit_.SetDeviceBufferFrames(ScaleFrames(request_.period_frames, fromRate, hwRate));
  }
#endif
  const std::uint32_t rate = request_.sample_rate != 0u ? request_.sample_rate : hwRate;
  std::uint32_t channels = request_.channels != 0u ? request_.channels : hwChannels;
  if (channels > SND_MAX_CHANNELS) channels = SND_MAX_CHANNELS;
  if (unit_.SetClientFormat(rate, channels) != noErr) return false;
  if (unit_.SetRenderCallback(&CoreAudioOutputDevice::RenderThunk, this) != noErr) return false;
  unit_.SetMaxFramesPerSlice(kMaxFramesPerSlice);
  if (unit_.Initialize() != noErr) return false;
  clientChannels_ = channels;
  rate_.store(rate, std::memory_order_relaxed);
  channels_.store(channels, std::memory_order_relaxed);
  RefreshRoute();
  return true;
}

bool CoreAudioOutputDevice::Reconfigure(std::uint32_t channels) noexcept {
  // Leaves the unit stopped: the caller posts CHANGED first and restarts
  // afterwards, so CHANGED precedes the first callback in the new format (D-62).
  StopUnit();
  unit_.Uninitialize();
  if (unit_.SetClientFormat(rate_.load(std::memory_order_relaxed), channels) != noErr || unit_.Initialize() != noErr) {
    return false;
  }
  clientChannels_ = channels;
  channels_.store(channels, std::memory_order_relaxed);
  return true;
}

void CoreAudioOutputDevice::ApplyChannelsAndAnnounce(std::uint32_t channels) noexcept {
  // channels == 0: the client layout is kept. Otherwise switch it; a failed
  // switch is reported as LOST (never silent output without an event).
#if TARGET_OS_IPHONE
  const bool restart = wantRunning_ && !interrupted_;
#else
  const bool restart = wantRunning_;
#endif
  if (channels != 0u && channels != clientChannels_) {
    if (!Reconfigure(channels)) {
      lost_ = true;
      PostEvent(DeviceEventKind::Lost, 0, 0, 0);
      return;
    }
    RefreshRoute();
    PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
    if (restart) StartUnit();
    return;
  }
  RefreshRoute();
  PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
}

bool CoreAudioOutputDevice::Open(const DeviceDesc& desc, DeviceCallback cb, void* user) {
  if (cb == nullptr || desc.channels > SND_MAX_CHANNELS) return false;
  base::LockGuard g(control_);
  if (open_) return false;
  request_ = desc;
  callback_ = cb;
  callbackUser_ = user;
  written_.store(0, std::memory_order_relaxed);
  ResetClock();
  lost_ = false;
  followDefault_ = optionName_.Empty() && desc.device_index == 0u;
#if TARGET_OS_IPHONE
  // iOS cannot choose the route (AVAudioSession does): only the default
  // (deviceName unset or "default", device_index 0) opens.
  if (desc.device_index != 0u || (!optionName_.Empty() && std::strcmp(optionName_.Get(), "default") != 0)) {
    return false;
  }
  followDefault_ = true;
  if (manageSession_) {
    if (!session_.Activate(category_, desc.sample_rate, desc.period_frames)) return false;
    sessionActive_ = true;
  }
  session_.StartObserving(&CoreAudioOutputDevice::OnOsEvent, this);
#else
  if (!followDefault_) {
    AudioObjectID dev = 0;
    if (!optionName_.Empty()) {
      if (!MacFindDeviceByUid(optionName_.Get(), &dev)) return false;
    } else {
      AudioObjectID list[256];
      const std::uint32_t n = MacOutputDevices(list, 256);
      const std::int32_t at = ResolveDeviceIndex(desc.device_index, n < 256u ? n : 256u);
      if (at < 0) return false;
      dev = list[at];
    }
    device_ = dev;
    if (!MacDeviceUid(dev, deviceUid_, sizeof(deviceUid_))) deviceUid_[0] = '\0';
  }
#endif
  if (!BuildUnit()) {
    unit_.Dispose();
#if TARGET_OS_IPHONE
    session_.StopObserving();
    if (sessionActive_) session_.Deactivate();
    sessionActive_ = false;
#endif
    return false;
  }
#if !TARGET_OS_IPHONE
  watch_.Start(device_, &CoreAudioOutputDevice::OnOsEvent, this);
#endif
  open_ = true;
  return true;
}

bool CoreAudioOutputDevice::Start() {
  base::LockGuard g(control_);
  if (!open_) return false;
  wantRunning_ = true;
#if TARGET_OS_IPHONE
  if (interrupted_) {
    // The app asks to resume after an interruption that did not say ShouldResume.
    if (manageSession_ && !session_.Reactivate()) return false;
    interrupted_ = false;
    unit_.Stop();  // the system stopped it; reset our flag
    if (!StartUnit()) return false;
    PostEvent(DeviceEventKind::Resumed, Channels(), SampleRate(), PeriodFrames());
    return true;
  }
#endif
  if (lost_ || running_.load(std::memory_order_relaxed)) return true;  // a lost device starts when back
  if (!StartUnit()) {
    wantRunning_ = false;
    return false;
  }
  return true;
}

void CoreAudioOutputDevice::Stop() {
  if (CalledFromCallback(this)) {  // from the render callback: waiting for ourselves would hang
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  base::LockGuard g(control_);
  wantRunning_ = false;
  StopUnit();  // returns after the last render finished
}

void CoreAudioOutputDevice::Close() {
  if (CalledFromCallback(this)) {
    statMisuse_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Listeners first (no new events), then drain the queue WITHOUT holding
  // the control lock (the queued handlers take it).
#if TARGET_OS_IPHONE
  session_.StopObserving();
#else
  watch_.Stop();
#endif
  if (queue_ != nullptr) dispatch_sync_f(queue_, nullptr, [](void*) {});
  base::LockGuard g(control_);
  if (!open_) return;
  wantRunning_ = false;
  StopUnit();
  unit_.Dispose();
  ReleaseWorkgroups();
#if TARGET_OS_IPHONE
  if (sessionActive_) session_.Deactivate();
  sessionActive_ = false;
#endif
  callback_ = nullptr;
  open_ = false;
}

// --- OS events ----------------------------------------------------------------------------

void CoreAudioOutputDevice::OnOsEvent(void* self, std::uint32_t bits) {
  auto* d = static_cast<CoreAudioOutputDevice*>(self);
  // Coalesce: only the first pending bit schedules the queue.
  if (d->pendingBits_.fetch_or(bits, std::memory_order_acq_rel) == 0u) {
    dispatch_async_f(d->queue_, d, &CoreAudioOutputDevice::ProcessEventsThunk);
  }
}

void CoreAudioOutputDevice::ProcessEventsThunk(void* self) { static_cast<CoreAudioOutputDevice*>(self)->ProcessEvents(); }

void CoreAudioOutputDevice::ProcessEvents() noexcept {
  const std::uint32_t bits = pendingBits_.exchange(0, std::memory_order_acq_rel);
  base::LockGuard g(control_);
  if (!open_ || bits == 0u) return;
  const std::uint32_t oldRate = rate_.load(std::memory_order_relaxed);
  const std::uint32_t oldChannels = channels_.load(std::memory_order_relaxed);
  const std::uint32_t oldPeriod = period_.load(std::memory_order_relaxed);
#if TARGET_OS_IPHONE
  if ((bits & kIosMediaLost) != 0u) {
    lost_ = true;
    PostEvent(DeviceEventKind::Lost, 0, 0, 0);
  }
  if ((bits & kIosMediaReset) != 0u) {
    // Every audio object is invalid: rebuild the session and the unit.
    if (!lost_) PostEvent(DeviceEventKind::Lost, 0, 0, 0);
    StopUnit();
    unit_.Dispose();
    ReleaseWorkgroups();
    lost_ = true;
    if ((!manageSession_ || session_.Activate(category_, request_.sample_rate, request_.period_frames)) &&
        BuildUnit()) {
      lost_ = false;
      statReopens_.fetch_add(1, std::memory_order_relaxed);
      PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
      if (wantRunning_) StartUnit();
    }
    return;
  }
  // Began and ended may be coalesced: follow the last one.
  const bool anyInterrupt = (bits & (kIosInterruptBegan | kIosInterruptEnded)) != 0u;
  if (anyInterrupt) {
    const bool began = (bits & kIosInterruptEnded) == 0u ||
                       ((bits & kIosInterruptBegan) != 0u && session_.LastInterruptionBegan());
    if ((bits & kIosInterruptBegan) != 0u && !interrupted_) {  // a coalesced began still suspends first
      running_.store(false, std::memory_order_seq_cst);  // the system stopped the unit
      interrupted_ = true;
      PostEvent(DeviceEventKind::Suspended, 0, 0, 0);
    }
    // An ended without a preceding began (not interrupted) does nothing.
    if (!began && interrupted_ && session_.LastInterruptionShouldResume()) {
      // Ended with ShouldResume: reactivate and continue. Without it the
      // device stays SUSPENDED until the app calls Start() again.
      if (!manageSession_ || session_.Reactivate()) {
        interrupted_ = false;
        unit_.Stop();  // reset our started flag; the unit was stopped by the system
        if (wantRunning_) StartUnit();
        PostEvent(DeviceEventKind::Resumed, Channels(), SampleRate(), PeriodFrames());
      }
    }
  }
  if ((bits & kIosRouteChanged) != 0u) {
    const IosSessionInfo s = session_.Query();
    std::uint32_t want = 0;
    if (request_.channels == 0u && s.outputChannels != 0u) {
      want = s.outputChannels <= SND_MAX_CHANNELS ? s.outputChannels : SND_MAX_CHANNELS;
    }
    const std::uint32_t reason = session_.LastRouteReason();
    if (want != 0u && want != clientChannels_) {
      ApplyChannelsAndAnnounce(want);  // CHANGED (or LOST) before the unit restarts
    } else {
      RefreshRoute();
      const bool differs = Channels() != oldChannels || SampleRate() != oldRate || PeriodFrames() != oldPeriod;
      const std::uint32_t ev = RouteChangeEvent(reason, differs);
      if (ev == SND_DEVICE_EVENT_LOST) {
        PostEvent(DeviceEventKind::Lost, 0, 0, 0);
      } else if (ev == SND_DEVICE_EVENT_CHANGED) {
        PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
      }
    }
  }
#else
  (void)oldChannels;
  if ((bits & kMacDefaultChanged) != 0u && followDefault_) {
    // DefaultOutput already moved to the new device; adopt its format.
    const AudioObjectID dev = unit_.CurrentDevice();
    MacDeviceInfo info;
    std::uint32_t want = 0;
    if (dev != 0 && MacQueryDevice(dev, &info) && request_.channels == 0u && info.outputChannels != 0u) {
      want = info.outputChannels <= SND_MAX_CHANNELS ? info.outputChannels : SND_MAX_CHANNELS;
    }
    lost_ = false;
    statReopens_.fetch_add(1, std::memory_order_relaxed);
    ApplyChannelsAndAnnounce(want);  // CHANGED before the restart; LOST if the switch fails
  }
  if ((bits & kMacDeviceDied) != 0u && !followDefault_) {
    MacDeviceInfo info;
    if (!MacQueryDevice(device_, &info) || !info.alive) {
      if (!lost_) PostEvent(DeviceEventKind::Lost, 0, 0, 0);
      lost_ = true;
    }
  }
  if ((bits & kMacDevicesChanged) != 0u && lost_ && !followDefault_ && reopenOnLoss_ && deviceUid_[0] != '\0') {
    AudioObjectID dev = 0;
    if (MacFindDeviceByUid(deviceUid_, &dev)) {
      // The same device (UID) is back with a new object id: rebuild on it.
      StopUnit();
      unit_.Dispose();
      device_ = dev;
      if (BuildUnit()) {
        lost_ = false;
        watch_.Retarget(device_);
        statReopens_.fetch_add(1, std::memory_order_relaxed);
        PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
        if (wantRunning_) StartUnit();
      }
    }
  }
  if ((bits & kMacDeviceFormat) != 0u && (bits & kMacDefaultChanged) == 0u) {
    RefreshRoute();
    if (PeriodFrames() != oldPeriod || SampleRate() != oldRate) {
      PostEvent(DeviceEventKind::Changed, Channels(), SampleRate(), PeriodFrames());
    }
  }
#endif
}

}  // namespace snd::platform::detail::coreaudio

namespace snd::platform::detail {

PlatformOutputDevice* CreateCoreAudioOutputDevice(IAllocator& allocator, const OutputDeviceOptions& options) noexcept {
  auto* d = NewDevice<coreaudio::CoreAudioOutputDevice>(allocator, allocator, options);
  if (d == nullptr) return nullptr;
  if (!d->Init(options)) {
    d->~CoreAudioOutputDevice();
    allocator.Free(d, MemTag::Misc);
    return nullptr;
  }
  return d;
}

}  // namespace snd::platform::detail
