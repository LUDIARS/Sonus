// IO thread and IO scheduler (design.md 2 thread table, 6.1, 6.3, 10.3).
//
// One IO thread owns the IFileSystem (it is the only caller, 6.5) and serves
// two kinds of work:
//
//   stream reads   source::IoReadRequest posted from the audio thread / RT
//                  workers through SourceServices::postIo (D-08): bounded,
//                  lock-free, never waits. One chunk (<= streamChunkBytes) per
//                  request, read into a StreamPool chunk and delivered IN
//                  REQUEST ORDER to the stream's StreamChannel (the compressed
//                  ring "IO -> decode" of chapter 6).
//   file reads     FileRequest submitted by non-RT threads (bank loading,
//                  10.3): whole-file or range reads into caller memory, or a
//                  size query. Split into pieces so they share the bandwidth.
//
// Order of service (6.1): every request has an IoClass. The class with the
// best rank goes first (default music > voice > ambience > bank; configurable
// with IoSchedulerConfig::classRank), FIFO inside a class. Bytes issued per
// window (default 100 ms) are limited to bytesPerWindow
// (Config.tuning.ioBytesPer100ms); a read that does not fit waits for the next
// window. Reads are never skipped to fill the remaining budget with a smaller
// lower-ranked read (the order stays the priority order).
//
// Starvation policy (aging): a request that has waited agingNs (default 300 ms
// = 3 windows) is served before any younger request, oldest first, whatever
// its class. So a lower class is delayed by higher classes at most ~agingNs
// plus the time to serve the requests that aged before it; it never starves.
//
// Cancel / seek (6.3): each StreamChannel has an epoch. StreamChannel::Cancel()
// (audio thread: stop, virtualise, seek) bumps it. Requests posted before the
// bump are stale:
//   - queued (not yet issued): dropped by the IO thread, chunk back to the pool;
//   - in flight: IFileSystem cannot abort a read, so the IO thread keeps
//     polling it; when it completes the data is discarded and the chunk goes
//     back to the pool. A stale in-flight read does not hold back the delivery
//     of newer reads of the same channel;
//   - already delivered to the ring: StreamChannel::Pop() skips and releases them.
// A seek is Cancel() followed by new requests from the new position.
//
// Loops (6.3): StreamReadPlanner (stream_read_planner.hpp) produces the read
// sequence that wraps from the loop end to the loop start, so the head of the
// loop is read ahead as the cursor approaches the loop end.
//
// Notifications the audio thread reads without waiting (all atomics on the
// StreamChannel, slot-indexed): delivered records (ring), errors, read
// position, and the IO-side stall flag: a request outstanding longer than
// stallNs (default Config.tuning.streamTargetAheadMs) raises IoStalled() and
// counts IoStallEvents() once per request. The consumer-side stall count
// (schema Stream.stallCount) stays with Render.
//
// Threads (summary):
//   Init / Start / Shutdown / Destroy / Submit / Cancel(FileRequest)  non-RT
//   Post / PostIo / Wake / Channel(): Cancel, status                  RT-safe (audio thread, RT workers)
//   Channel(): Pop / Peek / Release / Reclaim                         RT-safe, the stream's consumer
//   RunOnce                                                           test driver (no IO thread running)
//
// Composition root wiring:
//   StreamPool pool;   pool.Init(alloc, cfg.memory.streamPoolBytes, cfg.memory.streamChunkBytes, cfg.maxStreams);
//   IoScheduler io;    io.Init(alloc, fs, pool, MakeIoSchedulerConfig(cfg));  io.Start();
//   services.postIo = &IoScheduler::PostIo;  services.ioCtx = &io;
//   (default FS)       defaultFs.SetCompletionNotify(&IoScheduler::WakeThunk, &io);
//   ...
//   io.Shutdown();  // after the audio thread stopped and decode jobs finished
//   io.Destroy();   pool.Destroy();
#ifndef SND_IO_IO_SCHEDULER_HPP
#define SND_IO_IO_SCHEDULER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/base/spsc.hpp"
#include "snd/config.h"
#include "snd/ext/file_system.hpp"
#include "snd/ext/types.hpp"
#include "snd/internal/common.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/io/stream_pool.hpp"

namespace snd::io {

class IoScheduler;

// Service classes. The first three equal source::SourceAsset::ioPriority /
// IoReadRequest::priority (6.1); Bank is used by FileRequest by default.
enum class IoClass : std::uint8_t { Ambience = 0, Voice = 1, Music = 2, Bank = 3, Count = 4 };
inline constexpr std::uint32_t kIoClassCount = static_cast<std::uint32_t>(IoClass::Count);

// Replaceable clock (deterministic tests). nowNs must be monotonic and callable
// from the IO thread. nowNs == nullptr: the platform monotonic clock.
struct IoClock {
  std::uint64_t (*nowNs)(void* user) noexcept = nullptr;
  void* user = nullptr;
};

struct IoSchedulerConfig {
  std::uint32_t maxStreams = 32;              // channels = stream slots (Config.maxStreams)
  std::uint32_t ringChunks = 8;               // records per channel ring; also the per-stream cap of
                                              // outstanding chunks (queued + in flight + delivered + held)
  std::uint32_t requestQueueCapacity = 256;   // postIo queue (MPMC)
  std::uint32_t fileQueueCapacity = 32;       // FileRequests outstanding at once
  std::uint32_t maxInFlight = 8;              // reads handed to the IFileSystem at once
  std::uint32_t maxOpenFiles = 0;             // 0 = maxStreams + 16
  std::uint32_t bytesPerWindow = 4u << 20;    // Config.tuning.ioBytesPer100ms; 0 = unlimited
  std::uint64_t windowNs = 100000000ull;      // 100 ms
  std::uint32_t filePieceBytes = 256u << 10;  // FileRequest piece size (clamped to bytesPerWindow)
  // Rank per IoClass (index = class); lower rank is served first. Ties: lower class index first.
  std::uint8_t classRank[kIoClassCount] = {2, 1, 0, 3};  // ambience, voice, music, bank
  std::uint64_t agingNs = 300000000ull;       // starvation bound (see header comment)
  std::uint64_t stallNs = 200000000ull;       // IO-side stall threshold per stream request
  std::uint64_t pollIntervalNs = 1000000ull;  // IO thread poll period while reads are in flight
  IoClock clock{};
  const char* threadName = "snd-io";
};

// Fills the config from snd_config (maxStreams, ioBytesPer100ms, streamTargetAheadMs).
IoSchedulerConfig MakeIoSchedulerConfig(const snd_config& config) noexcept;

// ---------------------------------------------------------------------------
// Compressed ring records (IO thread -> decode)
// ---------------------------------------------------------------------------
enum class ChunkStatus : std::uint8_t {
  Ok = 0,          // `bytes` valid bytes in the chunk (< requested only at end of file)
  ReadError = 1,   // IFileSystem reported Error; no chunk
  OpenFailed = 2,  // the file could not be opened (or the open-file table was full); no chunk
};

struct ChunkRecord {
  std::uint64_t offset;     // file offset of the first byte
  std::uint32_t chunk;      // StreamPool chunk, kInvalidChunk unless status == Ok
  std::uint32_t bytes;      // valid bytes
  std::uint32_t requested;  // IoReadRequest::bytes
  std::uint32_t tag;        // IoReadRequest::chunkIndex (caller's ring index), echoed back
  std::uint32_t epoch;      // channel epoch the request was posted in
  ChunkStatus status;
  std::uint8_t reserved[3];
};
static_assert(sizeof(ChunkRecord) == 32);

// Per stream slot: the compressed ring and the status the audio thread reads.
class StreamChannel {
 public:
  StreamChannel() noexcept = default;
  StreamChannel(const StreamChannel&) = delete;
  StreamChannel& operator=(const StreamChannel&) = delete;

  // --- audio thread (the stream slot's owner); RT-safe ------------------------
  // Makes every request posted so far stale (stop, virtualise, seek) and wakes
  // the IO thread so queued reads are dropped and their reservations returned.
  // Must not race with Post() for the same slot (same thread, as in RunEventLogic).
  void Cancel() noexcept;
  std::uint32_t Epoch() const noexcept { return epoch_.value.load(std::memory_order_acquire); }

  // --- consumer (one decode job at a time, or the audio thread when no job runs) ---
  // RT-safe. Next current record in request order; stale records are released
  // and skipped. false when nothing is ready. The chunk stays borrowed until Release.
  bool Pop(ChunkRecord& out) noexcept;
  // RT-safe. Oldest current record without removing it (stale ones are released first).
  const ChunkRecord* Peek() noexcept;
  // RT-safe. Gives the chunk of a popped record back to the pool and ends the
  // record's reservation. Call exactly once per popped record (any status).
  void Release(const ChunkRecord& record) noexcept;
  // RT-safe. Pops and releases every record (current or stale). Returns true
  // when nothing of this channel is outstanding any more (no queued or
  // in-flight read, nothing in the ring, nothing held): the slot is clean.
  // Use after Cancel() when the stream is released, until it returns true,
  // before handing the slot to a new stream (or rely on Shutdown()).
  bool Reclaim() noexcept;

  // --- status: any thread, lock-free ----------------------------------------------
  // Requests of this channel posted and not yet released (<= Capacity()).
  std::uint32_t Outstanding() const noexcept { return reserved_.value.load(std::memory_order_acquire); }
  std::uint32_t Capacity() const noexcept { return capacity_; }
  // Records waiting in the ring (approximate).
  std::uint32_t Ready() const noexcept { return ring_.SizeApprox(); }
  // End offset (offset + bytes) of the last record delivered in the current
  // epoch; this is the value meant for the schema column Stream.readPos.
  std::uint64_t ReadPos() const noexcept { return readPos_.load(std::memory_order_acquire); }
  std::uint32_t Delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }
  std::uint32_t Errors() const noexcept { return errors_.load(std::memory_order_relaxed); }
  // A current request has been outstanding longer than IoSchedulerConfig::stallNs.
  bool IoStalled() const noexcept { return stalled_.load(std::memory_order_acquire) != 0u; }
  std::uint32_t IoStallEvents() const noexcept { return stallEvents_.load(std::memory_order_relaxed); }

 private:
  friend class IoScheduler;
  void Unreserve() noexcept { reserved_.value.fetch_sub(1u, std::memory_order_acq_rel); }

  base::CachePadded<std::atomic<std::uint32_t>> epoch_;     // written by the audio thread
  base::CachePadded<std::atomic<std::uint32_t>> reserved_;  // Post +1, release / drop -1
  base::SpscRing<ChunkRecord> ring_;                        // IO thread -> consumer
  std::atomic<std::uint64_t> readPos_{0};                   // IO thread
  std::atomic<std::uint32_t> delivered_{0};                 // IO thread
  std::atomic<std::uint32_t> errors_{0};                    // IO thread
  std::atomic<std::uint32_t> stalled_{0};                   // IO thread
  std::atomic<std::uint32_t> stallEvents_{0};               // IO thread
  StreamPool* pool_ = nullptr;
  IoScheduler* owner_ = nullptr;
  std::uint32_t capacity_ = 0;
};

// ---------------------------------------------------------------------------
// File requests (non-RT callers: bank loading)
// ---------------------------------------------------------------------------
enum class FileState : std::uint32_t {
  Idle = 0,       // not submitted (or finished and reusable)
  Queued = 1,     // accepted by Submit
  Done = 2,       // bytesRead valid (< bytes only at end of file); fileSize valid
  Error = 3,      // open or read failed
  Cancelled = 4,  // Cancel() or Shutdown() before completion
};

struct FileResult {
  FileState state;
  std::uint64_t bytesRead;
  std::uint64_t fileSize;  // UINT64_MAX if unknown (open failed)
};

// Called on the IO thread after the request reached its final state; it does
// not touch the FileRequest (which the submitter may already reuse or free).
// Keep it short (post a job, signal a semaphore); it must not call Submit/Cancel
// synchronously waiting for IO.
using FileCallback = void (*)(void* user, const FileResult& result) noexcept;

// Caller-owned request. Fill the inputs, Submit(), then either wait for the
// callback or poll Finished()/Result(). The object must stay alive and unchanged
// until Finished() is true. dst must stay valid until then too.
struct FileRequest {
  // --- inputs ---
  const char* path = nullptr;      // relative to the file system root; read only until Finished()
  std::uint64_t offset = 0;        // first byte to read
  std::uint64_t bytes = 0;         // bytes to read into dst; the read stops early at end of file
  void* dst = nullptr;             // nullptr: size query only (fileSize, no read, no bandwidth)
  IoClass ioClass = IoClass::Bank;
  FileCallback onComplete = nullptr;
  void* user = nullptr;

  // --- outputs (any thread) ---
  FileState State() const noexcept { return static_cast<FileState>(state.load(std::memory_order_acquire)); }
  bool Finished() const noexcept {
    const FileState s = State();
    return s == FileState::Done || s == FileState::Error || s == FileState::Cancelled;
  }
  // Valid once Finished().
  FileResult Result() const noexcept { return FileResult{State(), bytesRead, fileSize}; }

  // --- internal (scheduler) ---
  std::atomic<std::uint32_t> state{0};
  std::atomic<std::uint32_t> cancel{0};
  std::uint64_t bytesRead = 0;
  std::uint64_t fileSize = UINT64_MAX;
};

// ---------------------------------------------------------------------------
// Statistics (any thread, relaxed snapshot)
// ---------------------------------------------------------------------------
struct IoStats {
  std::uint64_t readsIssued = 0;        // ReadAsync calls accepted
  std::uint64_t bytesIssued = 0;        // bytes requested by those reads
  std::uint64_t readsCompleted = 0;     // Done (any epoch)
  std::uint64_t bytesCompleted = 0;
  std::uint64_t readErrors = 0;         // Error from the file system
  std::uint64_t openFailures = 0;
  std::uint64_t streamPosted = 0;       // accepted Post()s
  std::uint64_t streamRejected = 0;     // Post()s refused (full, invalid, stopped)
  std::uint64_t staleDropped = 0;       // stale requests dropped before issue
  std::uint64_t staleCompleted = 0;     // stale reads that completed and were discarded
  std::uint64_t fsRejects = 0;          // ReadAsync returned false (retried later)
  std::uint64_t budgetWaits = 0;        // windows in which a read waited for the next window
  std::uint64_t poolStarved = 0;        // issue attempts that found the stream pool empty
  std::uint64_t agedServed = 0;         // requests served by the aging rule
  std::uint64_t stallEvents = 0;        // sum of IoStallEvents over channels
  std::uint64_t filesCompleted = 0;     // FileRequests finished (any state)
  std::uint64_t maxWindowBytes = 0;     // largest bytes issued in one window
  std::uint32_t inFlight = 0;
  std::uint32_t pending = 0;            // drained, not yet issued
};

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------
class IoScheduler {
 public:
  IoScheduler() noexcept = default;
  ~IoScheduler() { Destroy(); }
  IoScheduler(const IoScheduler&) = delete;
  IoScheduler& operator=(const IoScheduler&) = delete;

  // Non-RT. All memory comes from `allocator` here (MemTag::Misc, rings
  // MemTag::StreamPool). `fs` and `pool` must outlive the scheduler; pool
  // chunk size bounds IoReadRequest::bytes. Does not start the thread.
  bool Init(IAllocator& allocator, IFileSystem& fs, StreamPool& pool, const IoSchedulerConfig& config) noexcept;
  // Non-RT. Starts the IO thread (normal priority). false if not initialised,
  // already started or the thread could not be created.
  bool Start() noexcept;
  // Non-RT. Stops accepting work, cancels everything queued (FileRequests end
  // Cancelled), waits until every in-flight read has completed, closes all
  // files and stops the IO thread. Works with or without Start(). Precondition
  // for freeing chunk memory: no read is left in the IFileSystem afterwards.
  // Call after the audio thread and decode jobs stopped using the channels.
  void Shutdown() noexcept;
  // Non-RT. Shutdown() if needed, returns every chunk still in the rings to the
  // pool and frees the scheduler's memory.
  void Destroy() noexcept;

  // --- RT side (audio thread, RT workers) -----------------------------------
  // Queues a stream read. RT-safe: lock-free, bounded, never waits. Returns
  // false (nothing queued; the caller keeps ownership of req.dst) when:
  // the queue or the channel is full (retry later), after Shutdown, or the
  // request is invalid (streamSlot >= maxStreams, bytes == 0 or > chunk size,
  // dst neither nullptr nor the start of a borrowed StreamPool chunk).
  // dst == nullptr: the IO thread borrows a chunk for the slot when it issues
  // the read. On success ownership of a dst chunk passes to the scheduler.
  bool Post(const source::IoReadRequest& req) noexcept;
  // SourceServices::postIo (ctx = this).
  static bool PostIo(void* ctx, const source::IoReadRequest& req) noexcept;
  // Wakes the IO thread (RT-safe, any thread; cheap when it is already awake).
  void Wake() noexcept;
  // Completion hook for file systems that can notify (DefaultFileSystem::SetCompletionNotify).
  static void WakeThunk(void* ctx) noexcept;

  // Channel of a stream slot (slot < ChannelCount()). The reference is stable
  // between Init and Destroy.
  StreamChannel& Channel(SlotIndex slot) noexcept { return channels_[slot]; }
  std::uint32_t ChannelCount() const noexcept { return channelCount_; }

  // --- non-RT callers (bank loading, tools) ------------------------------------
  // Any non-RT thread. false (state stays Idle) if the request is invalid
  // (no path, or dst set with bytes == 0), already in use, the file queue is
  // full, or the scheduler is shut down.
  bool Submit(FileRequest& request) noexcept;
  // Any thread. Asks to cancel; the request still ends through the normal path
  // (Cancelled, or Done/Error if it finished first). A piece already in flight
  // completes before the request ends.
  void Cancel(FileRequest& request) noexcept;

  // --- test driver --------------------------------------------------------------
  // One iteration of the IO loop on the calling thread (drain queues, poll,
  // deliver, issue). Only when Start() was not called. Returns true if work
  // remains (queued, in flight or waiting for the next window).
  bool RunOnce() noexcept;

  // --- status (any thread) ----------------------------------------------------------
  IoStats Stats() const noexcept;
  // Incremented by the IO thread after each drain of the input queues. Once it
  // advanced by 2 after a moment T, every request posted / submitted before T
  // has been drained and its path copied (bank unload epoch, 3.3).
  std::uint64_t DrainEpoch() const noexcept { return drainEpoch_.value.load(std::memory_order_acquire); }
  bool Running() const noexcept;

  struct Impl;  // private (io/src)

 private:
  Impl* impl_ = nullptr;
  StreamChannel* channels_ = nullptr;
  std::uint32_t channelCount_ = 0;
  base::CachePadded<std::atomic<std::uint64_t>> drainEpoch_;
  IAllocator* alloc_ = nullptr;
};

}  // namespace snd::io

#endif  // SND_IO_IO_SCHEDULER_HPP
