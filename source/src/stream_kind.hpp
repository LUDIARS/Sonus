// Stream kind (design.md 6.1, 6.3; D-45, A1-15).
//
// Three rings per stream slot: IO -> compressed (io::StreamChannel, chunks of
// the io::StreamPool) -> decode job (prefetch lane) -> PCM ring (one float ring
// per channel, the Stream table's ringWrite / ringRead are its positions) ->
// RenderBatch, which only copies out of the PCM ring.
//
// Audio thread (Start / Release / Control in RunEventLogic, ScheduleBackground
// in Publish): owns the read plan (io::StreamReadPlanner), posts the reads
// (IoScheduler::Post) and cancels them (StreamChannel::Cancel) itself, so a
// Post never races a Cancel of the same slot. It touches the decoder state and
// the channel's consumer side only while no decode job of the slot is in
// flight (the job's done counter is 0): seeks, virtualisation and the release
// of a slot wait for that.
//
// Start: first the stream file header (bank-format.md 6.3, 64 bytes at offset
// 0) is read and checked (magic, version, codec, channels, frames, data
// offset); it gives the exact data length. Meanwhile the preload (PREL) is
// decoded: at Start on the audio thread for the first frames (bounded per
// call), then by the job. IO reads continue at the end of the preload.
//
// Seek / loop (D-23): the seek table (or PacketForFrame) gives the packet at or
// before target - preRoll; the extra frames are dropped. A loop wraps at the
// loop end frame exactly; the planner reads ahead into the loop head
// (wrapped read), the decoder seeks there.
//
// Underrun (6.3): when the PCM ring runs dry the output fades out (5 ms),
// Stream.stallCount is incremented and the voice reports `starved`. It resumes
// with a 5 ms fade in once the ready amount is buffered again. Sound priority
// resumes where it stopped (the playhead waits); time priority keeps the
// playhead moving and drops the frames that elapsed, re-seeking ahead when the
// gap no longer fits the ring.
//
// IO failure (open failed, read error, bad header) and decode errors end the
// voice (status `ended` after what was buffered).
#ifndef SND_SOURCE_STREAM_KIND_HPP
#define SND_SOURCE_STREAM_KIND_HPP

#include "detail.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/io/stream_read_planner.hpp"

namespace snd::source::detail {

class StreamKind;

inline constexpr std::uint32_t kTagWrapped = 1u << 31;
inline constexpr std::uint32_t kTagHeader = 1u << 30;

struct StreamInst {
  std::uint32_t state;  // 0 free, 1 live, 2 closing
  std::uint32_t slot;
  AssetCopy a;
  LoopRange loop;
  std::uint32_t channels;  // rendered channels
  StreamKind* kind;
  char path[io::kMaxIoPathBytes];

  // --- decode side (the job, or the audio thread while no job runs) ---
  Decoder dec;
  codec::DecoderSlot dslot;
  std::uint32_t rings[kMaxChannels];
  float* ring[kMaxChannels];
  io::ChunkRecord cur;
  std::uint32_t curValid;
  std::uint32_t curOff;
  std::uint32_t skipToWrap;
  std::uint32_t preActive;     // preload input
  std::uint64_t preOff;
  std::uint64_t dataBytes;     // from the header
  std::uint32_t eof;           // the data end (or a short read) was consumed
  std::atomic<std::uint32_t> headerDone{0};
  std::atomic<std::uint32_t> ended{0};
  std::atomic<std::uint32_t> error{0};  // snd_end_detail
  std::uint32_t jobCounter;
  std::uint32_t cancel;

  // --- audio thread ---
  io::StreamReadPlanner planner;
  std::uint32_t plannerSet;
  std::uint32_t planEnded;      // non-looping plan reached the data end
  std::uint32_t wrapsPosted;    // wrapped reads posted since the plan was set
  std::uint64_t ioStart;        // data byte where the IO reads start (after the preload)
  std::uint32_t headerPosted;
  std::uint32_t pendingSeek;
  std::uint64_t seekTarget;
  std::uint32_t pendingStop;    // virtualised: drop IO and buffered data
  std::uint32_t hasPendingLoop; // SetLoop waiting for the seek
  LoopRange pendingLoop;
  std::uint32_t virt;
  std::uint32_t seq;
  std::uint32_t errorCounted;

  // --- render (RT worker / audio thread) ---
  std::uint8_t primed;
  std::uint8_t stalled;
  std::uint8_t kill;
  std::uint32_t fadeDone;       // frames of the current fade in
  std::uint64_t ringPos;        // material position of the PCM ring's read side
  std::uint32_t ringLoops;
  std::int64_t lag;             // time priority: frames the voice is ahead of the ring
  std::uint32_t hasOverride;
  std::uint64_t overridePos;
  std::uint64_t renderPos;      // playhead after the last render (audio thread reads it)
  std::atomic<std::uint32_t> reseek{0};  // time priority asks for a re-seek to reseekTarget
  std::uint64_t reseekTarget;
  std::int64_t reseekAhead;
};

class StreamKind final : public ISourceKind {
 public:
  explicit StreamKind(Shared& shared) noexcept : sh_(shared) {}

  SourceKind Kind() const noexcept override { return SourceKind::Stream; }
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
  std::uint32_t RingFrames() const noexcept { return rings_.frames; }
  std::uint32_t ReadyFrames() const noexcept { return readyFrames_; }

  static void SND_CALL DecodeJob(void* data);

  // Used by the input source of the decode engine.
  io::StreamPool& Pool() noexcept { return *sh_.streamPool; }
  io::StreamChannel& Channel(std::uint32_t slot) noexcept { return sh_.io->Channel(slot); }
  bool ParseHeader(StreamInst& s, const io::ChunkRecord& rec) noexcept;
  void MirrorReadPos(StreamInst& s) noexcept;

 private:
  void Layout(Carver& c, const SourceLimits& l) noexcept;
  bool StartOne(const SourceStartRequest& r, SourceStartResult& res, std::uint32_t& inlineBudget) noexcept;
  void Decode(StreamInst& s) noexcept;
  void ResetForSeek(StreamInst& s, std::uint64_t target) noexcept;
  void SetupPlanner(StreamInst& s) noexcept;
  void PostReads(StreamInst& s) noexcept;
  bool TryFree(StreamInst& s) noexcept;
  void DropCurrent(StreamInst& s) noexcept;
  bool Ready(const StreamInst& s) const noexcept;
  std::uint64_t RingWrite(const StreamInst& s) const noexcept;
  std::uint64_t RingRead(const StreamInst& s) const noexcept;
  void SetRingRead(StreamInst& s, std::uint64_t v) noexcept;
  void SetRingWrite(StreamInst& s, std::uint64_t v) noexcept;
  void RenderOne(StreamInst& s, const RenderItem& it, VoiceRenderStatus& st) noexcept;

  Shared& sh_;
  SourceServices sv_{};
  SourceLimits lim_{};
  StreamInst* inst_ = nullptr;
  std::uint32_t cap_ = 0;
  FreeStack free_;
  RingPool rings_;
  std::uint32_t readyFrames_ = 0;
  std::uint32_t fadeFrames_ = 0;
  std::uint32_t chunkBytes_ = 0;
  std::uint32_t live_ = 0;
  bool decoders_ = false;
  bool enabled_ = false;
};

}  // namespace snd::source::detail

#endif  // SND_SOURCE_STREAM_KIND_HPP
