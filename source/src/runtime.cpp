// snd::source::Runtime (runtime.hpp): the source kinds and their shared state.
#include "snd/source/runtime.hpp"

#include <new>

#include "detail.hpp"
#include "snd/io/io_scheduler.hpp"
#include "callback_kind.hpp"
#include "input_kind.hpp"
#include "granular_kind.hpp"
#include "patch_kind.hpp"
#include "noise_kind.hpp"
#include "oscillator_kind.hpp"
#include "sample_kind.hpp"
#include "stream_kind.hpp"
#include "user_buffer_kind.hpp"

namespace snd::source {

struct Runtime::Impl {
  detail::Shared shared;
  detail::SampleKind sample{shared};
  detail::StreamKind stream{shared};
  detail::CallbackKind callback;
  detail::UserBufferKind userBuffer;
  detail::OscillatorKind oscillator;
  detail::NoiseKind noise;
  detail::InputKind input;
  detail::GranularKind granular;
  detail::PatchKind patch;
  // Input ports (D-194): the port objects and their sample memory.
  InputPort* ports = nullptr;
  float* portData = nullptr;
  std::uint32_t portCount = 0;
};

bool Runtime::Init(const RuntimeDesc& desc) noexcept {
  Destroy();
  if (desc.allocator == nullptr) return false;
  void* mem = desc.allocator->Allocate(sizeof(Impl), alignof(Impl) < 64 ? 64 : alignof(Impl), MemTag::Misc);
  if (mem == nullptr) return false;
  impl_ = new (mem) Impl();
  alloc_ = desc.allocator;
  impl_->shared.allocator = desc.allocator;
  impl_->shared.io = desc.io;
  impl_->shared.streamPool = desc.streamPool;
  {
    const std::uint32_t n = desc.maxBanks == 0u ? 64u : desc.maxBanks;
    void* m = desc.allocator->Allocate(sizeof(std::atomic<std::uint32_t>) * n, 64, MemTag::Misc);
    if (m == nullptr) {
      Destroy();
      return false;
    }
    impl_->shared.bankEpochs = static_cast<std::atomic<std::uint32_t>*>(m);
    for (std::uint32_t i = 0; i < n; ++i) new (&impl_->shared.bankEpochs[i]) std::atomic<std::uint32_t>(0u);
    impl_->shared.bankEpochCount = n;
  }
  impl_->shared.maxVoiceChannels =
      desc.maxVoiceChannels == 0u || desc.maxVoiceChannels > kMaxChannels ? kMaxChannels : desc.maxVoiceChannels;
  if (desc.enableInput && desc.inputPorts != 0u) {
    const std::uint32_t n = desc.inputPorts;
    const std::uint32_t ch = desc.inputPortChannels == 0u || desc.inputPortChannels > kMaxChannels
                                 ? 2u
                                 : desc.inputPortChannels;
    const std::uint32_t frames = detail::NextPow2(desc.inputPortFrames < 64u ? 64u : desc.inputPortFrames);
    void* pm = desc.allocator->Allocate(sizeof(InputPort) * n, 64, MemTag::Misc);
    void* dm = pm != nullptr ? desc.allocator->Allocate(sizeof(float) * std::size_t{n} * ch * frames, 64, MemTag::Misc)
                             : nullptr;
    if (pm == nullptr || dm == nullptr) {
      if (pm != nullptr) desc.allocator->Free(pm, MemTag::Misc);
      Destroy();
      return false;
    }
    impl_->ports = static_cast<InputPort*>(pm);
    impl_->portData = static_cast<float*>(dm);
    impl_->portCount = n;
    for (std::uint32_t i = 0; i < n; ++i) {
      InputPort* port = new (&impl_->ports[i]) InputPort();
      float* data = impl_->portData + std::size_t{i} * ch * frames;
      for (std::size_t k = 0; k < std::size_t{ch} * frames; ++k) data[k] = 0.0f;
      port->Bind(data, ch, frames);
    }
    impl_->input.BindPorts(impl_->ports, n);
  }
  kinds_ = SourceKindSet{};
  if (desc.enableSample) kinds_.kinds[static_cast<std::size_t>(SourceKind::Sample)] = &impl_->sample;
  if (desc.enableStream && desc.io != nullptr && desc.streamPool != nullptr) {
    kinds_.kinds[static_cast<std::size_t>(SourceKind::Stream)] = &impl_->stream;
  }
  if (desc.enableCallback) kinds_.kinds[static_cast<std::size_t>(SourceKind::Callback)] = &impl_->callback;
  if (desc.enableUserBuffer) kinds_.kinds[static_cast<std::size_t>(SourceKind::UserBuffer)] = &impl_->userBuffer;
  if (desc.enableGranular) kinds_.kinds[static_cast<std::size_t>(SourceKind::Granular)] = &impl_->granular;
  if (desc.enablePatch) kinds_.kinds[static_cast<std::size_t>(SourceKind::Patch)] = &impl_->patch;
  if (desc.enableOscillator) kinds_.kinds[static_cast<std::size_t>(SourceKind::Oscillator)] = &impl_->oscillator;
  if (desc.enableNoise) kinds_.kinds[static_cast<std::size_t>(SourceKind::Noise)] = &impl_->noise;
  if (impl_->portCount != 0u) kinds_.kinds[static_cast<std::size_t>(SourceKind::Input)] = &impl_->input;
  return true;
}

void Runtime::Destroy() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->shared.bankEpochs != nullptr) alloc_->Free(impl_->shared.bankEpochs, MemTag::Misc);
  if (impl_->ports != nullptr) {
    for (std::uint32_t i = 0; i < impl_->portCount; ++i) impl_->ports[i].~InputPort();
    alloc_->Free(impl_->ports, MemTag::Misc);
  }
  if (impl_->portData != nullptr) alloc_->Free(impl_->portData, MemTag::Misc);
  impl_->~Impl();
  alloc_->Free(impl_, MemTag::Misc);
  impl_ = nullptr;
  alloc_ = nullptr;
  kinds_ = SourceKindSet{};
}

void Runtime::BankUnregistered(void* runtime, std::uint16_t bankSlot) noexcept {
  auto* rt = static_cast<Runtime*>(runtime);
  if (rt == nullptr || rt->impl_ == nullptr) return;
  rt->impl_->shared.BankEpoch(bankSlot).fetch_add(1u, std::memory_order_release);
  // Stream files need nothing here: every playback opens its file at its start
  // and closes it at its end (StreamChannel::ReopenFile / CloseFile), so no file
  // of the unregistered bank stays open after its playbacks, and playbacks of
  // other banks keep theirs (never opened again while they play).
}

bool Runtime::CallbackBusy(std::uint64_t handle) const noexcept {
  return impl_ != nullptr && impl_->callback.PlaybackBusy(handle);
}

InputPort* Runtime::InputPortOf(std::uint32_t index) const noexcept {
  return impl_ != nullptr && index < impl_->portCount ? &impl_->ports[index] : nullptr;
}

std::uint32_t Runtime::InputPortCount() const noexcept { return impl_ != nullptr ? impl_->portCount : 0u; }

RuntimeStats Runtime::Stats() const noexcept {
  RuntimeStats s;
  if (impl_ == nullptr) return s;
  const detail::Shared& sh = impl_->shared;
  auto c = [&](detail::Counter k) { return sh.counters[k].load(std::memory_order_relaxed); };
  s.sampleInstances = impl_->sample.LiveInstances();
  s.streamInstances = impl_->stream.LiveInstances();
  if (sh.decodersReady) {
    const codec::DecoderPoolUsage u = sh.decoders.Usage();
    s.decodersInUse = u.inUse;
    s.decoderCapacity = u.capacity;
  }
  s.cacheEntries = impl_->sample.Cache().EntriesInUse();
  s.cachePagesUsed = impl_->sample.Cache().PagesUsed();
  s.cachePages = impl_->sample.Cache().Pages();
  s.cacheHits = c(detail::kCacheHits);
  s.cacheMisses = c(detail::kCacheMisses);
  s.cacheEvictions = c(detail::kCacheEvictions);
  s.cacheFallbacks = c(detail::kCacheFallbacks);
  s.rejectedDecoder = c(detail::kRejectedDecoder);
  s.rejectedStreamSlot = c(detail::kRejectedStreamSlot);
  s.rejectedOther = c(detail::kRejectedOther);
  s.sampleUnderruns = c(detail::kSampleUnderruns);
  s.streamStalls = c(detail::kStreamStalls);
  s.streamStallBlocks = c(detail::kStreamStallBlocks);
  s.streamIoErrors = c(detail::kStreamIoErrors);
  s.decodeErrors = c(detail::kDecodeErrors);
  s.jobsPosted = c(detail::kJobsPosted);
  s.jobsRejected = c(detail::kJobsRejected);
  s.ioPosted = c(detail::kIoPosted);
  s.ioRejected = c(detail::kIoRejected);
  s.inlinePrimes = c(detail::kInlinePrimes);
  const detail::CallbackStats cb = impl_->callback.Stats();
  s.callbackInstances = cb.instances;
  s.callbackCalls = cb.calls;
  s.callbackUnderruns = cb.underruns;
  s.callbackDemotions = cb.demotions;
  s.callbackRejected = cb.rejected;
  s.userBufferInstances = impl_->userBuffer.LiveInstances();
  s.userBufferUnderruns = impl_->userBuffer.Underruns();
  s.oscillatorInstances = impl_->oscillator.LiveInstances();
  s.noiseInstances = impl_->noise.LiveInstances();
  s.inputInstances = impl_->input.LiveInstances();
  s.inputUnderruns = impl_->input.Underruns();
  s.granularInstances = impl_->granular.LiveInstances();
  s.patchInstances = impl_->patch.LiveInstances();
  return s;
}

}  // namespace snd::source
