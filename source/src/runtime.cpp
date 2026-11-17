// snd::source::Runtime (runtime.hpp): the source kinds and their shared state.
#include "snd/source/runtime.hpp"

#include <new>

#include "detail.hpp"
#include "snd/io/io_scheduler.hpp"
#include "sample_kind.hpp"
#include "stream_kind.hpp"

namespace snd::source {

struct Runtime::Impl {
  detail::Shared shared;
  detail::SampleKind sample{shared};
  detail::StreamKind stream{shared};
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
  kinds_ = SourceKindSet{};
  if (desc.enableSample) kinds_.kinds[static_cast<std::size_t>(SourceKind::Sample)] = &impl_->sample;
  if (desc.enableStream && desc.io != nullptr && desc.streamPool != nullptr) {
    kinds_.kinds[static_cast<std::size_t>(SourceKind::Stream)] = &impl_->stream;
  }
  return true;
}

void Runtime::Destroy() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->shared.bankEpochs != nullptr) alloc_->Free(impl_->shared.bankEpochs, MemTag::Misc);
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
  // Its stream files may be rebuilt: close what no read uses (playing streams keep theirs).
  if (rt->impl_->shared.io != nullptr) rt->impl_->shared.io->CloseIdleFiles();
}

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
  return s;
}

}  // namespace snd::source
