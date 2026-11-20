// core <-> source internal interface (design.md 5, 5.1, 5.2, 6, 12.2-2).
// FROZEN CONTRACT.
//
// One ISourceKind object per SourceKind. Core never calls a source per voice:
//   - RunEventLogic (audio thread, serial) calls Start / Release / Control /
//     QueryReady with arrays.
//   - Render calls RenderBatch once per (render-list chunk, kind) run, on RT
//     workers or the audio thread, and AdvanceVirtual once per kind for the
//     virtual voices (serial).
//   - Publish calls ScheduleBackground once per kind and block.
// Everything called on the audio thread / RT workers obeys principle 1. The
// only way out to non-RT threads is SourceServices::postJob / postIo, which
// are lock-free and bounded (they return false when full).
//
// Resampling: kinds that read recorded data (Sample, Stream, UserBuffer,
// Input, Granular source reads) produce frames at the SOURCE rate and core
// resamples with dsp (source does not depend on dsp, 12.1). Synthesising kinds
// (Oscillator, Noise, Patch, Callback) render at the internal rate and apply
// pitch themselves. ISourceKind::RendersAtOutputRate() tells which.
// Pitch / rate ratios are clamped to Config.maxPitchRatio (1.1, D-44), the
// value core's resampler pool is sized for.
//
// Late chunks (1.1, D-43; design 2.3). A chunk that misses the deadline is not
// added to the output, but its worker may keep running after Run() returned
// and even into the next block. Therefore RenderBatch (and anything core runs
// inside a chunk) may touch ONLY:
//   - the RenderItem array of its chunk (core copies every per-block input the
//     chunk needs - pitch, gain, filter target, pan, ... - into the items in
//     the serial part; a chunk never reads table columns directly),
//   - the chunk's per-bus accumulators (out buffers),
//   - the calling thread's arena (RenderBatch::scratch),
//   - the render state of the voices of its chunk, addressed by RealVoice slot
//     (RenderItem::realSlot): their RealVoice columns (RealVoice is a Direct
//     table: rows never move; playhead and renderStatus live there) and the
//     pools indexed by the real slot (resampler, filters, source instance).
// No Voice row (Sparse, rows move), no other shared memory, no source-wide
// mutable state that another chunk or the audio thread reads in the same
// block without synchronisation.
// Core keeps two sets of render lists + chunk accumulators and alternates per
// block. A block is rendered in parallel (RtWorkerPool::Run) only when
// RtWorkerPool::IsQuiescent() is true at its start: then every earlier chunk
// has finished and both sets are free. Otherwise the block is rendered
// serially on the audio thread alone into the set the last parallel block did
// not use; at most one set is ever in use by a late chunk, so that set always
// exists. Voices of a late chunk are HELD (schema Voice.renderHold) until
// IsQuiescent() is true: no system reads or writes their render state, their
// real slots are neither reassigned (stolen) nor freed, they are not in the
// render list, their output fades to silence from the end of
// the previous block (D-15 short fade). After the hold the render state is
// what the late chunk wrote and the voice continues from the next block.
// StragglersInFlight() is for statistics / diagnostics only.
//
// What a source kind keeps to itself because of late chunks (stage 2,
// comment only): ScheduleBackground (Publish, audio thread) may run while a
// late chunk still renders a voice of the previous block. It must therefore
// not write the render state of a voice (whatever RenderBatch reads or writes
// for it: ring read positions, playheads, fades, the per-voice decoder while
// RenderBatch may use it). Publish changes reach the render side only through
// an atomic hand-over (write the request, publish it with a release store) that
// the voice's next RenderBatch takes in at its head. Jobs posted for a voice
// must not race what RenderBatch writes either (single producer / single
// consumer rings, the job's done counter checked by whoever touches the shared
// decoder state).
//
// Control (RunEventLogic) may change a voice's decoder, ring write position
// and loop in place (a SetLoop that keeps the buffered data): sources rely on
// core sending no Control op to a held voice (D-43: no system touches a held
// voice's render state), which core guarantees.
//
// Errors after the start (stream IO, decode) can only end a voice through
// VoiceRenderStatus::ended: core reports such a playback as completed. An end
// reason in VoiceRenderStatus is planned for contract 1.2 (api stage).
#ifndef SND_INTERNAL_SOURCE_IF_HPP
#define SND_INTERNAL_SOURCE_IF_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/ext/codec.hpp"
#include "snd/ext/types.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/schema.hpp"
#include "snd/types.h"

namespace snd::source {

struct BlockInfo {
  BlockIndex index;
  ClockSample clock;        // clock of the first frame of the block
  std::uint32_t frames;     // = Config.blockSize
  std::uint32_t sampleRate; // internal rate
};

// Per-thread scratch (thread arena, 3.3). Valid only during the call it is passed to.
struct Scratch {
  std::uint8_t* base;
  std::size_t bytes;
  std::size_t used;
  // Bump allocation; returns nullptr when exhausted (caller renders silence and reports starved).
  void* Take(std::size_t n, std::size_t align = kBufferAlignment) noexcept {
    std::size_t p = (used + (align - 1)) & ~(align - 1);
    if (p + n > bytes) return nullptr;
    used = p + n;
    return base + p;
  }
};

// ---------------------------------------------------------------------------
// Assets resolved by core from the bank (bank_format ASST row + chunks)
// ---------------------------------------------------------------------------
enum class LoadForm : std::uint8_t { PcmResident = 0, CompressedResident = 1, DecompressCache = 2, Stream = 3 };

struct SourceAsset {
  CodecHeader codec;                // codec, channels, rate, frames, pre-skip, pre-roll, extra
  const std::uint8_t* data;         // resident data in bank memory; nullptr for Stream
  std::uint64_t dataBytes;
  const std::uint8_t* preload;      // preload chunk (6.1) or nullptr
  std::uint32_t preloadBytes;
  std::uint32_t preloadFrames;
  const std::uint64_t* seekFrames;  // SEEK.frame column slice
  const std::uint64_t* seekOffsets; // SEEK.byteOffset column slice
  std::uint32_t seekCount;
  std::uint32_t streamFile;         // STRM row (Stream form)
  const char* streamPath;           // SPTH string (Stream form)
  std::uint64_t streamDataOffset;
  std::uint64_t loopStart;          // frames
  std::uint64_t loopEnd;            // 0 = no loop
  Id assetId;                       // PCM cache key
  LoadForm form;
  std::uint8_t stallPolicy;         // 0 time priority, 1 sound priority (6.3)
  std::uint8_t virtualReturn;       // 0 continue, 1 restart, 2 never (4.4, 6.3)
  std::uint8_t ioPriority;          // 0 ambience, 1 voice, 2 music (6.1)
  std::uint16_t bankSlot;
  std::uint16_t reserved;           // stage 2 (comment only): core writes the low 16 bits of the bank
                                    // handle's generation (a new value per load); sources key caches with it
};

// ---------------------------------------------------------------------------
// Kind-specific start parameters (SourceStartRequest::params)
// ---------------------------------------------------------------------------
struct OscillatorParams {
  std::uint32_t waveform;  // bank_format::Waveform
  float frequencyHz;
  float duty;
  float tablePosition;
  const SourceAsset* wavetable;  // nullptr unless wavetable
};
struct NoiseParams {
  std::uint32_t color;  // 0 white, 1 pink, 2 brown
  std::uint32_t seed;
};
struct GranularParams {
  const SourceAsset* asset;
  float density, lengthMs, position, pitchJitterCents;
};
struct PatchParams {
  const std::uint8_t* program;  // PTCH bytes, interpreted by the patch kind
  std::uint32_t programBytes;
  std::uint32_t reserved;
};
struct CallbackParams {
  // 1.2 (D-94): the desc as the game gave it. When `callback` is NULL, `cpp_callback`
  // is the snd::SynthCallback of the C++ wrapper (snd/sonus.hpp) and the Callback kind
  // calls it with the same snd_render_context viewed as snd::RenderContext (an alias).
  snd_synth_desc desc;
};
struct UserBufferParams {
  HandleRef buffer;
};
struct InputParams {
  snd_input_desc desc;
};

// Shared control block of a user buffer (5.1). Allocated by core at Create
// (Config.maxUserBuffers). The game writes through snd_write_user_buffer
// (writePos), the UserBuffer kind reads (readPos). Positions are monotonic
// frame counters; index = pos % frames.
struct alignas(64) UserBufferControl {
  float* channels[kMaxChannels];
  std::uint32_t channelCount;
  std::uint32_t frames;
  std::uint32_t sampleRate;
  std::uint32_t mode;  // snd_user_buffer_mode
  std::uint32_t loop;
  std::atomic<std::uint32_t> generation;  // live generation, 0 = free
  alignas(64) std::atomic<std::uint64_t> writePos;  // game side
  alignas(64) std::atomic<std::uint64_t> readPos;   // UserBuffer kind (Render)
};

// ---------------------------------------------------------------------------
// Start / control / render
// ---------------------------------------------------------------------------
enum SourceStartFlags : std::uint32_t {
  kStartLoop = 1u << 0,
  kStartVirtual = 1u << 1,    // voice starts virtual: do not prefetch yet (6.3)
  kStartLowLatency = 1u << 2, // Callback: synchronous mode (5.2)
};

struct SourceStartRequest {
  SlotIndex voiceSlot;
  HandleRef playback;
  ClockSample startClock;
  std::uint64_t startFrame;   // offset into the material (random start, same-bar entry ...)
  const SourceAsset* asset;   // Sample / Stream (nullptr for synth kinds)
  const void* params;         // *Params struct of the kind, or nullptr
  std::uint32_t channels;
  std::uint32_t seed;
  std::uint32_t syncGroup;    // 0 = none (8.4)
  std::uint32_t flags;        // SourceStartFlags
};

struct SourceStartResult {
  SlotIndex sourceSlot;         // kInvalidSlot on failure
  std::uint32_t endDetail;      // snd_end_detail when failed (e.g. NO_STREAM_SLOT)
  std::uint32_t needsPriming;   // 1: playback goes to Priming until QueryReady says ready
  // Stage 7-D (D-194, comment only): a source-rate kind without an asset (Input) writes
  // the rate of the frames it delivers here (0 = the internal rate / the asset's rate).
  // Core should use it as the voice's source rate (requests/D-03); until then it is 0
  // for every kind except an Input port configured with another rate.
  std::uint32_t reserved;
};

enum class ControlOp : std::uint8_t {
  Seek = 0,          // frame = target source frame
  Virtualize = 1,    // stop IO/decoding, keep position (6.3)
  Devirtualize = 2,  // seek to frame and re-prime
  Pause = 3,
  Resume = 4,
  SetLoop = 5,       // frame = loop start, frame2 = loop end
};
struct SourceControlOp {
  SlotIndex sourceSlot;
  ControlOp op;
  std::uint8_t reserved[3];
  std::uint64_t frame;
  std::uint64_t frame2;
};

struct RenderItem {
  SlotIndex voiceSlot;
  SlotIndex sourceSlot;
  SlotIndex realSlot;
  // Output-rate kinds: write frames [frameOffset, frameOffset + framesRequested) of `out`
  // (frameOffset = sample-accurate start inside the block, 4.4).
  // Source-rate kinds: frameOffset is 0 and framesRequested source frames (computed by core
  // from the pitch ramp and resampler history) are written from index 0; core resamples and
  // applies the start offset.
  std::uint32_t frameOffset;
  std::uint32_t framesRequested;
  std::uint32_t channels;         // channels of `out`
  Ramp pitch;                     // output-rate kinds only: rate ratio at block start/end,
                                  // already clamped to Config.maxPitchRatio (D-44)
  Playhead* playhead;             // RealVoice.playhead[realSlot] (Render owns, 1.1): read and advance
  float* const* out;              // non-interleaved, 64-byte aligned, zeroed; frameOffset+frames fit
  // 1.2 (D-90), appended: the transport the voice's playback follows (a synth started
  // with a `when` naming a transport), copied in the serial part like every other input.
  // transportBeats: quarter notes since the transport origin at the block's first frame
  // (BlockInfo::clock), -1 if none. transportBeatsPerSample: quarter notes per sample at
  // the block's tempo (0 while the transport does not play). snd_render_context's
  // transport_beats at frame f of the block = transportBeats + f * transportBeatsPerSample.
  double transportBeats;
  double transportBeatsPerSample;
  // Stage 7-D (D-191), appended: synth modulation of the Oscillator kind, copied in the
  // serial part like `pitch` (offsets at the block's first frame / one past its last,
  // linear per sample in between). RenderItem{} (zero) = no modulation, which is what
  // core writes until it fills them from Parameter curves (requests/D-01).
  Ramp dutyOffset;           // added to OscillatorParams::duty (Pulse)
  Ramp tablePositionOffset;  // added to OscillatorParams::tablePosition (wavetable, 0..1)
};

struct RenderBatch {
  SourceKind kind;
  std::uint8_t lodLevel;  // 2.4: >0 may select cheaper variants for NEW voices only
  std::uint16_t reserved;
  std::uint32_t count;
  const RenderItem* items;
  BlockInfo block;
  Scratch* scratch;       // calling thread's arena
  std::uint32_t chunkIndex;     // render-list chunk (late results are discarded by generation, 2.3)
  std::uint32_t chunkGeneration;
};

struct AdvanceItem {  // virtual voice: advance without rendering (4.4)
  SlotIndex voiceSlot;
  SlotIndex sourceSlot;
  std::uint32_t outputFrames;
  float pitch;
  Playhead* playhead;
};

// RT-safe requests to non-RT threads.
struct IoReadRequest {
  SlotIndex streamSlot;          // Stream table slot
  std::uint32_t chunkIndex;      // ring chunk to fill
  const char* path;              // bank SPTH string (stable while the bank is loaded)
  std::uint64_t offset;
  void* dst;
  std::uint32_t bytes;
  std::uint8_t priority;         // 6.1
  std::uint8_t reserved[3];
};

struct SourceServices {
  IAllocator* allocator;  // Init only (owner thread)
  schema::Tables* tables; // Stream rows are created/written inside RunEventLogic/Render per schema
  // Posts a job to a normal-worker lane. RT-safe, bounded; false = queue full (retry next block).
  bool (*postJob)(void* ctx, const JobDesc& job, JobLane lane) noexcept;
  void* jobCtx;
  // Posts a stream read to the IO scheduler. RT-safe, bounded.
  bool (*postIo)(void* ctx, const IoReadRequest& req) noexcept;
  void* ioCtx;
  // Codec lookup by fourcc (custom codecs override built-ins). RT-safe (table read).
  ICodec* (*findCodec)(void* ctx, std::uint32_t fourcc) noexcept;
  void* codecCtx;
  // Returns the user buffer control block (UserBuffer kind). RT-safe.
  UserBufferControl* (*userBuffer)(void* ctx, HandleRef buffer) noexcept;
  void* userBufferCtx;
};

struct SourceLimits {
  std::uint32_t maxVoices;          // Config.maxVirtualVoices
  std::uint32_t maxRealVoices;      // + fade reserve
  std::uint32_t maxStreams;
  std::uint32_t streamChunkBytes;
  std::uint64_t streamPoolBytes;
  std::uint32_t maxDecoders;
  std::uint32_t decoderSlotBytes;
  std::uint32_t voiceRingFrames;
  std::uint32_t callbackAheadBlocks;
  std::uint64_t pcmCacheBytes;
  std::uint32_t pcmCachePageBytes;
  std::uint32_t blockFrames;
  std::uint32_t sampleRate;
  std::uint32_t streamTargetAheadMs;
};

class ISourceKind {
 public:
  virtual ~ISourceKind() = default;

  virtual SourceKind Kind() const noexcept = 0;
  virtual bool RendersAtOutputRate() const noexcept = 0;

  // Owner thread, Create(): private pool size and initialisation.
  virtual std::size_t PoolBytes(const SourceLimits& limits) const noexcept = 0;
  virtual bool Init(void* pool, std::size_t bytes, const SourceLimits& limits,
                    const SourceServices& services) noexcept = 0;
  // Owner thread, Destroy(): no audio thread running; wait for posted jobs via their counters.
  virtual void Shutdown() noexcept = 0;

  // Audio thread (RunEventLogic). Allocation-free.
  virtual void Start(const SourceStartRequest* requests, std::uint32_t count,
                     SourceStartResult* results) noexcept = 0;
  // Releases instances; resources still used by in-flight jobs are returned by epoch (3.3).
  virtual void Release(const SlotIndex* sourceSlots, std::uint32_t count) noexcept = 0;
  virtual void Control(const SourceControlOp* ops, std::uint32_t count) noexcept = 0;
  // ready[i] = 1 when sourceSlots[i] can render from its start position (Priming -> Playing).
  virtual void QueryReady(const SlotIndex* sourceSlots, std::uint32_t count,
                          std::uint8_t* ready) const noexcept = 0;

  // RT worker or audio thread (Render). One call per batch; never per voice.
  // status[i].framesRendered = frames written for items[i] (source frames for
  // source-rate kinds); ended/starved as observed.
  virtual void RenderBatch(const RenderBatch& batch, VoiceRenderStatus* status) noexcept = 0;
  // Audio thread (Render, serial).
  virtual void AdvanceVirtual(const AdvanceItem* items, std::uint32_t count,
                              VoiceRenderStatus* status) noexcept = 0;
  // Audio thread (Publish). Post decode / callback-ahead jobs and IO for the next blocks.
  virtual void ScheduleBackground(const BlockInfo& block) noexcept = 0;

  // Any thread, pure: frames of lead time needed before `asset` can start at
  // `startFrame` (stream head-seek + decode; 0 for resident data). Used for the
  // minimum lookahead of reservations (8.2).
  virtual std::uint32_t PrimeFrames(const SourceAsset& asset,
                                    std::uint64_t startFrame) const noexcept = 0;
};

// The composition root fills this with the source module's kinds and hands it
// to core at Create. A null entry means the kind is unsupported in this build
// (Start of such a voice ends the playback with SND_END_ERROR / UNSUPPORTED).
struct SourceKindSet {
  ISourceKind* kinds[static_cast<std::size_t>(SourceKind::Count)];
};

}  // namespace snd::source

#endif  // SND_INTERNAL_SOURCE_IF_HPP
