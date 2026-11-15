#include "snd/jobs/rt_workers.hpp"

#include <new>

#include "snd/internal/rt_check.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/cpu.hpp"

namespace snd::jobs {

struct RtWorkerPool::Worker {
  RtWorkerPool* pool = nullptr;
  std::uint32_t index = 0;  // threadIndex (1..N)
  platform::Semaphore wake;
  platform::Thread thread;
  std::atomic<std::uint32_t> busyGeneration{0};  // generation of the chunk being run, 0 = idle
};

namespace {
constexpr std::uint64_t Word(std::uint32_t gen, std::uint64_t state) noexcept {
  return (static_cast<std::uint64_t>(gen) << 32) | state;
}
}  // namespace

bool RtWorkerPool::Init(IAllocator& allocator, const RtWorkerConfig& config) noexcept {
  Shutdown();
  if (config.maxChunks == 0u) return false;
  alloc_ = &allocator;
  config_ = config;
  maxChunks_ = config.maxChunks;
  const std::uint32_t threads = config.workers + 1u;
  chunkState_ = base::AllocateArray<std::atomic<std::uint64_t>>(allocator, maxChunks_, MemTag::Misc);
  scratch_ = base::AllocateArray<source::Scratch>(allocator, threads, MemTag::Misc);
  if (chunkState_ == nullptr || scratch_ == nullptr ||
      !arenas_.Init(allocator, threads, config.threadArenaBytes == 0u ? 64u : config.threadArenaBytes)) {
    Shutdown();
    return false;
  }
  for (std::uint32_t t = 0; t < threads; ++t) {
    base::LinearArena& a = arenas_.Arena(t);
    scratch_[t] = source::Scratch{a.data(), a.Capacity(), 0};
  }
  for (std::uint32_t i = 0; i < maxChunks_; ++i) chunkState_[i].store(Word(0, kLate), std::memory_order_relaxed);
  cursor_.value.store(0, std::memory_order_relaxed);
  count_.store(0, std::memory_order_relaxed);
  generation_ = 0;
  stop_.store(false, std::memory_order_relaxed);

  if (config.workers > 0u) {
    workers_ = base::AllocateArray<Worker>(allocator, config.workers, MemTag::Misc);
    if (workers_ == nullptr) {
      Shutdown();
      return false;
    }
    for (std::uint32_t w = 0; w < config.workers; ++w) {
      Worker& wk = workers_[w];
      wk.pool = this;
      wk.index = w + 1u;
      platform::ThreadDesc td;
      td.name = "snd-rt-worker";
      td.priority = config.realtimePriority ? platform::ThreadPriority::Realtime : platform::ThreadPriority::Normal;
      td.realtimePriority = config.realtimePriorityLevel;
      if (!wk.thread.Start(td, &RtWorkerPool::WorkerMain, &wk)) {
        workerCount_ = w;  // the ones already running are stopped by Shutdown
        Shutdown();
        return false;
      }
      workerCount_ = w + 1u;
    }
  }
  return true;
}

void RtWorkerPool::Shutdown() noexcept {
  if (workers_ != nullptr) {
    stop_.store(true, std::memory_order_release);
    for (std::uint32_t w = 0; w < workerCount_; ++w) workers_[w].wake.Post();
    for (std::uint32_t w = 0; w < workerCount_; ++w) workers_[w].thread.Join();
    base::FreeArray(*alloc_, workers_, config_.workers, MemTag::Misc);
    workers_ = nullptr;
  }
  workerCount_ = 0;
  if (alloc_ != nullptr) {
    base::FreeArray(*alloc_, chunkState_, maxChunks_, MemTag::Misc);
    base::FreeArray(*alloc_, scratch_, config_.workers + 1u, MemTag::Misc);
  }
  chunkState_ = nullptr;
  scratch_ = nullptr;
  arenas_.Destroy();
  maxChunks_ = 0;
  alloc_ = nullptr;
}

void RtWorkerPool::WorkerMain(void* p) noexcept {
  auto* w = static_cast<Worker*>(p);
  w->pool->WorkerLoop(*w);
}

void RtWorkerPool::WorkerLoop(Worker& w) noexcept {
  platform::FpControl saved{};
  if (config_.flushDenormals) saved = platform::SetFlushDenormals(true);
  std::uint32_t lastGeneration = 0;
  {
    SND_RT_SCOPE();  // everything an RT worker does obeys principle 1
    for (;;) {
      w.wake.Wait();  // pre-made semaphore: allowed (principle 1)
      if (stop_.load(std::memory_order_acquire)) break;
      const std::uint32_t n = Participate(w.index, lastGeneration);
      if (n != 0u) byWorkers_.fetch_add(n, std::memory_order_relaxed);
    }
  }
  if (config_.flushDenormals) platform::RestoreFpControl(saved);
}

std::uint32_t RtWorkerPool::Participate(std::uint32_t threadIndex, std::uint32_t& lastGeneration) noexcept {
  Worker* self = threadIndex > 0u ? &workers_[threadIndex - 1u] : nullptr;
  std::uint32_t ran = 0;
  std::uint64_t c = cursor_.value.load(std::memory_order_acquire);
  for (;;) {
    const std::uint32_t gen = static_cast<std::uint32_t>(c >> 32);
    const std::uint32_t next = static_cast<std::uint32_t>(c);
    // Read the batch before claiming: if the claim below succeeds, the batch
    // of `gen` is still running, so these are its values (Run publishes them
    // before the cursor with release).
    const RtChunkFn fn = fn_.load(std::memory_order_relaxed);
    void* const ctx = ctx_.load(std::memory_order_relaxed);
    const std::uint32_t count = count_.load(std::memory_order_relaxed);
    if (gen == 0u || next >= count || fn == nullptr) break;
    if (!cursor_.value.compare_exchange_weak(c, c + 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
      continue;  // c reloaded
    }
    // Announce "this worker may touch a chunk of `gen`" BEFORE the claim can
    // succeed. Ordering argument (IsQuiescent / StragglersInFlight):
    //   worker: busy = gen (seq_cst)  ->  CAS Pending->Taken (acq_rel, release)
    //   audio:  deadline CAS Taken->Late (acq_rel, reads the worker's Taken)
    //           ->  IsQuiescent loads busy (acquire)
    // If the claim succeeded, the audio thread's Late CAS read the Taken
    // value written by that release, so the earlier busy store happens-before
    // the IsQuiescent load: it sees `gen` (or the 0 stored after the chunk
    // function returned). If the deadline won (Pending->Late first), the claim
    // fails and the chunk function never runs. Either way IsQuiescent cannot
    // return true while a chunk function of a finished batch runs or is about to.
    if (self != nullptr) self->busyGeneration.store(gen, std::memory_order_seq_cst);
    std::atomic<std::uint64_t>& state = chunkState_[next];
    std::uint64_t expected = Word(gen, kPending);
    if (!state.compare_exchange_strong(expected, Word(gen, kTaken), std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
      if (self != nullptr) self->busyGeneration.store(0u, std::memory_order_release);
      c = cursor_.value.load(std::memory_order_acquire);
      continue;  // the deadline already marked it Late
    }
    if (lastGeneration != gen) {  // first chunk of this batch on this thread
      scratch_[threadIndex].used = 0;
      lastGeneration = gen;
    }
    const RtChunkContext rc{next, gen, threadIndex, 0u, &scratch_[threadIndex]};
    fn(ctx, rc);
    expected = Word(gen, kTaken);
    // Done is the only completion signal; the audio thread counts Done chunks
    // from the chunk words of ITS generation, so a delayed worker can never
    // affect the accounting of a later batch (no shared counter).
    if (state.compare_exchange_strong(expected, Word(gen, kDone), std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
      ++ran;
    } else {
      discarded_.fetch_add(1u, std::memory_order_relaxed);  // late result, generation moved on
    }
    // Release: the chunk function's writes happen-before an IsQuiescent() that sees 0.
    if (self != nullptr) self->busyGeneration.store(0u, std::memory_order_release);
    c = cursor_.value.load(std::memory_order_acquire);
  }
  return ran;
}

RtRunResult RtWorkerPool::Run(std::uint32_t chunkCount, RtChunkFn fn, void* ctx) noexcept {
  RtRunResult r;
  if (chunkState_ == nullptr || fn == nullptr) return r;
  if (chunkCount > maxChunks_) {
    r.clamped = 1u;
    clampedRuns_.fetch_add(1u, std::memory_order_relaxed);
    chunkCount = maxChunks_;
  }
  generation_ = generation_ + 1u == 0u ? 1u : generation_ + 1u;
  const std::uint32_t gen = generation_;
  r.generation = gen;
  r.chunks = chunkCount;
  lastCount_ = chunkCount;
  runs_.fetch_add(1u, std::memory_order_relaxed);
  if (chunkCount == 0u) return r;
  chunks_.fetch_add(chunkCount, std::memory_order_relaxed);

  for (std::uint32_t i = 0; i < chunkCount; ++i) chunkState_[i].store(Word(gen, kPending), std::memory_order_relaxed);
  fn_.store(fn, std::memory_order_relaxed);
  ctx_.store(ctx, std::memory_order_relaxed);
  count_.store(chunkCount, std::memory_order_relaxed);
  cursor_.value.store(Word(gen, 0u), std::memory_order_release);  // publish the batch

  const std::uint32_t wake = workerCount_ < chunkCount - 1u ? workerCount_ : chunkCount - 1u;
  for (std::uint32_t w = 0; w < wake; ++w) workers_[w].wake.Post();

  r.byCaller = Participate(0u, callerLastGeneration_);

  // No Pending chunk is left: spin for the ones in flight, bounded (2.3).
  // "All done" is read from this generation's chunk words (Done is final for
  // the generation), scanning forward from the first chunk not yet seen Done.
  std::uint32_t firstOpen = 0;
  auto allDone = [&]() noexcept {
    while (firstOpen < chunkCount && chunkState_[firstOpen].load(std::memory_order_acquire) == Word(gen, kDone)) {
      ++firstOpen;
    }
    return firstOpen == chunkCount;
  };
  if (!allDone()) {
    const std::uint64_t start = platform::MonotonicNanoseconds();
    const std::uint64_t limit = static_cast<std::uint64_t>(config_.spinMicroseconds) * 1000u;
    std::uint64_t now = start;
    while (!allDone() && now - start < limit) {
      for (int k = 0; k < 16; ++k) base::CpuRelax();
      now = platform::MonotonicNanoseconds();
    }
    r.spinNanoseconds = now - start;
  }

  // Deadline: everything not Done becomes Late (later completions fail their CAS).
  for (std::uint32_t i = 0; i < chunkCount; ++i) {
    std::uint64_t s = chunkState_[i].load(std::memory_order_acquire);
    while (s != Word(gen, kDone)) {
      if (chunkState_[i].compare_exchange_weak(s, Word(gen, kLate), std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        ++r.late;
        break;
      }
    }
    if (s == Word(gen, kDone)) ++r.done;
  }
  if (r.late != 0u) late_.fetch_add(r.late, std::memory_order_relaxed);
  return r;
}

bool RtWorkerPool::ChunkDone(std::uint32_t chunk) const noexcept {
  if (chunk >= lastCount_) return false;
  return chunkState_[chunk].load(std::memory_order_acquire) == Word(generation_, kDone);
}

std::uint32_t RtWorkerPool::StragglersInFlight() const noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t w = 0; w < workerCount_; ++w) {
    const std::uint32_t g = workers_[w].busyGeneration.load(std::memory_order_acquire);
    if (g != 0u && g != generation_) ++n;
  }
  return n;
}

bool RtWorkerPool::IsQuiescent() const noexcept {
  for (std::uint32_t w = 0; w < workerCount_; ++w) {
    if (workers_[w].busyGeneration.load(std::memory_order_acquire) != 0u) return false;
  }
  return true;
}

platform::PriorityResult RtWorkerPool::WorkerPriority(std::uint32_t worker) const noexcept {
  return worker < workerCount_ ? workers_[worker].thread.Priority() : platform::PriorityResult::Failed;
}

RtWorkerPool::Stats RtWorkerPool::GetStats() const noexcept {
  Stats s;
  s.runs = runs_.load(std::memory_order_relaxed);
  s.chunks = chunks_.load(std::memory_order_relaxed);
  s.lateChunks = late_.load(std::memory_order_relaxed);
  s.discardedResults = discarded_.load(std::memory_order_relaxed);
  s.chunksByWorkers = byWorkers_.load(std::memory_order_relaxed);
  s.clampedRuns = clampedRuns_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace snd::jobs
