// PCM cache (pcm_cache.hpp).
#include "pcm_cache.hpp"

#include <new>
#include <thread>

namespace snd::source::detail {

namespace {
constexpr std::uint32_t kMaxEntries = 64;

std::uint32_t EntryCap(const SourceLimits& l, std::uint32_t pages) noexcept {
  std::uint32_t n = pages < kMaxEntries ? pages : kMaxEntries;
  if (l.maxDecoders == 0u) n = 0;
  return n;
}
std::uint32_t PageCount(const SourceLimits& l) noexcept {
  if (l.pcmCachePageBytes < 4u * kMaxChannels || l.pcmCacheBytes < l.pcmCachePageBytes) return 0;
  const std::uint64_t n = l.pcmCacheBytes / l.pcmCachePageBytes;
  return n > 0xFFFFu ? 0xFFFFu : static_cast<std::uint32_t>(n);
}

class PageSink final : public OutputSink {
 public:
  PageSink(std::uint8_t* base, std::uint32_t pageBytes, const CacheEntry& e, std::uint64_t pos) noexcept
      : base_(base), pageBytes_(pageBytes), e_(e), pos_(pos), fpp_(pageBytes / (4u * e.channels)) {}
  std::uint32_t Room() const noexcept override {
    const std::uint64_t left = e_.frames - pos_;
    return left > 0x40000000u ? 0x40000000u : static_cast<std::uint32_t>(left);
  }
  void Put(const float* const* src, std::uint32_t offset, std::uint32_t n) noexcept override {
    const std::uint32_t ch = e_.channels;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint64_t f = pos_ + i;
      float* dst = reinterpret_cast<float*>(base_ + std::size_t{e_.pages[f / fpp_]} * pageBytes_) + (f % fpp_) * ch;
      for (std::uint32_t c = 0; c < ch; ++c) dst[c] = src[c][offset + i];
    }
    pos_ += n;
  }
  std::uint64_t Pos() const noexcept { return pos_; }

 private:
  std::uint8_t* base_;
  std::uint32_t pageBytes_;
  const CacheEntry& e_;
  std::uint64_t pos_;
  std::uint32_t fpp_;
};
}  // namespace

void PcmCache::Measure(Carver& c, const SourceLimits& l) noexcept {
  const std::uint32_t pages = PageCount(l);
  const std::uint32_t ents = EntryCap(l, pages);
  if (pages == 0u || ents == 0u) return;
  c.Take<std::uint8_t>(std::size_t{pages} * l.pcmCachePageBytes);
  c.Take<std::uint32_t>(pages);
  c.Take<CacheEntry>(ents);
  c.Take<std::uint32_t>(std::size_t{ents} * pages);
  MeasureCarry(c, ents);
}

bool PcmCache::Bind(Carver& c, const SourceLimits& l) noexcept {
  const std::uint32_t pages = PageCount(l);
  const std::uint32_t ents = EntryCap(l, pages);
  if (pages == 0u || ents == 0u) return true;  // no cache: DecompressCache plays through the ring
  pageBase_ = c.Take<std::uint8_t>(std::size_t{pages} * l.pcmCachePageBytes);
  std::uint32_t* fs = c.Take<std::uint32_t>(pages);
  entries_ = c.Take<CacheEntry>(ents);
  pageLists_ = c.Take<std::uint32_t>(std::size_t{ents} * pages);
  carry_ = c.Take<float>(std::size_t{ents} * kMaxChannels * kCarryFrames);
  if (pageBase_ == nullptr || fs == nullptr || entries_ == nullptr || pageLists_ == nullptr || carry_ == nullptr) return false;
  pageBytes_ = l.pcmCachePageBytes;
  pageCount_ = pages;
  entryCap_ = ents;
  freePages_.Bind(fs, pages);
  for (std::uint32_t i = 0; i < ents; ++i) {
    CacheEntry* e = new (&entries_[i]) CacheEntry();
    e->state = 0;
    e->pages = pageLists_ + std::size_t{i} * pages;
    e->cache = this;
    e->jobCounter = 0;
    e->cancel = 0;
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
      e->dec.carry[ch] = carry_ + (std::size_t{i} * kMaxChannels + ch) * kCarryFrames;
    }
  }
  return true;
}

std::uint32_t PcmCache::EntriesInUse() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < entryCap_; ++i) n += entries_[i].state != 0u ? 1u : 0u;
  return n;
}

void PcmCache::Free(std::uint32_t i) noexcept {
  CacheEntry& e = entries_[i];
  for (std::uint32_t p = e.pageCount; p > 0u; --p) freePages_.Push(e.pages[p - 1u]);
  e.pageCount = 0;
  e.state = 0;
  e.pins = 0;
  e.filled.store(0, std::memory_order_relaxed);
  e.error.store(0, std::memory_order_relaxed);
}

bool PcmCache::Evict(std::uint32_t need, Shared& sh) noexcept {
  while (freePages_.size < need) {
    std::uint32_t best = kNone;
    for (std::uint32_t i = 0; i < entryCap_; ++i) {
      const CacheEntry& e = entries_[i];
      if (e.state < 2u || e.pins != 0u || JobBusy(e.jobCounter)) continue;
      if (best == kNone || e.lastUse < entries_[best].lastUse) best = i;
    }
    if (best == kNone) return false;
    Free(best);
    Count(sh, kCacheEvictions);
  }
  return true;
}

CacheResult PcmCache::Acquire(const AssetCopy& a, ICodec* codec, Shared& sh, std::uint64_t now,
                              std::uint32_t* entry) noexcept {
  if (!Enabled() || a.h.channels == 0u || a.h.channels > kMaxChannels) return CacheResult::Full;
  for (std::uint32_t i = 0; i < entryCap_; ++i) {
    CacheEntry& e = entries_[i];
    if (e.state == 0u || e.state == 3u) continue;
    if (e.assetId == a.assetId && e.bankSlot == a.bankSlot && e.data == a.data && e.frames == a.h.total_frames &&
        e.codecId == a.h.codec) {
      ++e.pins;
      e.lastUse = now;
      *entry = i;
      Count(sh, kCacheHits);
      return CacheResult::Hit;
    }
  }
  Count(sh, kCacheMisses);
  if (codec == nullptr || a.data == nullptr) return CacheResult::Unsupported;
  const std::uint32_t fpp = FramesPerPage(a.h.channels);
  const std::uint64_t pagesNeeded = (a.h.total_frames + fpp - 1u) / fpp;
  if (pagesNeeded == 0u || pagesNeeded > pageCount_) return CacheResult::Full;
  std::uint32_t slotIdx = kNone;
  for (std::uint32_t i = 0; i < entryCap_ && slotIdx == kNone; ++i) {
    if (entries_[i].state == 0u && !JobBusy(entries_[i].jobCounter)) slotIdx = i;
  }
  if (slotIdx == kNone) {  // reuse the least recently used unpinned entry
    std::uint32_t best = kNone;
    for (std::uint32_t i = 0; i < entryCap_; ++i) {
      const CacheEntry& e = entries_[i];
      if (e.state < 2u || e.pins != 0u || JobBusy(e.jobCounter)) continue;
      if (best == kNone || e.lastUse < entries_[best].lastUse) best = i;
    }
    if (best == kNone) return CacheResult::Full;
    Free(best);
    Count(sh, kCacheEvictions);
    slotIdx = best;
  }
  if (!Evict(static_cast<std::uint32_t>(pagesNeeded), sh)) return CacheResult::Full;
  const std::uint32_t stateBytes = codec->StateBytes(a.h);
  codec::DecoderSlot ds;
  if (!sh.decodersReady || sh.decoders.Acquire(stateBytes, &ds) != codec::DecoderPoolStatus::Ok) {
    return CacheResult::NoDecoder;
  }
  CacheEntry& e = entries_[slotIdx];
  e.state = 1;
  e.assetId = a.assetId;
  e.bankSlot = a.bankSlot;
  e.codecId = a.h.codec;
  e.data = a.data;
  e.frames = a.h.total_frames;
  e.channels = a.h.channels;
  e.pageCount = static_cast<std::uint32_t>(pagesNeeded);
  for (std::uint32_t p = 0; p < e.pageCount; ++p) e.pages[p] = freePages_.Pop();
  e.pins = 1;
  e.lastUse = now;
  e.filled.store(0, std::memory_order_relaxed);
  e.error.store(0, std::memory_order_relaxed);
  e.a = a;
  e.slot = ds;
  e.inputOff = 0;
  e.dec.codec = codec;
  e.dec.state = ds.state;
  e.dec.asset = &e.a;
  e.dec.loop = LoopRange{};
  e.dec.channels = a.h.channels;
  e.dec.error = 0;
  codec->Init(ds.state, a.h);
  std::uint64_t byte = 0;
  e.dec.SeekTo(0, &byte);
  e.inputOff = byte;
  std::atomic_ref<std::uint32_t>(e.cancel).store(0u, std::memory_order_relaxed);
  *entry = slotIdx;
  return CacheResult::Started;
}

void PcmCache::Unpin(std::uint32_t i) noexcept {
  if (i < entryCap_ && entries_[i].pins > 0u) --entries_[i].pins;
}

void PcmCache::Fill(CacheEntry& e, std::uint64_t upTo) noexcept {
  ResidentInput in(e.a, e.inputOff);
  PageSink sink(e.cache->pageBase_, e.cache->pageBytes_, e, e.filled.load(std::memory_order_relaxed));
  // Publish progress every 16 Ki frames so voices can start before the fill ends.
  while (!e.dec.ended && e.dec.error == 0u && sink.Room() != 0u && sink.Pos() < upTo) {
    if (std::atomic_ref<std::uint32_t>(e.cancel).load(std::memory_order_acquire) != 0u) return;
    struct Chunk final : OutputSink {
      PageSink& s;
      std::uint32_t left;
      Chunk(PageSink& p, std::uint32_t n) noexcept : s(p), left(n) {}
      std::uint32_t Room() const noexcept override { return s.Room() < left ? s.Room() : left; }
      void Put(const float* const* src, std::uint32_t off, std::uint32_t n) noexcept override {
        s.Put(src, off, n);
        left -= n;
      }
    };
    const std::uint64_t want = upTo - sink.Pos();
    Chunk chunk(sink, want < 16384u ? static_cast<std::uint32_t>(want) : 16384u);
    const std::uint32_t before = chunk.left;
    const std::uint64_t got = e.dec.Run(in, chunk);
    e.filled.store(sink.Pos(), std::memory_order_release);
    if (got == 0u && chunk.left == before && !e.dec.ended && e.dec.error == 0u) break;  // no progress
  }
  if (e.dec.error != 0u) e.error.store(e.dec.error, std::memory_order_release);
}

void PcmCache::FillHead(std::uint32_t i, std::uint64_t frames) noexcept {
  if (i < entryCap_ && entries_[i].state == 1u && !JobBusy(entries_[i].jobCounter)) Fill(entries_[i], frames);
}

void SND_CALL PcmCache::FillJob(void* data) {
  CacheEntry& e = *static_cast<CacheEntry*>(data);
  if (std::atomic_ref<std::uint32_t>(e.cancel).load(std::memory_order_acquire) != 0u) return;
  Fill(e, ~std::uint64_t{0});
}

void PcmCache::Schedule(const SourceServices& sv, Shared& sh) noexcept {
  for (std::uint32_t i = 0; i < entryCap_; ++i) {
    CacheEntry& e = entries_[i];
    if (e.state != 1u || JobBusy(e.jobCounter)) continue;
    const std::uint64_t filled = e.filled.load(std::memory_order_acquire);
    const std::uint32_t err = e.error.load(std::memory_order_acquire);
    if (filled >= e.frames || err != 0u || e.dec.ended) {
      if (err == 0u && filled < e.frames) e.error.store(SND_END_DETAIL_DECODE, std::memory_order_release);
      sh.decoders.Release(e.slot);
      e.slot = codec::DecoderSlot{};
      e.state = e.error.load(std::memory_order_relaxed) != 0u ? 3u : 2u;
      if (e.state == 3u) Count(sh, kDecodeErrors);
      continue;
    }
    if (e.pins == 0u) {
      // Unfinished and no voice plays it any more: the bank may go away (a job
      // posted now would not be covered by core's job epoch). Drop the entry.
      sh.decoders.Release(e.slot);
      e.slot = codec::DecoderSlot{};
      Free(i);
      continue;
    }
    if (!PostJob(sv, sh, &PcmCache::FillJob, &e, &e.jobCounter, &e.cancel, 0u) && sv.postJob == nullptr) {
      FillJob(&e);  // no job service at all: fill on this thread (tests without core services)
    }
  }
  for (std::uint32_t i = 0; i < entryCap_; ++i) {  // failed and unused: give the pages back
    CacheEntry& e = entries_[i];
    if (e.state == 3u && e.pins == 0u && !JobBusy(e.jobCounter)) Free(i);
  }
}

void PcmCache::Shutdown(Shared& sh) noexcept {
  for (std::uint32_t i = 0; i < entryCap_; ++i) {
    std::atomic_ref<std::uint32_t>(entries_[i].cancel).store(1u, std::memory_order_release);
  }
  for (std::uint32_t i = 0; i < entryCap_; ++i) {
    while (JobBusy(entries_[i].jobCounter)) std::this_thread::yield();
    if (entries_[i].state == 1u && sh.decodersReady) sh.decoders.Release(entries_[i].slot);
    entries_[i].state = 0;
    entries_[i].~CacheEntry();
  }
  entryCap_ = 0;
  pageCount_ = 0;
}

void PcmCache::Read(std::uint32_t i, std::uint64_t pos, std::uint32_t n, float* const* out, std::uint32_t offset,
                    std::uint32_t channels) const noexcept {
  const CacheEntry& e = entries_[i];
  const std::uint32_t ch = e.channels;
  const std::uint32_t fpp = FramesPerPage(ch);
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint64_t f = pos + done;
    const std::uint32_t inPage = static_cast<std::uint32_t>(f % fpp);
    std::uint32_t k = fpp - inPage;
    if (k > n - done) k = n - done;
    const float* src = reinterpret_cast<const float*>(pageBase_ + std::size_t{e.pages[f / fpp]} * pageBytes_) +
                       std::size_t{inPage} * ch;
    for (std::uint32_t j = 0; j < k; ++j) {
      for (std::uint32_t c = 0; c < channels; ++c) out[c][offset + done + j] = src[std::size_t{j} * ch + c];
    }
    done += k;
  }
}

}  // namespace snd::source::detail
