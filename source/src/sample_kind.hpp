// Sample kind (design.md 5, 6.2): resident assets of a bank.
//
// Modes per instance (chosen at Start from ASST.loadForm and the codec):
//   Direct  PcmResident PCMF / PC16: RenderBatch reads the bank memory.
//   Ring    CompressedResident (or a resident non-PCM codec): a decode context
//           (decoder pool slot + per-voice ring of voiceRingFrames per channel)
//           filled by prefetch-lane jobs; the first frames are decoded on the
//           audio thread at Start (bounded per call) so the start is not delayed.
//   Cache   DecompressCache: the whole asset decoded once into the PCM cache
//           (pcm_cache.hpp); voices read the pages as they fill.
#ifndef SND_SOURCE_SAMPLE_KIND_HPP
#define SND_SOURCE_SAMPLE_KIND_HPP

#include "detail.hpp"
#include "pcm_cache.hpp"

namespace snd::source::detail {

class SampleKind;

enum class SampleMode : std::uint8_t { Direct = 0, Ring = 1, Cache = 2 };

// Decode context of a Ring-mode voice (count = maxDecoders).
struct RingCtx {
  Decoder dec;
  codec::DecoderSlot slot;
  std::uint32_t rings[kMaxChannels];
  float* ring[kMaxChannels];
  std::uint32_t channels;
  std::uint64_t inputOff;              // resident data position of the decoder
  std::atomic<std::uint64_t> w{0};     // frames written (job)
  std::atomic<std::uint64_t> r{0};     // frames read (Render)
  std::atomic<std::uint32_t> ended{0}; // decoder reached the end (job)
  std::atomic<std::uint32_t> error{0}; // snd_end_detail (job)
  std::uint32_t jobCounter;            // done counter of the job in flight (atomic_ref)
  std::uint32_t cancel;                // cancel flag of the job (atomic_ref)
  std::uint32_t inst;                  // owning instance, kNone = free
  // Requests from Control to Render (D-43 / source_if.hpp: the render state of
  // a voice is changed only by its RenderBatch). req: 0 none, 1 seek to
  // reqTarget, 2 loop change to reqLoop. Executed at the head of the next
  // RenderBatch once no job of the context runs; Publish posts no job meanwhile.
  std::atomic<std::uint32_t> req{0};
  std::uint64_t reqTarget;
  LoopRange reqLoop;
  std::uint32_t listPos;               // position in the active list
  SampleKind* kind;
};

struct SampleInst {
  std::uint32_t state;  // 0 free, 1 live, 2 closing (waits for its job)
  SampleMode mode;
  std::uint8_t virt;
  std::uint8_t primed;  // render: ring / cache had the ready amount
  std::uint8_t kill;    // end at the next render (virtual return "never")
  AssetCopy a;
  LoopRange loop;
  std::uint32_t channels;
  std::uint32_t ctx;    // RingCtx (Ring)
  std::uint32_t entry;  // CacheEntry (Cache)
  std::uint32_t hasOverride;  // render sets the playhead to `overridePos` first
  std::uint64_t overridePos;
  std::uint64_t renderPos;    // render's last position (for Control on the audio thread)
  std::uint32_t renderLoops;
};

class SampleKind final : public ISourceKind {
 public:
  explicit SampleKind(Shared& shared) noexcept : sh_(shared) {}

  SourceKind Kind() const noexcept override { return SourceKind::Sample; }
  bool RendersAtOutputRate() const noexcept override { return false; }
  std::size_t PoolBytes(const SourceLimits& limits) const noexcept override;
  bool Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept override;
  void Shutdown() noexcept override;
  void Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept override;
  void Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept override;
  void Control(const SourceControlOp* ops, std::uint32_t count) noexcept override;
  void QueryReady(const SlotIndex* sourceSlots, std::uint32_t count, std::uint8_t* ready) const noexcept override;
  void RenderBatch(const snd::source::RenderBatch& batch, VoiceRenderStatus* status) noexcept override;
  void AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept override;
  void ScheduleBackground(const BlockInfo& block) noexcept override;
  std::uint32_t PrimeFrames(const SourceAsset& asset, std::uint64_t startFrame) const noexcept override;

  std::uint32_t LiveInstances() const noexcept { return live_; }
  const PcmCache& Cache() const noexcept { return cache_; }

  static void SND_CALL RingJob(void* data);

 private:
  void Layout(Carver& c, const SourceLimits& l) noexcept;
  bool StartOne(const SourceStartRequest& r, SourceStartResult& res, std::uint32_t& inlineBudget) noexcept;
  bool StartRing(SampleInst& in, std::uint32_t instIndex, std::uint64_t start, std::uint32_t& inlineBudget,
                 std::uint32_t* detail) noexcept;
  void FreeInstance(std::uint32_t i) noexcept;
  void FreeCtx(std::uint32_t c) noexcept;
  void SeekRing(RingCtx& c, std::uint64_t target, std::uint32_t loops, std::uint32_t& inlineBudget) noexcept;
  bool LoopChangeFits(const RingCtx& c, const SampleInst& in, std::uint64_t pos, std::uint32_t loops,
                      const LoopRange& to, std::uint64_t* agree) const noexcept;
  void ExecuteRequest(RingCtx& c, SampleInst& in, const RenderItem& it, std::uint32_t& inlineBudget) noexcept;
  void DecodeRing(RingCtx& c, std::uint64_t limitFrames) noexcept;
  bool Ready(const SampleInst& in) const noexcept;
  void RenderDirect(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st) noexcept;
  void RenderRing(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st, std::uint32_t& inlineBudget) noexcept;
  void RenderCache(SampleInst& in, const RenderItem& it, VoiceRenderStatus& st) noexcept;

  Shared& sh_;
  SourceServices sv_{};
  SourceLimits lim_{};
  SampleInst* inst_ = nullptr;
  std::uint32_t instCap_ = 0;
  FreeStack instFree_;
  RingCtx* ctx_ = nullptr;
  std::uint32_t ctxCap_ = 0;
  FreeStack ctxFree_;
  std::uint32_t* active_ = nullptr;  // live RingCtx indices (dense)
  std::uint32_t activeCount_ = 0;
  RingPool rings_;
  PcmCache cache_;
  std::uint32_t readyFrames_ = 0;
  std::uint32_t live_ = 0;
  std::uint64_t block_ = 0;
  bool decoders_ = false;
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_SAMPLE_KIND_HPP
