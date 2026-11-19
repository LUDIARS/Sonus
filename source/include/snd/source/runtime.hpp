// Sample, Stream, Callback and UserBuffer source kinds (design.md 5, 6; source_if.hpp).
// Callback (callback_kind.hpp) and UserBuffer (user_buffer_kind.hpp) were added at the
// api stage (contract 1.2, D-94).
//
// snd::source::Runtime owns the source module's ISourceKind objects and the
// state they share (the decoder pool, D-14). The composition root
// (api/compose) creates one Runtime per system, initialises it, and hands
// Kinds() to core (CoreDesc::sources). Core then calls PoolBytes / Init of each
// kind at Create and Shutdown at Destroy.
//
//   snd::source::Runtime src;
//   snd::source::RuntimeDesc rd;
//   rd.allocator = &alloc;          // Init only (the Runtime's own bookkeeping)
//   rd.io = &ioScheduler;           // Stream kind: IO scheduler and its stream pool.
//   rd.streamPool = &streamPool;    //   Without them the Stream kind is absent (UNSUPPORTED).
//   src.Init(rd);
//   coreDesc.sources = &src.Kinds();
//   ... core Create / run / Destroy ...
//   src.Destroy();                  // after core's Destroy
//
// Decisions (stage 2):
//   - Load forms (6.2): PcmResident plays PCMF / PC16 from bank memory
//     directly in RenderBatch. CompressedResident (and PcmResident assets whose
//     codec is not PCM) decodes on the prefetch lane into a per-voice ring
//     (voiceRingFrames). DecompressCache decodes the whole asset once into the
//     PCM cache (pcmCacheBytes in pages of pcmCachePageBytes, LRU, pinned while
//     playing); a full cache falls back to the per-voice ring, and if that has
//     no decoder either the start is refused (PCM_CACHE_FULL).
//     A DecompressCache start inside a part the cache does not hold yet (a start
//     offset past the head) plays that voice through a per-voice ring to its
//     end while the cache fills in the background; without a free decoder it
//     waits for the cache (Priming).
//   - Work on the audio thread is bounded everywhere: Start (8 voices per
//     call), RenderBatch for a seek / loop change (4 voices per block, an
//     atomic budget shared by every chunk of the block) and Publish (4 streams
//     per block) decode at most the ready amount (a few blocks) each.
//   - Seek and SetLoop arriving in one block compose: the voice ends at the
//     last Seek's target (mapped with the loop in force when that Seek came)
//     and follows the last SetLoop. A SetLoop whose loop end is behind the
//     playhead sends the voice to the loop start (every form).
//   - Seek / SetLoop of data in memory (resident, cache, per-voice ring) take
//     effect inside the next block without a gap. A per-voice ring's decode
//     job that is posted but not started is claimed (it then does nothing);
//     while a job is running, the voice plays on from its ring and switches in
//     the block after the job, with a 5 ms cross-fade (D-15) from the old
//     position to the target: the one exception to "no fade on a seek". A loop change keeps the
//     buffered data when it agrees with the new loop (only what lies past the
//     point where the loops differ is decoded again). A stream's SetLoop only
//     swaps the read plan when the data read so far agrees; otherwise it
//     re-reads from the current position. A stream SetLoop that shortens the
//     loop keeps the PCM already decoded up to the new loop end, drops what is
//     past it and goes on at the loop start (from the preload when the loop
//     start lies in it, so no gap); only data not decoded yet is read again.
//   - Streams read through io::IoScheduler / StreamChannel directly on the
//     audio thread (IoScheduler::Post and StreamChannel::Cancel are RT-safe and
//     must run on the same thread for one slot; core's relayed postIo would
//     post after a Cancel with the new epoch). SourceServices::postIo is unused.
//   - Stream underrun (6.3): short fade out, Stream.stallCount + starved;
//     resume with a short fade in once the ring holds the ready amount again.
//     Time priority skips the frames that elapsed (the playhead follows the
//     clock, also while re-seeking ahead); sound priority resumes where it
//     stopped.
//   - Stream file header (bank-format.md 6.3): magic, version, codec, channels,
//     sampleRate, assetId, totalFrames, dataOffset and dataBytes (the size the
//     codec needs) are checked; a short read inside the data is an IO error.
//   - Errors after the start (IO, decode) end the voice through `ended`: core
//     cannot tell them from a normal end (VoiceRenderStatus has no reason; the
//     game sees SND_END_COMPLETED). Counted in RuntimeStats::streamIoErrors /
//     decodeErrors. Planned: an end reason in VoiceRenderStatus (contract 1.2).
//   - Stream table columns (D-45): ringWrite / ringRead ARE the ring positions;
//     readPos mirrors StreamChannel::ReadPos() (written by the decode job, the
//     channel's consumer, as the IO side's value).
//   - Late chunks (D-43, source_if.hpp): Publish (ScheduleBackground) never
//     writes what RenderBatch reads or writes; changes reach the render side
//     through atomic requests that the voice's next RenderBatch takes in.
//
// Memory of the PCM rings (from the kinds' pools, sized at Create):
//   streams   maxStreams x maxVoiceChannels rings of
//             NextPow2(streamTargetAheadMs x rate + 4 blocks) frames x 4 bytes
//             (defaults 32 x 8 x 16384 x 4 = 16 MiB; stereo projects with
//             maxVoiceChannels 2: 4 MiB)
//   per-voice maxDecoders x maxVoiceChannels rings of NextPow2(voiceRingFrames)
//             frames x 4 bytes (defaults 64 x 8 x 4096 x 4 = 8 MiB; 2 ch: 2 MiB)
//   plus the PCM cache (pcmCacheBytes) and 8 KiB of decode carry per decode
//   context (maxDecoders + maxStreams + 64 cache entries).
//
// Threads: Init / Destroy / Stats on the owner thread (Stats any thread,
// approximate). Everything else goes through ISourceKind (source_if.hpp).
#ifndef SND_SOURCE_RUNTIME_HPP
#define SND_SOURCE_RUNTIME_HPP

#include <cstdint>

#include "snd/ext/allocator.hpp"
#include "snd/internal/source_if.hpp"

namespace snd::io {
class IoScheduler;
class StreamPool;
}  // namespace snd::io

namespace snd::source {

struct RuntimeDesc {
  IAllocator* allocator = nullptr;      // required
  io::IoScheduler* io = nullptr;        // Stream kind (with streamPool)
  io::StreamPool* streamPool = nullptr; // the pool `io` was initialised with
  // Config.maxVoiceChannels (1..8): the PCM rings guarantee that maxStreams
  // streams and maxDecoders per-voice rings can each play this many channels
  // at once (memory below).
  std::uint32_t maxVoiceChannels = 8;
  // Config.maxBanks: one unregistration counter per bank slot (BankUnregistered).
  std::uint32_t maxBanks = 64;
  bool enableSample = true;
  bool enableStream = true;
  // 1.2 (api stage, D-94): synth callbacks (snd_play_synth) and game buffers
  // (snd_play_user_buffer). Both need no IO.
  bool enableCallback = true;
  bool enableUserBuffer = true;
};

// Counters (relaxed snapshot).
struct RuntimeStats {
  std::uint32_t sampleInstances = 0;     // live Sample instances (incl. closing)
  std::uint32_t streamInstances = 0;     // live Stream instances (incl. closing)
  std::uint32_t decodersInUse = 0;       // decoder pool slots in use
  std::uint32_t decoderCapacity = 0;
  std::uint32_t cacheEntries = 0;        // PCM cache entries (filled or filling)
  std::uint32_t cachePagesUsed = 0;
  std::uint32_t cachePages = 0;
  std::uint64_t cacheHits = 0;
  std::uint64_t cacheMisses = 0;
  std::uint64_t cacheEvictions = 0;
  std::uint64_t cacheFallbacks = 0;      // cache full -> per-voice ring
  std::uint64_t rejectedDecoder = 0;     // DECODER_POOL_FULL
  std::uint64_t rejectedStreamSlot = 0;  // NO_STREAM_SLOT
  std::uint64_t rejectedOther = 0;       // unsupported codec / bad asset
  std::uint64_t sampleUnderruns = 0;     // per-voice ring ran dry (blocks)
  std::uint64_t streamStalls = 0;        // stall events (6.3)
  std::uint64_t streamStallBlocks = 0;
  std::uint64_t streamIoErrors = 0;      // read error / open failed / bad stream header
  std::uint64_t decodeErrors = 0;
  std::uint64_t jobsPosted = 0;
  std::uint64_t jobsRejected = 0;        // postJob queue full (retried next block)
  std::uint64_t ioPosted = 0;
  std::uint64_t ioRejected = 0;          // IoScheduler::Post refused (retried next block)
  std::uint64_t inlinePrimes = 0;        // starts primed on the audio thread
  // Callback / UserBuffer kinds (D-94).
  std::uint32_t callbackInstances = 0;
  std::uint64_t callbackCalls = 0;
  std::uint64_t callbackUnderruns = 0;   // Buffered: the ring ran dry after the voice played
  std::uint64_t callbackDemotions = 0;   // LowLatency over the deadline -> Buffered
  std::uint64_t callbackRejected = 0;    // Buffered start without a free ring
  std::uint32_t userBufferInstances = 0;
  std::uint64_t userBufferUnderruns = 0; // Stream buffer had nothing written in time
};

class Runtime {
 public:
  Runtime() noexcept = default;
  ~Runtime() { Destroy(); }
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Owner thread. false on a missing allocator or allocation failure.
  bool Init(const RuntimeDesc& desc) noexcept;
  // Owner thread, after core's Destroy (the kinds are shut down then).
  void Destroy() noexcept;

  const SourceKindSet& Kinds() const noexcept { return kinds_; }
  // core::CoreDesc::bankUnregistered (user = this Runtime). Any thread, RT-safe
  // (one atomic increment): the PCM cache frees the bank's entries once unpinned.
  static void BankUnregistered(void* runtime, std::uint16_t bankSlot) noexcept;
  RuntimeStats Stats() const noexcept;
  // Any thread (D-97): a synth callback of the playback `handle` may still be called (a
  // render-ahead job is posted or running). The api holds on_end back meanwhile.
  bool CallbackBusy(std::uint64_t handle) const noexcept;

  struct Impl;  // source/src

 private:
  Impl* impl_ = nullptr;
  IAllocator* alloc_ = nullptr;
  SourceKindSet kinds_{};
};

}  // namespace snd::source

#endif  // SND_SOURCE_RUNTIME_HPP
