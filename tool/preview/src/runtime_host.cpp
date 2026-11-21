// A Sonus runtime inside the tool (preview engine and replay host).
#include "runtime_host.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "snd/base/allocator.hpp"

namespace snd::tool::preview {
namespace ll = livelink;

namespace {
constexpr std::uint32_t kRecentBlocks = 32;
IAllocator& Alloc() { return base::DefaultAllocator(); }
}  // namespace

snd_result RuntimeHost::Create(const RuntimeHostDesc& desc, std::string* error) {
  auto fail = [&](snd_result r, const char* what) {
    if (error != nullptr) *error = what;
    Destroy();
    return r;
  };
  Destroy();
  IAllocator& a = Alloc();
  config_ = desc.config;
  output_ = desc.output;
  coreConfig_ = config_;
  coreConfig_.livelink.enabled = desc.hooks == HostHooks::Live ? 1u : 0u;
  if (desc.hooks == HostHooks::Live) coreConfig_.max_registered_threads = config_.max_registered_threads + 1u;
  channels_ = config_.output_channels == 0u ? 2u : config_.output_channels;
  blockFrames_ = config_.block_size;

  jobs::WorkerPoolConfig wc;
  wc.workers = config_.jobs.normal_workers == 0u ? 1u : config_.jobs.normal_workers;
  wc.threadName = "snd-preview-worker";
  if (!workers_.Init(a, wc)) return fail(SND_ERR_OUT_OF_BUDGET, "normal workers");
  workersStarted_ = true;
  if (codecs_.Init(nullptr, 0) != codec::RegistryStatus::Ok) return fail(SND_ERR_INVALID_ARGUMENT, "codecs");
  if (!streamPool_.Init(a, config_.memory.stream_pool_bytes, config_.memory.stream_chunk_bytes, config_.max_streams)) {
    return fail(SND_ERR_OUT_OF_BUDGET, "stream pool");
  }
  if (!io_.Init(a, fs_, streamPool_, io::MakeIoSchedulerConfig(coreConfig_))) return fail(SND_ERR_OUT_OF_BUDGET, "io");
  ioInit_ = true;
  if (!io_.Start()) return fail(SND_ERR_OUT_OF_BUDGET, "io thread");
  ioStarted_ = true;
  if (!kinds_.Init(a, &io_, &streamPool_, config_.max_voice_channels, config_.max_banks)) {
    return fail(SND_ERR_OUT_OF_BUDGET, "source kinds");
  }

  core::CoreDesc d;
  d.config = &coreConfig_;
  d.allocator = &a;
  d.sources = kinds_.Kinds();
  d.music = &music_;
  d.jobs = &workers_;
  d.bankScheduler = &io_;
  d.bankUnregistered = &compose::SourceKinds::BankUnregistered;
  d.bankUser = kinds_.BankUser();
  d.findCodec = &codec::CodecRegistry::FindForSource;
  d.codecUser = &codecs_;
  d.rtWorkerRealtimePriority = false;
  d.limits.maxBuses = config_.max_buses;
  d.limits.maxCategories = config_.max_categories;
  d.limits.maxSnapshots = config_.max_snapshots;
  d.limits.maxLimitGroups = config_.max_limit_groups;
  d.limits.maxParamDefs = config_.max_param_defs;
  d.limits.maxDuckRules = config_.max_duck_rules;
  if (desc.hooks == HostHooks::Live) {
    ll::RuntimeDesc rd;
    rd.config = &coreConfig_;
    rd.allocator = &a;
    rd.structEditor = desc.structEditor;
    rd.chunkSlots = desc.chunkSlots == 0u ? 1u : desc.chunkSlots;
    const int r = ll::LiveRuntime::Create(rd, &live_);
    if (r != SND_OK) return fail(r, "live runtime");
    live_->Install(&d);
  } else if (desc.hooks == HostHooks::Replay && desc.replayer != nullptr) {
    replayer_ = desc.replayer;
    replayer_->Install(&d);
  }
  {
    const snd_result r = core::System::Create(d, &sys_);
    if (r != SND_OK) return fail(r, "core System::Create (Config out of range?)");
  }
  if (live_ != nullptr) live_->Bind(sys_);
  if (replayer_ != nullptr) replayer_->Bind(sys_);
  mix_.assign(std::size_t{blockFrames_} * channels_, 0.0f);
  recentSamples_ = std::size_t{kRecentBlocks} * blockFrames_ * channels_;
  recent_ = std::make_unique<std::atomic<float>[]>(recentSamples_);
  for (std::size_t i = 0; i < recentSamples_; ++i) recent_[i].store(0.0f, std::memory_order_relaxed);
  recentWritten_.store(0, std::memory_order_relaxed);

  if (output_ != HostOutput::Manual) {
    device_ = platform::CreateOutputDevice(
        output_ == HostOutput::Device ? platform::OutputBackend::Default : platform::OutputBackend::SilentTimer, a,
        nullptr);
    if (device_ == nullptr) return fail(SND_ERR_NO_DEVICE, "no output device");
    sys_->SetOutputLatency(device_->OutputLatencySeconds());
    platform::AudioDriverDesc ad;
    ad.core.blockFrames = blockFrames_;
    ad.core.renderAheadBlocks = config_.render_ahead_blocks;
    ad.core.catchupBlocks = config_.tuning.underrun_catchup_blocks;
    ad.core.loadTargetPercent = static_cast<double>(config_.tuning.load_target_percent);
    ad.core.lod.downBlocks = config_.tuning.lod_down_blocks;
    ad.core.lod.upBlocks = config_.tuning.lod_up_blocks;
    ad.core.render = &RenderThunk;
    ad.core.user = this;
    ad.realtimeThread = false;
    const bool silent = output_ == HostOutput::Silent;
    const DeviceDesc dd{config_.sample_rate, channels_, silent ? blockFrames_ : 0u, 0u};
    if (!driver_.Start(*device_, dd, ad, a)) return fail(SND_ERR_NO_DEVICE, "the output device does not start");
    driverStarted_ = true;
    sys_->SetOutputLatency(device_->OutputLatencySeconds());
  }
  return SND_OK;
}

snd_result RuntimeHost::StartServer(ll::ServerDesc sd) {
  if (sys_ == nullptr || live_ == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (!linkInit_) {
    if (!link_.Init(Alloc(), 1u << 20)) return SND_ERR_OUT_OF_BUDGET;
    linkInit_ = true;
  }
  sd.system = sys_;
  sd.runtime = live_;
  sd.allocator = &Alloc();
  sd.acceptor = &link_.acceptor();
  snd_result r = static_cast<snd_result>(ll::Server::Create(sd, &server_));
  if (r == SND_OK) r = static_cast<snd_result>(server_->Start());
  return r;
}

void RuntimeHost::Destroy() {
  if (server_ != nullptr) {
    ll::Server::Destroy(server_);
    server_ = nullptr;
  }
  if (driverStarted_) {
    driver_.Stop();
    driverStarted_ = false;
  }
  if (ioStarted_) {
    io_.Shutdown();
    ioStarted_ = false;
  }
  if (workersStarted_) {
    workers_.Shutdown();
    workersStarted_ = false;
  }
  if (sys_ != nullptr) {
    core::System::Destroy(sys_);
    sys_ = nullptr;
  }
  if (live_ != nullptr) {
    ll::LiveRuntime::Destroy(live_);
    live_ = nullptr;
  }
  replayer_ = nullptr;
  kinds_.Destroy();
  if (ioInit_) {
    io_.Destroy();
    ioInit_ = false;
  }
  streamPool_.Destroy();
  if (device_ != nullptr) {
    platform::DestroyOutputDevice(device_, Alloc());
    device_ = nullptr;
  }
  if (linkInit_) {
    link_.Destroy();
    linkInit_ = false;
  }
}

void RuntimeHost::Keep(const float* block) {
  if (recent_ == nullptr) return;
  const std::size_t n = std::size_t{blockFrames_} * channels_;
  const std::uint64_t w = recentWritten_.load(std::memory_order_relaxed);
  std::atomic<float>* dst = recent_.get() + static_cast<std::size_t>(w % kRecentBlocks) * n;
  for (std::size_t i = 0; i < n; ++i) dst[i].store(block[i], std::memory_order_relaxed);
  recentWritten_.store(w + 1u, std::memory_order_release);
}

std::vector<float> RuntimeHost::RecentOutput(std::uint32_t blocks) const {
  std::vector<float> out;
  if (recent_ == nullptr) return out;
  if (blocks > kRecentBlocks / 2u) blocks = kRecentBlocks / 2u;
  const std::size_t n = std::size_t{blockFrames_} * channels_;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const std::uint64_t w = recentWritten_.load(std::memory_order_acquire);
    const std::uint64_t have = w < blocks ? w : blocks;
    out.assign(static_cast<std::size_t>(have) * n, 0.0f);
    for (std::uint64_t k = 0; k < have; ++k) {
      const std::atomic<float>* src = recent_.get() + static_cast<std::size_t>((w - have + k) % kRecentBlocks) * n;
      for (std::size_t i = 0; i < n; ++i) out[static_cast<std::size_t>(k) * n + i] = src[i].load(std::memory_order_relaxed);
    }
    // The writer may have lapped the copied blocks (kRecentBlocks / 2 of slack): try again.
    if (recentWritten_.load(std::memory_order_acquire) - w < kRecentBlocks / 2u) return out;
  }
  return out;
}

void RuntimeHost::RenderThunk(void* user, float* interleaved, std::uint32_t frames, std::uint32_t channels,
                              const platform::RenderInfo* info) noexcept {
  RuntimeHost& h = *static_cast<RuntimeHost*>(user);
  if (info != nullptr) h.lostPending_ += info->lostFrames;
  for (std::uint32_t k = 0; k < h.config_.tuning.underrun_catchup_blocks && h.lostPending_ >= h.blockFrames_; ++k) {
    h.sys_->RenderBlock(nullptr);
    h.lostPending_ -= h.blockFrames_;
  }
  if (channels == h.channels_ && frames == h.blockFrames_) {
    h.sys_->RenderBlock(interleaved);
    h.Keep(interleaved);
  } else {
    // The device's layout differs: render whole blocks and copy the common channels (D-95's
    // bridge, reduced: the preview opens the device with the core's channels).
    std::uint32_t done = 0;
    while (done < frames) {
      std::uint32_t n = frames - done;
      if (n > h.blockFrames_) n = h.blockFrames_;
      h.sys_->Render(h.mix_.data(), n);
      for (std::uint32_t f = 0; f < n; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
          interleaved[std::size_t{done + f} * channels + c] = c < h.channels_ ? h.mix_[std::size_t{f} * h.channels_ + c] : 0.0f;
        }
      }
      done += n;
    }
  }
  h.blocks_.fetch_add(1u, std::memory_order_acq_rel);
}

void RuntimeHost::Step(float* out) {
  if (sys_ == nullptr) return;
  sys_->Update();
  float* dst = out != nullptr ? out : mix_.data();
  sys_->RenderBlock(dst);
  Keep(dst);
  blocks_.fetch_add(1u, std::memory_order_acq_rel);
}

void RuntimeHost::Pump() {
  if (sys_ != nullptr) sys_->Update();
}

std::uint32_t RuntimeHost::Underruns() const {
  return driverStarted_ ? static_cast<std::uint32_t>(driver_.Stats().core.underruns) : 0u;
}

snd_bank_handle RuntimeHost::LoadBank(const std::string& path, std::uint32_t timeoutMs, std::string* error) {
  snd_bank_handle h{0};
  if (sys_ == nullptr) return h;
  h = sys_->LoadBankAsync(path.c_str());
  if (h.value == 0u) {
    if (error != nullptr) *error = "no bank slot for " + path;
    return h;
  }
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    const snd_bank_state st = sys_->GetBankState(h);
    if (st == SND_BANK_STATE_LOADED) break;
    if (st == SND_BANK_STATE_FAILED || st == SND_BANK_STATE_UNLOADED) {
      if (error != nullptr) *error = "the bank " + path + " did not load";
      return snd_bank_handle{0};
    }
    if (std::chrono::steady_clock::now() > end) {
      if (error != nullptr) *error = "loading " + path + " timed out";
      return snd_bank_handle{0};
    }
    if (output_ == HostOutput::Manual) {
      Step(nullptr);
    } else {
      Pump();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  sys_->Update();
  return h;
}

bool RuntimeHost::UnloadBank(snd_bank_handle h, std::uint32_t timeoutMs) {
  if (sys_ == nullptr || h.value == 0u) return false;
  if (sys_->UnloadBank(h, SND_UNLOAD_STOP_AND_UNLOAD) != SND_OK) return false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (sys_->GetBankState(h) != SND_BANK_STATE_UNLOADED) {
    if (std::chrono::steady_clock::now() > end) return false;
    if (output_ == HostOutput::Manual) {
      Step(nullptr);
    } else {
      Pump();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

}  // namespace snd::tool::preview
