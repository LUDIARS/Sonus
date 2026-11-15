// RT workers and the chunk deadline of the parallel render (design.md 2.3).
//
// A fixed set of RT worker threads (Config.jobs.realtimeWorkers) at the audio
// thread's priority. Per block the audio thread calls Run(): the render list
// is split into `chunkCount` chunks, each managed atomically as
//
//     Pending (未着手) -> Taken (着手) -> Done (完了)           (or -> Late)
//
// Workers are woken with their pre-made semaphores; the audio thread itself
// also takes Pending chunks. When no Pending chunk is left, the audio thread
// spins at most Config.tuning.rtSpinMicroseconds for chunks still in flight;
// chunks that do not finish are marked Late and reported as not done — they
// contribute nothing this block (the caller fades those voices, 2.3). A worker
// that finishes a Late chunk afterwards fails its Done transition (the chunk
// word carries the batch generation), so its result is discarded and counted.
//
// This path does NOT go through IJobSystem and never allocates, locks or does
// IO: Run() and the worker loop are RT-safe (principle 1; checked under
// SND_RT_SCOPE in jobs/test). Only Init/Shutdown are non-RT.
//
// Contract for chunk functions (RtChunkFn):
//  - Called on the audio thread (threadIndex 0) or an RT worker (1..N),
//    possibly concurrently for different chunks, under principle 1.
//  - Write results ONLY to memory owned by the chunk (e.g. a per-chunk
//    accumulator selected by chunkIndex) or to the thread's scratch. Results
//    of chunk i may be used by the caller after Run() only if ChunkDone(i).
//  - A Late chunk may still be running when Run() returns and even when the
//    next Run() starts. Its function keeps reading its inputs and writing its
//    outputs until it returns. What to wait for before touching them:
//
//      SAFE POINT. Let G be the generation returned by a Run() that has
//      returned. If IsQuiescent() returns true in a call made after that
//      Run() returned (same thread, or any thread ordered after it), then
//        (a) no chunk function of any generation <= G is executing, and none
//            will ever start (every chunk of those batches is Done or Late,
//            and a Late chunk that was not claimed in time is never run);
//        (b) every memory write made by any chunk function of generation <= G
//            happens-before the return of that IsQuiescent() call.
//      From then on the inputs and outputs of all chunks of those batches,
//      including the Late ones, may be reused or freed. The guarantee does not
//      cover batches started after the call (a worker may already be running
//      a chunk of a later Run(); IsQuiescent() then returns false).
//
//    StragglersInFlight() (audio thread, after Run()) counts workers that run a
//    chunk of a generation OLDER than the last Run's; a late chunk of the last
//    Run itself is not counted. It is for statistics / diagnostics only: core
//    decides with IsQuiescent() (contract 1.1, CONTRACTS.md D-43). Both
//    functions are conservative: a worker that is about to discover there is
//    nothing to run may make them report "busy" for a moment, never the opposite.
//  - Core's rules on top of this (D-43): two sets of render lists / chunk
//    accumulators; a block is rendered in parallel only when IsQuiescent() is
//    true at its start (both sets free), otherwise serially on the audio thread
//    into the set the last parallel block did not use (at most one set is in
//    use by a late chunk). Voices of a Late chunk stay held (schema
//    Voice.renderHold) and their set is reused only once IsQuiescent() is true.
//  - Stack: workers get at least platform::kRtThreadMinStackBytes (256 KiB,
//    D-47; RtWorkerConfig::minStackBytes).
//  - scratch is the calling thread's arena (Config.memory.threadArenaBytes),
//    reset when that thread starts its first chunk of a batch; it stays valid
//    until that thread joins a later batch, i.e. until the next Run().
#ifndef SND_JOBS_RT_WORKERS_HPP
#define SND_JOBS_RT_WORKERS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/arena.hpp"
#include "snd/base/atomic.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

namespace snd::jobs {

struct RtChunkContext {
  std::uint32_t chunkIndex;   // [0, chunkCount)
  std::uint32_t generation;   // batch generation (RenderBatch::chunkGeneration)
  std::uint32_t threadIndex;  // 0 = thread calling Run(), 1..workers = RT workers
  std::uint32_t reserved;
  source::Scratch* scratch;   // this thread's arena
};

using RtChunkFn = void (*)(void* ctx, const RtChunkContext& chunk) noexcept;

struct RtWorkerConfig {
  std::uint32_t workers = 2;              // Config.jobs.realtimeWorkers (0 = caller only)
  std::uint32_t maxChunks = 64;           // capacity of the chunk table per Run
  std::uint32_t spinMicroseconds = 50;    // Config.tuning.rtSpinMicroseconds
  std::size_t threadArenaBytes = 256u << 10;  // Config.memory.threadArenaBytes
  bool realtimePriority = true;           // workers ask for SCHED_FIFO (fallback reported)
  int realtimePriorityLevel = 80;
  bool flushDenormals = true;             // FTZ/DAZ on workers (2.4)
  // Lower bound of each worker's stack (D-47). Values below
  // platform::kRtThreadMinStackBytes are raised to it.
  std::size_t minStackBytes = 256u << 10;
};

struct RtRunResult {
  std::uint32_t chunks = 0;         // chunks requested (after clamping to maxChunks)
  std::uint32_t done = 0;           // finished in time
  std::uint32_t late = 0;           // not finished at the deadline (not in the output)
  std::uint32_t byCaller = 0;       // chunks run on the calling thread
  std::uint32_t generation = 0;     // batch generation of this run
  std::uint32_t clamped = 0;        // 1 if chunkCount exceeded maxChunks (extra chunks not run)
  std::uint64_t spinNanoseconds = 0;  // time spent waiting for in-flight chunks
};

class RtWorkerPool {
 public:
  RtWorkerPool() noexcept = default;
  ~RtWorkerPool() { Shutdown(); }
  RtWorkerPool(const RtWorkerPool&) = delete;
  RtWorkerPool& operator=(const RtWorkerPool&) = delete;

  // Non-RT (owner thread). Allocates (MemTag::ThreadArena / Misc) and starts
  // the workers. false on allocation or thread creation failure (nothing left running).
  bool Init(IAllocator& allocator, const RtWorkerConfig& config) noexcept;
  // Non-RT. Stops and joins the workers (waits for stragglers), frees memory.
  void Shutdown() noexcept;

  // Audio thread only (one caller at a time). RT-safe.
  RtRunResult Run(std::uint32_t chunkCount, RtChunkFn fn, void* ctx) noexcept;

  // Audio thread, after Run(): status of chunk i of the last run.
  bool ChunkDone(std::uint32_t chunk) const noexcept;
  // Scratch of the calling thread (threadIndex 0), e.g. for serial work after Run().
  source::Scratch& CallerScratch() noexcept { return scratch_[0]; }

  std::uint32_t Workers() const noexcept { return workerCount_; }
  std::uint32_t MaxChunks() const noexcept { return maxChunks_; }
  // Audio thread, after Run(): workers that may be executing (or about to
  // start) a chunk of a generation older than the last Run's. RT-safe.
  // Statistics / diagnostics only (D-43 uses IsQuiescent()).
  std::uint32_t StragglersInFlight() const noexcept;
  // Any thread: true when no worker executes or is about to start a chunk
  // (of any generation, the last Run's included). RT-safe. The predicate of
  // D-43 (parallel block, hold release, set reuse); see SAFE POINT above.
  bool IsQuiescent() const noexcept;
  platform::PriorityResult WorkerPriority(std::uint32_t worker) const noexcept;

  struct Stats {
    std::uint64_t runs;
    std::uint64_t chunks;
    std::uint64_t lateChunks;        // chunks marked Late (2.3: input to the load LOD)
    std::uint64_t discardedResults;  // late chunks that finished afterwards (discarded by generation)
    std::uint64_t chunksByWorkers;
    std::uint64_t clampedRuns;
  };
  Stats GetStats() const noexcept;

 private:
  struct Worker;
  static void WorkerMain(void* w) noexcept;
  void WorkerLoop(Worker& w) noexcept;
  // Claims and runs chunks of the current batch. Returns the number run.
  std::uint32_t Participate(std::uint32_t threadIndex, std::uint32_t& lastGeneration) noexcept;

  static constexpr std::uint64_t kPending = 0, kTaken = 1, kDone = 2, kLate = 3;

  IAllocator* alloc_ = nullptr;
  RtWorkerConfig config_{};
  std::uint32_t workerCount_ = 0;
  std::uint32_t maxChunks_ = 0;
  Worker* workers_ = nullptr;
  std::atomic<std::uint64_t>* chunkState_ = nullptr;  // (generation << 32) | state
  source::Scratch* scratch_ = nullptr;                // [workers + 1]
  base::ThreadArenaSet arenas_;
  std::uint32_t generation_ = 0;  // audio-thread private copy
  std::uint32_t lastCount_ = 0;
  std::uint32_t callerLastGeneration_ = 0;

  // Current batch (written by Run before publishing `cursor_`).
  base::CachePadded<std::atomic<std::uint64_t>> cursor_;  // (generation << 32) | next chunk
  std::atomic<RtChunkFn> fn_{nullptr};
  std::atomic<void*> ctx_{nullptr};
  std::atomic<std::uint32_t> count_{0};

  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> runs_{0}, chunks_{0}, late_{0}, discarded_{0}, byWorkers_{0}, clampedRuns_{0};
};

}  // namespace snd::jobs

#endif  // SND_JOBS_RT_WORKERS_HPP
