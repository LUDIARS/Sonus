// Callback kind (design.md 5.2; source_if.hpp CallbackParams; CONTRACTS.md D-91).
//
// A synth voice (snd_play_synth) calls the game's callback. It renders at the
// internal rate and ignores the voice pitch (the callback generates what it
// wants; RendersAtOutputRate() is true).
//
// Modes (snd_synth_desc.mode):
//   Buffered (default)  a normal-worker job renders ahead into a per-voice ring
//                       (ahead_blocks blocks, 0 = Config.tuning.callbackAheadBlocks);
//                       RenderBatch only copies from the ring. The ring is
//                       consumed in lockstep with the voice's playhead: frame f of
//                       the voice is heard f frames after its start (pauses and
//                       virtual time excluded). Frames the job did not deliver in
//                       time are silence and are dropped (the job continues from
//                       the consumption point); after such an underrun the voice
//                       stays silent until the ring holds ahead_blocks again
//                       ("締切の判定は事後"). The first block of a voice is always
//                       silent: the job cannot run before it.
//   LowLatency          RenderBatch calls the callback synchronously (RT worker or
//                       audio thread) for exactly the frames of the block. A call
//                       that takes longer than one block's duration mutes the voice
//                       from the next block and demotes it to Buffered (when a
//                       ring is free; otherwise it stays muted).
// Rings: min(maxRealVoices, kMaxRings) of NextPow2((ahead + 2) x blockFrames)
// frames x SND_MAX_CHANNELS. A Buffered start without a free ring fails
// (SND_END_DETAIL_VOICE_LIMIT).
//
// snd_render_context: clock_sample = the common-clock sample of the first frame
// (LowLatency: exact; Buffered: start clock + frames, extrapolated from the
// last rendered block), playback = the voice's playback, param_* = none (0, a
// synth has no parameter list in contract 1.2), sample_rate / block_frames of
// the system, flags LOW_LATENCY / FIRST_BLOCK, transport_beats from
// RenderItem::transportBeats (D-90): LowLatency exact at the first frame,
// Buffered extrapolated from the last rendered block at its tempo.
// cpp_callback (C++ wrapper) is called with the same context as snd::RenderContext.
//
// Threads: Start / Release / Control / AdvanceVirtual / ScheduleBackground on
// the audio thread, RenderBatch on RT workers or the audio thread, the
// render-ahead job on a normal worker. Per ring: the job is the only writer of
// `written`, RenderBatch the only writer of `consumed`.
#ifndef SND_SOURCE_CALLBACK_KIND_HPP
#define SND_SOURCE_CALLBACK_KIND_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/internal/source_if.hpp"

namespace snd::source::detail {

class CallbackKind;

struct CallbackRing {
  float* data = nullptr;                    // [kMaxChannels][frames]
  std::atomic<std::uint64_t> written{0};    // voice frames written (job)
  std::atomic<std::uint64_t> consumed{0};   // voice frames consumed (RenderBatch)
  std::atomic<std::uint32_t> jobBusy{0};    // 1 while a job is posted or running
  std::uint32_t done = 0;                   // JobDesc done counter
  std::uint32_t cancel = 0;                 // JobDesc cancel flag (Shutdown)
  std::uint32_t inst = 0;                   // owning instance (kInvalidSlot: free)
  CallbackKind* kind = nullptr;
};

struct CallbackInst {
  std::uint32_t state = 0;  // 0 free, 1 live, 2 closing (waits for its ring's job)
  snd_synth_desc desc{};
  HandleRef playback{};
  std::uint32_t channels = 0;
  std::uint32_t aheadFrames = 0;
  std::uint32_t ring = kInvalidSlot;
  // RenderBatch -> job / ScheduleBackground (relaxed fields behind `seq`, a seqlock).
  std::atomic<std::uint32_t> seq{0};
  std::atomic<std::uint64_t> refPos{0};     // voice frame of the last rendered block's first frame
  std::atomic<std::uint64_t> refClock{0};   // its common clock
  std::atomic<double> refBeats{-1.0};       // transport_beats there
  std::atomic<double> refBps{0.0};          // quarter notes per sample
  std::atomic<std::uint32_t> lowLatency{0}; // 1 = LowLatency mode (RenderBatch reads, ScheduleBackground demotes)
  std::atomic<std::uint32_t> demote{0};     // RenderBatch: the deadline was missed
  std::atomic<std::uint32_t> muted{0};      // demoted without a ring: silence
  std::atomic<std::uint32_t> called{0};     // the callback ran once (FIRST_BLOCK); written by its single caller
  // Render state (RenderBatch only).
  std::uint32_t waiting = 1;  // Buffered: silent until the ring holds aheadFrames
  std::uint32_t played = 0;   // Buffered: delivered frames once (an empty ring is an underrun)
};

struct CallbackStats {
  std::uint32_t instances = 0;
  std::uint64_t underruns = 0;  // Buffered blocks with missing frames after the voice played
  std::uint64_t demotions = 0;  // LowLatency calls over the deadline
  std::uint64_t rejected = 0;   // Buffered starts without a free ring
  std::uint64_t calls = 0;      // callback invocations
};

class CallbackKind final : public ISourceKind {
 public:
  static constexpr std::uint32_t kMaxRings = 32;

  CallbackKind() noexcept = default;

  SourceKind Kind() const noexcept override { return SourceKind::Callback; }
  bool RendersAtOutputRate() const noexcept override { return true; }
  std::size_t PoolBytes(const SourceLimits& limits) const noexcept override;
  bool Init(void* pool, std::size_t bytes, const SourceLimits& limits, const SourceServices& services) noexcept override;
  void Shutdown() noexcept override;
  void Start(const SourceStartRequest* requests, std::uint32_t count, SourceStartResult* results) noexcept override;
  void Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept override;
  void Control(const SourceControlOp* ops, std::uint32_t count) noexcept override;
  void QueryReady(const SlotIndex* sourceSlots, std::uint32_t count, std::uint8_t* ready) const noexcept override;
  void RenderBatch(const source::RenderBatch& batch, VoiceRenderStatus* status) noexcept override;
  void AdvanceVirtual(const AdvanceItem* items, std::uint32_t count, VoiceRenderStatus* status) noexcept override;
  void ScheduleBackground(const BlockInfo& block) noexcept override;
  std::uint32_t PrimeFrames(const SourceAsset&, std::uint64_t) const noexcept override { return 0; }

  CallbackStats Stats() const noexcept;

 private:
  static void JobThunk(void* data) noexcept;
  void RunJob(CallbackRing& ring) noexcept;
  void Call(CallbackInst& in, float* const* out, std::uint32_t channels, std::uint32_t frames, std::uint64_t clock,
            double beats, std::uint32_t flags) noexcept;
  std::uint32_t AcquireRing(std::uint32_t inst) noexcept;
  void FreeInst(std::uint32_t inst) noexcept;
  float* RingChannel(const CallbackRing& r, std::uint32_t c) const noexcept {
    return r.data + std::size_t{c} * ringFrames_;
  }

  SourceServices sv_{};
  CallbackInst* inst_ = nullptr;
  CallbackRing* rings_ = nullptr;
  std::uint32_t* freeInst_ = nullptr;
  std::uint32_t freeCount_ = 0;
  std::uint32_t instCap_ = 0;
  std::uint32_t ringCap_ = 0;
  std::uint32_t ringFrames_ = 0;
  std::uint32_t defaultAhead_ = 4;
  std::uint32_t blockFrames_ = 256;
  std::uint32_t sampleRate_ = 48000;
  std::uint64_t deadlineNs_ = 0;
  std::atomic<std::uint32_t> live_{0};
  std::atomic<std::uint64_t> underruns_{0}, demotions_{0}, rejected_{0}, calls_{0};
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_CALLBACK_KIND_HPP
