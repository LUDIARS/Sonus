// PCM cache for the DecompressCache load form (design.md 6.2, 3.3).
//
// Config.memory.pcmCacheBytes cut into pages of pcmCachePageBytes. An entry
// holds one asset decoded to interleaved float32 (an integer number of frames
// per page). The first play of an asset takes free pages (evicting the least
// recently used unpinned entries) and a decoder slot, and a prefetch-lane job
// decodes the whole asset into the pages; voices play the frames already
// filled (Filled() grows while the job runs). Entries are pinned while a voice
// plays them. When every page is pinned the caller falls back to the per-voice
// ring (6.2), which is refused when no decoder slot is free either.
//
// Key: (assetId, bank slot, data pointer, frames, codec). The cached PCM is a
// copy, so an entry stays valid after its bank is unloaded; a different bank
// loaded at the same slot and address with the same asset id and length would
// match too (documented limitation; the asset id names the same asset).
//
// Threads: everything on the audio thread except the fill job (prefetch lane)
// and Read / Filled (RenderBatch: RT workers or the audio thread).
#ifndef SND_SOURCE_PCM_CACHE_HPP
#define SND_SOURCE_PCM_CACHE_HPP

#include "detail.hpp"

namespace snd::source::detail {

class PcmCache;

struct CacheEntry {
  std::uint32_t state;  // 0 free, 1 filling, 2 ready, 3 failed
  Id assetId;
  std::uint16_t bankSlot;
  std::uint32_t codecId;
  const std::uint8_t* data;
  std::uint64_t frames;
  std::uint32_t channels;
  std::uint32_t pageCount;
  std::uint32_t* pages;               // pageCount page indices (capacity: all pages)
  std::uint32_t pins;
  std::uint64_t lastUse;
  std::atomic<std::uint64_t> filled{0};  // frames decoded (job, release)
  std::atomic<std::uint32_t> error{0};
  // fill job
  Decoder dec;
  codec::DecoderSlot slot;
  AssetCopy a;
  std::uint64_t inputOff;
  std::uint32_t jobCounter;
  std::uint32_t cancel;
  PcmCache* cache;
};

enum class CacheResult : std::uint8_t { Hit, Started, Full, NoDecoder, Unsupported };

class PcmCache {
 public:
  static void Measure(Carver& c, const SourceLimits& l) noexcept;
  bool Bind(Carver& c, const SourceLimits& l) noexcept;
  bool Enabled() const noexcept { return pageCount_ != 0u && entryCap_ != 0u; }

  // Audio thread. Pins the entry of `a` (creating it if needed). Hit / Started
  // set *entry. Full: no pages even after eviction. NoDecoder: no decoder slot.
  CacheResult Acquire(const AssetCopy& a, ICodec* codec, Shared& sh, std::uint64_t now, std::uint32_t* entry) noexcept;
  void Unpin(std::uint32_t entry) noexcept;
  // Audio thread (Publish): posts fill jobs, finishes filled entries.
  void Schedule(const SourceServices& sv, Shared& sh) noexcept;
  // Owner thread: cancels and waits for fill jobs, returns decoder slots.
  void Shutdown(Shared& sh) noexcept;

  // Any thread (RenderBatch).
  std::uint64_t Filled(std::uint32_t e) const noexcept {
    return entries_[e].filled.load(std::memory_order_acquire);
  }
  std::uint32_t Error(std::uint32_t e) const noexcept { return entries_[e].error.load(std::memory_order_acquire); }
  bool Finished(std::uint32_t e) const noexcept { return entries_[e].state >= 2u; }
  std::uint64_t Frames(std::uint32_t e) const noexcept { return entries_[e].frames; }
  // Copies n frames (< Filled) starting at `pos` into out[c][offset..].
  void Read(std::uint32_t e, std::uint64_t pos, std::uint32_t n, float* const* out, std::uint32_t offset,
            std::uint32_t channels) const noexcept;

  std::uint32_t EntriesInUse() const noexcept;
  std::uint32_t PagesUsed() const noexcept { return pageCount_ - freePages_.size; }
  std::uint32_t Pages() const noexcept { return pageCount_; }

  // Audio thread, right after Started (no job yet): decodes up to `frames` frames.
  void FillHead(std::uint32_t entry, std::uint64_t frames) noexcept;
  static void SND_CALL FillJob(void* data);
  static void Fill(CacheEntry& e, std::uint64_t upTo) noexcept;

 private:
  std::uint32_t FramesPerPage(std::uint32_t channels) const noexcept { return pageBytes_ / (4u * channels); }
  void Free(std::uint32_t e) noexcept;
  bool Evict(std::uint32_t pagesNeeded, Shared& sh) noexcept;

  std::uint8_t* pageBase_ = nullptr;
  std::uint32_t pageBytes_ = 0;
  std::uint32_t pageCount_ = 0;
  FreeStack freePages_;
  CacheEntry* entries_ = nullptr;
  std::uint32_t entryCap_ = 0;
  std::uint32_t* pageLists_ = nullptr;
  float* carry_ = nullptr;
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_PCM_CACHE_HPP
