// Sample and Stream source kinds (design.md 5, 6; source_if.hpp).
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
//     no decoder either the start is refused (DECODER_POOL_FULL).
//   - Streams read through io::IoScheduler / StreamChannel directly on the
//     audio thread (IoScheduler::Post and StreamChannel::Cancel are RT-safe and
//     must run on the same thread for one slot; core's relayed postIo would
//     post after a Cancel with the new epoch). SourceServices::postIo is unused.
//   - Stream underrun (6.3): short fade out, Stream.stallCount + starved;
//     resume with a short fade in once the ring holds the ready amount again.
//     Time priority skips the frames that elapsed; sound priority resumes where
//     it stopped.
//   - Stream table columns (D-45): ringWrite / ringRead ARE the ring positions;
//     readPos mirrors StreamChannel::ReadPos() (written by the decode job, the
//     channel's consumer, as the IO side's value).
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
  bool enableSample = true;
  bool enableStream = true;
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
  RuntimeStats Stats() const noexcept;

  struct Impl;  // source/src

 private:
  Impl* impl_ = nullptr;
  IAllocator* alloc_ = nullptr;
  SourceKindSet kinds_{};
};

}  // namespace snd::source

#endif  // SND_SOURCE_RUNTIME_HPP
