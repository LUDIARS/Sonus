// Composition root of the full runtime library `sonus` (CONTRACTS.md D-03, D-93).
//
// snd_create() needs objects from modules that api may not depend on at
// compile time (12.1): the source kinds, the music system, the IO scheduler
// and default file system, the platform output device and audio driver, and
// the live link. This library (api/compose) is the only place that sees all of
// them; it hands them to core through the contract interfaces. The order of
// creation and destruction is in snd/api/compose.hpp.
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>

#include "config_check.hpp"
#include "snd/api/api_module.hpp"
#include "snd/api/compose.hpp"
#include "snd/bank/bank_module.hpp"
#include "snd/base/allocator.hpp"
#include "snd/base/base_module.hpp"
#include "snd/codec/codec_module.hpp"
#include "snd/core/core_module.hpp"
#include "snd/dsp/dsp_module.hpp"
#include "snd/io/io_module.hpp"
#include "snd/jobs/jobs_module.hpp"
#include "snd/livelink/livelink_module.hpp"
#include "snd/music/music_module.hpp"
#include "snd/platform/platform_module.hpp"
#include "snd/source/runtime.hpp"
#include "snd/source/source_module.hpp"
#include "system.hpp"

namespace snd::compose {

// Names of the modules linked into this build, in dependency order.
const char* const* LinkedModules(unsigned* count) noexcept {
  static const char* const kModules[] = {
      base::ModuleName(),  platform::ModuleName(), jobs::ModuleName(),   io::ModuleName(),
      codec::ModuleName(), dsp::ModuleName(),      bank::ModuleName(),   core::ModuleName(),
      source::ModuleName(), music::ModuleName(),   api::ModuleName(),    livelink::ModuleName(),
  };
  if (count != nullptr) *count = static_cast<unsigned>(sizeof(kModules) / sizeof(kModules[0]));
  return kModules;
}

bool SourceKinds::Init(IAllocator& allocator, io::IoScheduler* io, io::StreamPool* streamPool,
                       std::uint32_t maxVoiceChannels, std::uint32_t maxBanks) noexcept {
  Destroy();
  void* mem = allocator.Allocate(sizeof(source::Runtime), 64, MemTag::Misc);
  if (mem == nullptr) return false;
  auto* rt = new (mem) source::Runtime();
  source::RuntimeDesc d;
  d.allocator = &allocator;
  d.io = io;
  d.streamPool = streamPool;
  d.maxVoiceChannels = maxVoiceChannels;
  d.maxBanks = maxBanks;
  if (!rt->Init(d)) {
    rt->~Runtime();
    allocator.Free(mem, MemTag::Misc);
    return false;
  }
  runtime_ = rt;
  alloc_ = &allocator;
  return true;
}

void SourceKinds::Destroy() noexcept {
  if (runtime_ == nullptr) return;
  auto* rt = static_cast<source::Runtime*>(runtime_);
  rt->~Runtime();
  alloc_->Free(rt, MemTag::Misc);
  runtime_ = nullptr;
  alloc_ = nullptr;
}

void SourceKinds::BankUnregistered(void* user, std::uint16_t bankSlot) noexcept {
  source::Runtime::BankUnregistered(user, bankSlot);
}

const source::SourceKindSet* SourceKinds::Kinds() const noexcept {
  return runtime_ != nullptr ? &static_cast<const source::Runtime*>(runtime_)->Kinds() : nullptr;
}

// ---------------------------------------------------------------------------------------
// Lifecycle observer (tests)
// ---------------------------------------------------------------------------------------
namespace {
std::atomic<LifecycleObserver> gObserver{nullptr};
std::atomic<void*> gObserverUser{nullptr};
}  // namespace

void SetLifecycleObserver(LifecycleObserver observer, void* user) noexcept {
  gObserverUser.store(user, std::memory_order_relaxed);
  gObserver.store(observer, std::memory_order_release);
}

void Note(const char* step) noexcept {
  const LifecycleObserver o = gObserver.load(std::memory_order_acquire);
  if (o != nullptr) o(gObserverUser.load(std::memory_order_relaxed), step);
}

// ---------------------------------------------------------------------------------------
// Audio driver bridge (audio thread)
// ---------------------------------------------------------------------------------------
namespace {

// Core renders Config.outputChannels; the device may have another layout (D-62: the
// conversion is the caller's). Common channels are copied; a mono device gets the mean
// of the first two core channels; a mono core feeds the first two device channels;
// further device channels are silent, further core channels are dropped (D-95).
void MapChannels(const float* in, std::uint32_t inCh, float* out, std::uint32_t outCh, std::uint32_t frames) noexcept {
  for (std::uint32_t f = 0; f < frames; ++f) {
    const float* src = in + std::size_t{f} * inCh;
    float* dst = out + std::size_t{f} * outCh;
    if (outCh == 1u && inCh >= 2u) {
      dst[0] = 0.5f * (src[0] + src[1]);
      continue;
    }
    for (std::uint32_t c = 0; c < outCh; ++c) {
      if (c < inCh) {
        dst[c] = src[c];
      } else if (inCh == 1u && c == 1u) {
        dst[c] = src[0];
      } else {
        dst[c] = 0.0f;
      }
    }
  }
}

void RenderBridge(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels,
                  const platform::RenderInfo* info) noexcept {
  snd_system& s = *static_cast<snd_system*>(user);
  // 2.2: the common clock keeps running through an underrun. Frames the device played
  // as silence are rendered (and discarded) here, at most underrun_catchup_blocks per
  // block, so playbacks, transports and reservations stay on the device's time line.
  if (info != nullptr) s.lostPending += info->lostFrames;
  for (std::uint32_t k = 0; k < s.config.tuning.underrun_catchup_blocks && s.lostPending >= s.blockFrames; ++k) {
    s.core->RenderBlock(nullptr);
    s.lostPending -= s.blockFrames;
  }
  if (channels == s.coreChannels && frames == s.blockFrames) {
    s.core->RenderBlock(interleaved);
  } else {
    std::uint32_t done = 0;
    while (done < frames) {
      std::uint32_t n = frames - done;
      if (n > s.blockFrames) n = s.blockFrames;
      s.core->Render(s.mix, n);
      MapChannels(s.mix, s.coreChannels, interleaved + std::size_t{done} * channels, channels, n);
      done += n;
    }
  }
  // 2.6 SND_FOCUS_SILENCE: the output fades to silence while the focus is lost (one
  // block ramp each way). PER_CATEGORY needs category flags core does not have yet.
  if (s.config.focus_mode == SND_FOCUS_SILENCE) {
    const float target = s.deviceEvents.focusLost.load(std::memory_order_relaxed) != 0u ? 0.0f : 1.0f;
    if (target != s.focusGain || target != 1.0f) {
      const float step = (target - s.focusGain) / static_cast<float>(frames == 0u ? 1u : frames);
      float g = s.focusGain;
      for (std::uint32_t f = 0; f < frames; ++f) {
        g += step;
        for (std::uint32_t c = 0; c < channels; ++c) interleaved[std::size_t{f} * channels + c] *= g;
      }
      s.focusGain = target;
    }
  }
}

void FormatChanged(void* user, std::uint32_t sampleRate, std::uint32_t channels) noexcept {
  snd_system& s = *static_cast<snd_system*>(user);
  s.deviceEvents.deviceRate.store(sampleRate, std::memory_order_relaxed);
  s.deviceEvents.deviceChannels.store(channels, std::memory_order_relaxed);
  if (sampleRate != 0u && sampleRate != s.config.sample_rate) {
    std::uint32_t expected = 0u;
    s.deviceEvents.rateMismatch.compare_exchange_strong(expected, 1u, std::memory_order_relaxed);
  }
}

void SND_CALL DeviceEvent(void* user, const snd_device_event* e) {
  if (e == nullptr) return;
  snd_system& s = *static_cast<snd_system*>(user);
  DeviceEventFlags& f = s.deviceEvents;
  switch (e->kind) {
    case SND_DEVICE_EVENT_SUSPENDED: f.suspended.store(1u, std::memory_order_release); break;
    case SND_DEVICE_EVENT_RESUMED: f.suspended.store(0u, std::memory_order_release); break;
    case SND_DEVICE_EVENT_LOST: f.lost.fetch_add(1u, std::memory_order_acq_rel); break;
    case SND_DEVICE_EVENT_FOCUS_LOST: f.focusLost.store(1u, std::memory_order_relaxed); break;
    case SND_DEVICE_EVENT_FOCUS_GAINED: f.focusLost.store(0u, std::memory_order_relaxed); break;
    default: break;  // CHANGED: the driver re-reads the format (FormatChanged)
  }
}

platform::OutputBackend BuiltInBackend(snd_output_kind kind) noexcept {
  switch (kind) {
    case SND_OUTPUT_SILENT: return platform::OutputBackend::SilentTimer;
    case SND_OUTPUT_MANUAL: return platform::OutputBackend::Manual;
    default: return platform::OutputBackend::Default;
  }
}

// Before each period of the manual device (snd_advance_output, D-102): wait until the audio
// thread has rendered its render-ahead target, so a slow caller never underruns. At most
// about 2 s; then the period plays what there is (an underrun, the clock catches up, 2.2).
void WaitRenderAhead(void* user) noexcept {
  snd_system& s = *static_cast<snd_system*>(user);
  if (!s.driverStarted) return;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    // The ring's fill, not rendered - consumed: the consumed count includes the silence of
    // underruns, which is never rendered, so after one bounded wait that difference would
    // stay short for good.
    const platform::DriverStats st = s.driver.Stats().core;
    if (st.targetFrames != 0u && st.queuedFrames >= st.targetFrames) return;
    if (std::chrono::steady_clock::now() > until) return;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
}

}  // namespace

std::uint32_t AdvanceManualDevice(snd_system& sys, std::uint32_t periods, float* out) noexcept {
  return sys.manual != nullptr && sys.driverStarted ? sys.manual->Advance(periods, out) : 0u;
}

// ---------------------------------------------------------------------------------------
// Compose / Decompose
// ---------------------------------------------------------------------------------------
snd_result Compose(snd_system& sys, const snd_allocator*, const snd_file_system* fileSystem) noexcept {
  IAllocator& a = *sys.alloc;
  const snd_config& cfg = sys.config;
  const bool live = LiveLinkWanted(cfg);
  sys.coreConfig = cfg;
  if (live) sys.coreConfig.max_registered_threads = cfg.max_registered_threads + 1u;  // the live link thread
  sys.limits = livelink::LimitsOf(sys.coreConfig);
  sys.coreChannels = cfg.output_channels == 0u ? 2u : cfg.output_channels;
  sys.blockFrames = cfg.block_size;

  // File system (6.5): the game's, else the OS file system under Config.file_system_root
  // (NULL / "" = the working directory; D-103: a root that is not a directory is NOT_FOUND).
  if (fileSystem != nullptr) {
    sys.fs = sys.cFileSystem.Emplace(*fileSystem);
  } else {
    io::DefaultFileSystemConfig fc;
    if (cfg.file_system_root != nullptr && cfg.file_system_root[0] != '\0') fc.root = cfg.file_system_root;
    if (!sys.defaultFs.Init(a, fc)) return SND_ERR_OUT_OF_BUDGET;
    sys.ownFs = true;
    if (!sys.defaultFs.RootAvailable()) return SND_ERR_NOT_FOUND;
    sys.fs = &sys.defaultFs;
    sys.defaultFs.SetCompletionNotify(&io::IoScheduler::WakeThunk, &sys.io);
  }
  Note("create:file-system");
  // Normal workers (2.3): the host's job system, else our own pool.
  if (cfg.jobs.external != nullptr) {
    sys.jobs = sys.cJobs.Emplace(*cfg.jobs.external);
  } else {
    jobs::WorkerPoolConfig wc;
    wc.workers = cfg.jobs.normal_workers == 0u ? 1u : cfg.jobs.normal_workers;
    if (!sys.workers.Init(a, wc)) return SND_ERR_OUT_OF_BUDGET;
    sys.ownWorkers = true;
    sys.jobs = &sys.workers;
  }
  Note("create:workers");
  // Codecs (D-31): plugins override the built-ins with the same fourcc.
  if (sys.codecs.Init(cfg.plugins.codecs, cfg.plugins.codec_count) != codec::RegistryStatus::Ok) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  Note("create:codecs");
  // Stream pool and IO scheduler: one channel per stream slot (channels >= maxStreams).
  if (!sys.streamPool.Init(a, cfg.memory.stream_pool_bytes, cfg.memory.stream_chunk_bytes, cfg.max_streams)) {
    return SND_ERR_OUT_OF_BUDGET;
  }
  if (!sys.io.Init(a, *sys.fs, sys.streamPool, io::MakeIoSchedulerConfig(sys.coreConfig))) return SND_ERR_OUT_OF_BUDGET;
  if (!sys.io.Start()) return SND_ERR_OUT_OF_BUDGET;
  sys.ioStarted = true;
  Note("create:io");
  if (!sys.kinds.Init(a, &sys.io, &sys.streamPool, cfg.max_voice_channels, cfg.max_banks)) return SND_ERR_OUT_OF_BUDGET;
  Note("create:source-kinds");
  sys.heldEnds = static_cast<snd_system::HeldEnd*>(
      a.Allocate(sizeof(snd_system::HeldEnd) * cfg.max_playbacks, 16, MemTag::Misc));
  if (sys.heldEnds == nullptr) return SND_ERR_OUT_OF_BUDGET;

  // Output device: its latency feeds core's notification timing (8.1). The value is the
  // device's estimate before Open (D-93). Without a game device, Config.output_kind picks
  // the platform's or a built-in silent one (D-102): those run at the internal rate with
  // the core's channels and one block per period.
  if (cfg.output.device != nullptr) {
    sys.device = sys.cDevice.Emplace(*cfg.output.device);
  } else {
    sys.outputAlloc.Bind(&a);
    sys.outputAlloc.Clear();
    sys.ownDevice = platform::CreateOutputDevice(BuiltInBackend(cfg.output_kind), sys.outputAlloc, nullptr);
    if (sys.ownDevice == nullptr) return sys.outputAlloc.Failed() ? SND_ERR_OUT_OF_BUDGET : SND_ERR_NO_DEVICE;
    sys.device = sys.ownDevice;
    if (sys.ownDevice->Backend() == platform::OutputBackend::Manual) {  // no RTTI: the backend id tells
      sys.manual = static_cast<platform::ManualOutputDevice*>(sys.ownDevice);
      sys.manual->SetBeforePeriod(&WaitRenderAhead, &sys);
    }
  }

  core::CoreDesc d;
  d.config = &sys.coreConfig;
  d.allocator = &a;
  d.sources = sys.kinds.Kinds();
  d.music = &sys.music;
  d.jobs = sys.jobs;
  d.bankScheduler = &sys.io;
  d.bankUnregistered = &SourceKinds::BankUnregistered;
  d.bankUser = sys.kinds.BankUser();
  d.findCodec = &codec::CodecRegistry::FindForSource;
  d.codecUser = &sys.codecs;
  d.outputLatencySeconds = sys.device->OutputLatencySeconds();
  d.limits.maxBuses = cfg.max_buses;
  d.limits.maxCategories = cfg.max_categories;
  d.limits.maxSnapshots = cfg.max_snapshots;
  d.limits.maxLimitGroups = cfg.max_limit_groups;
  d.limits.maxParamDefs = cfg.max_param_defs;
  d.limits.maxDuckRules = cfg.max_duck_rules;
  if (live) {
    livelink::RuntimeDesc rd;
    rd.config = &sys.coreConfig;
    rd.allocator = &a;
    const int r = livelink::LiveRuntime::Create(rd, &sys.live);
    if (r != SND_OK) return r;
    Note("create:live-runtime");
    sys.live->Install(&d);
    Note("create:live-install");
  }
  {
    const snd_result r = core::System::Create(d, &sys.core);
    if (r != SND_OK) return r;
  }
  Note("create:core");
  if (sys.live != nullptr) sys.live->Bind(sys.core);

  // Audio thread (2.2): blocks of Config.blockSize rendered by core.
  sys.mix = static_cast<float*>(a.Allocate(sizeof(float) * kMaxChannels * sys.blockFrames, 64, MemTag::Misc));
  if (sys.mix == nullptr) return SND_ERR_OUT_OF_BUDGET;
  platform::AudioDriverDesc ad;
  ad.core.blockFrames = sys.blockFrames;
  ad.core.renderAheadBlocks = cfg.render_ahead_blocks;
  ad.core.catchupBlocks = cfg.tuning.underrun_catchup_blocks;
  ad.core.loadTargetPercent = static_cast<double>(cfg.tuning.load_target_percent);
  ad.core.lod.downBlocks = cfg.tuning.lod_down_blocks;
  ad.core.lod.upBlocks = cfg.tuning.lod_up_blocks;
  ad.core.render = &RenderBridge;
  ad.core.formatChanged = &FormatChanged;
  ad.core.user = &sys;
  ad.listener = &DeviceEvent;
  ad.listenerUser = &sys;
  ad.workgroupSource = sys.ownDevice;  // a game device is opaque (no RTTI): no workgroup
  // The built-in silent devices take one block per period (D-102); the others their own.
  const bool silent = cfg.output.device == nullptr && cfg.output_kind != SND_OUTPUT_PLATFORM;
  const DeviceDesc dd{cfg.sample_rate, sys.coreChannels, silent ? sys.blockFrames : 0u, cfg.output.device_index};
  sys.outputAlloc.Bind(&a);
  sys.outputAlloc.Clear();
  if (!sys.driver.Start(*sys.device, dd, ad, sys.outputAlloc)) {
    return sys.outputAlloc.Failed() ? SND_ERR_OUT_OF_BUDGET : SND_ERR_NO_DEVICE;
  }
  sys.driverStarted = true;
  sys.core->SetOutputLatency(sys.device->OutputLatencySeconds());  // the opened device's value (8.1)
  Note("create:audio-driver");

  if (sys.live != nullptr) {
    livelink::ServerDesc sd;
    sd.system = sys.core;
    sd.runtime = sys.live;
    sd.allocator = &a;
    int r = livelink::Server::Create(sd, &sys.server);
    if (r != SND_OK) return r;
    Note("create:live-server");
    r = sys.server->Start();
    if (r != SND_OK) return r;
    Note("create:live-start");
  }
  return SND_OK;
}

void Decompose(snd_system& sys) noexcept {
  IAllocator* a = sys.alloc;
  if (sys.server != nullptr) {
    livelink::Server::Destroy(sys.server);  // stops its thread (unregisters it) first
    sys.server = nullptr;
    Note("destroy:live-server");
  }
  if (sys.driverStarted) {
    sys.driver.Stop();  // no render call after this
    sys.driverStarted = false;
    Note("destroy:audio-driver");
  }
  if (sys.ioStarted) {
    sys.io.Shutdown();
    sys.ioStarted = false;
    Note("destroy:io-shutdown");
  }
  if (sys.ownWorkers) {
    sys.workers.Shutdown();  // later submissions run inline (core's Destroy pumps its queue)
    sys.ownWorkers = false;
    Note("destroy:workers");
  }
  if (sys.core != nullptr) {
    core::System::Destroy(sys.core);
    sys.core = nullptr;
    Note("destroy:core");
  }
  if (sys.live != nullptr) {
    livelink::LiveRuntime::Destroy(sys.live);
    sys.live = nullptr;
    Note("destroy:live-runtime");
  }
  if (sys.kinds.Kinds() != nullptr) {
    sys.kinds.Destroy();
    Note("destroy:source-kinds");
  }
  sys.io.Destroy();
  sys.streamPool.Destroy();
  Note("destroy:io");
  if (sys.ownDevice != nullptr && a != nullptr) {
    platform::DestroyOutputDevice(sys.ownDevice, sys.outputAlloc);
    sys.ownDevice = nullptr;
    sys.manual = nullptr;
  }
  sys.cDevice.Reset();
  sys.device = nullptr;
  if (sys.ownFs) {
    sys.defaultFs.Shutdown();
    sys.ownFs = false;
  }
  sys.cFileSystem.Reset();
  sys.fs = nullptr;
  sys.cJobs.Reset();
  sys.jobs = nullptr;
  if (sys.mix != nullptr && a != nullptr) {
    a->Free(sys.mix, MemTag::Misc);
    sys.mix = nullptr;
  }
  if (sys.heldEnds != nullptr && a != nullptr) {
    a->Free(sys.heldEnds, MemTag::Misc);
    sys.heldEnds = nullptr;
    sys.heldCount = 0;
  }
  Note("destroy:services");
}

// ---------------------------------------------------------------------------------------
// Callbacks (owner thread, inside core's Update)
// ---------------------------------------------------------------------------------------
namespace {

bool CallbackBusy(const snd_system& s, std::uint64_t handle) noexcept {
  const source::Runtime* rt = s.kinds.SourceRuntime();
  return rt != nullptr && rt->CallbackBusy(handle);
}

void SND_CALL OnEnd(void* u, snd_playback_handle p, snd_end_reason r, snd_end_detail d) {
  snd_system& s = *static_cast<snd_system*>(u);
  if (s.callbacks.on_end == nullptr) return;
  // D-97: the game may free a synth's `user` in on_end; a render-ahead job that is still
  // posted or running would call it afterwards. Hold the record until it is done.
  if (CallbackBusy(s, p.value) && s.heldEnds != nullptr && s.heldCount < s.config.max_playbacks) {
    s.heldEnds[s.heldCount++] = snd_system::HeldEnd{p.value, r, d};
    return;
  }
  s.callbacks.on_end(s.callbacks.user, p, r, d);
}
void SND_CALL OnMarker(void* u, snd_playback_handle p, snd_id m, const snd_timing* t) {
  const snd_system& s = *static_cast<const snd_system*>(u);
  if (s.callbacks.on_marker != nullptr) s.callbacks.on_marker(s.callbacks.user, p, m, t);
}
void SND_CALL OnBeat(void* u, snd_transport_handle tr, const snd_musical_time* pos, const snd_timing* t) {
  const snd_system& s = *static_cast<const snd_system*>(u);
  if (s.callbacks.on_beat != nullptr) s.callbacks.on_beat(s.callbacks.user, tr, pos, t);
}
void SND_CALL OnError(void* u, snd_result code, std::uint32_t detail, std::uint64_t object) {
  const snd_system& s = *static_cast<const snd_system*>(u);
  if (s.callbacks.on_error != nullptr) s.callbacks.on_error(s.callbacks.user, code, detail, object);
}
void SND_CALL OnBank(void* u, snd_bank_handle b, snd_result r) {
  const snd_system& s = *static_cast<const snd_system*>(u);
  if (s.callbacks.on_bank_loaded != nullptr) s.callbacks.on_bank_loaded(s.callbacks.user, b, r);
}
void SND_CALL OnLog(void* u, snd_log_level l, const char* f, std::uint64_t a0, std::uint64_t a1) {
  const snd_system& s = *static_cast<const snd_system*>(u);
  if (s.callbacks.on_log != nullptr) s.callbacks.on_log(s.callbacks.user, l, f, a0, a1);
}

}  // namespace

snd_result SetCallbacks(snd_system& sys, const snd_callbacks* callbacks) noexcept {
  if (callbacks == nullptr) {
    const snd_result r = sys.core->SetCallbacks(nullptr);
    if (r != SND_OK) return r;
    sys.callbacks = snd_callbacks{};
    sys.hasCallbacks = false;
    return SND_OK;
  }
  snd_callbacks t{};
  t.user = &sys;
  t.on_end = callbacks->on_end != nullptr ? &OnEnd : nullptr;
  t.on_marker = callbacks->on_marker != nullptr ? &OnMarker : nullptr;
  t.on_beat = callbacks->on_beat != nullptr ? &OnBeat : nullptr;
  t.on_error = callbacks->on_error != nullptr ? &OnError : nullptr;
  t.on_bank_loaded = callbacks->on_bank_loaded != nullptr ? &OnBank : nullptr;
  t.on_log = callbacks->on_log != nullptr ? &OnLog : nullptr;
  const snd_result r = sys.core->SetCallbacks(&t);
  if (r != SND_OK) return r;
  sys.callbacks = *callbacks;
  sys.hasCallbacks = true;
  return SND_OK;
}

void DeliverHeldEnds(snd_system& sys) noexcept {
  std::uint32_t kept = 0;
  for (std::uint32_t i = 0; i < sys.heldCount; ++i) {
    const snd_system::HeldEnd e = sys.heldEnds[i];
    if (CallbackBusy(sys, e.handle)) {
      sys.heldEnds[kept++] = e;
    } else if (sys.callbacks.on_end != nullptr) {
      sys.callbacks.on_end(sys.callbacks.user, snd_playback_handle{e.handle}, e.reason, e.detail);
    }
  }
  sys.heldCount = kept;
}

bool ResumeDevice(snd_system& sys) noexcept {
  if (!sys.driverStarted || sys.deviceEvents.suspended.load(std::memory_order_acquire) == 0u) return false;
  if (!sys.driver.Resume()) return false;
  sys.deviceEvents.suspended.store(0u, std::memory_order_release);
  return true;
}

void ServiceDevice(snd_system& sys) noexcept {
  if (sys.resumeRequested.exchange(0u, std::memory_order_acq_rel) != 0u) ResumeDevice(sys);
  // Notices that came before the game installed on_error stay marked and are delivered
  // once, at the first snd_update after it did (D-95).
  if (!sys.hasCallbacks || sys.callbacks.on_error == nullptr) return;
  const std::uint32_t lost = sys.deviceEvents.lost.exchange(0u, std::memory_order_acq_rel);
  std::uint32_t mismatch = 1u;
  const bool rate = sys.deviceEvents.rateMismatch.compare_exchange_strong(mismatch, 2u, std::memory_order_acq_rel);
  for (std::uint32_t i = 0; i < lost; ++i) {
    sys.callbacks.on_error(sys.callbacks.user, SND_ERR_NO_DEVICE, SND_DEVICE_EVENT_LOST, 0u);
  }
  if (rate) {  // the device runs at another rate; the output is not converted (D-95)
    sys.callbacks.on_error(sys.callbacks.user, SND_ERR_UNSUPPORTED, 0u,
                           sys.deviceEvents.deviceRate.load(std::memory_order_relaxed));
  }
}

Parts PartsOf(snd_system* sys) noexcept {
  Parts p;
  if (sys == nullptr) return p;
  p.core = sys->core;
  p.music = &sys->music;
  p.sources = sys->kinds.SourceRuntime();
  p.io = &sys->io;
  p.workers = sys->ownWorkers ? &sys->workers : nullptr;
  p.driver = sys->driverStarted ? &sys->driver : nullptr;
  p.live = sys->live;
  p.server = sys->server;
  p.coreRegisteredThreads = sys->coreConfig.max_registered_threads;
  return p;
}

}  // namespace snd::compose
