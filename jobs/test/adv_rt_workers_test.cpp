// Independent tests of the RT workers and the chunk deadline (design.md 2.3).
//
// Most tests use realtimePriority = false: a SCHED_FIFO thread that spins (as
// the chunk functions here do on purpose) could starve the machine when the
// container grants realtime priority. One test covers the priority request.
#include <sys/resource.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;
using snd::platform::MonotonicNanoseconds;

namespace {
constexpr std::uint64_t kMs = 1000000ull;

RtWorkerConfig Cfg(std::uint32_t workers, std::uint32_t maxChunks, std::uint32_t spinUs) {
  RtWorkerConfig c;
  c.workers = workers;
  c.maxChunks = maxChunks;
  c.spinMicroseconds = spinUs;
  c.threadArenaBytes = 4096;
  c.realtimePriority = false;
  return c;
}

struct Rng {
  std::uint64_t s;
  std::uint32_t Below(std::uint32_t n) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return n == 0u ? 0u : static_cast<std::uint32_t>(s % n);
  }
};

// --- exactly-once bookkeeping --------------------------------------------------------
struct OnceCtx {
  static constexpr std::uint32_t kMax = 80;
  std::atomic<std::uint32_t> runs[kMax];
  std::atomic<std::uint32_t> badGeneration{0};
  std::atomic<std::uint32_t> badThread{0};
  std::atomic<std::uint32_t> expectGen{0};
  std::uint32_t workers = 0;
  std::atomic<std::uint32_t> doneMark[kMax];
};

void OnceFn(void* p, const RtChunkContext& c) noexcept {
  auto* x = static_cast<OnceCtx*>(p);
  if (c.chunkIndex >= OnceCtx::kMax) return;
  x->runs[c.chunkIndex].fetch_add(1, std::memory_order_relaxed);
  if (c.generation != x->expectGen.load(std::memory_order_relaxed)) x->badGeneration.fetch_add(1);
  if (c.threadIndex > x->workers || c.scratch == nullptr) x->badThread.fetch_add(1);
  x->doneMark[c.chunkIndex].store(c.generation, std::memory_order_release);
}
}  // namespace

SND_TEST(AdvRtWorkers, ExactlyOnceForWorkerAndChunkCountMatrix) {
  constexpr std::uint32_t kMaxChunks = 64;
  for (std::uint32_t workers : {0u, 1u, 3u, 8u}) {
    RtWorkerPool pool;
    SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(workers, kMaxChunks, 5000000)));  // 5 s: never late
    SND_CHECK_EQ(pool.Workers(), workers);
    auto ctx = std::make_unique<OnceCtx>();
    ctx->workers = workers;
    std::uint64_t expectRuns = 0, expectChunks = 0, byCallerTotal = 0, clampedRuns = 0;
    for (std::uint32_t count : {0u, 1u, 2u, kMaxChunks - 1u, kMaxChunks, kMaxChunks + 3u}) {
      for (int rep = 0; rep < 40; ++rep) {
        for (auto& r : ctx->runs) r.store(0);
        RtRunResult r;
        const std::uint32_t nextGen = static_cast<std::uint32_t>(expectRuns + 1u);
        ctx->expectGen.store(nextGen);
        {
          SND_RT_SCOPE();
          r = pool.Run(count, &OnceFn, ctx.get());
        }
        ++expectRuns;
        const std::uint32_t eff = count > kMaxChunks ? kMaxChunks : count;
        expectChunks += eff;
        byCallerTotal += r.byCaller;
        clampedRuns += r.clamped;
        SND_REQUIRE_EQ(r.generation, nextGen);
        SND_REQUIRE_EQ(r.chunks, eff);
        SND_REQUIRE_EQ(r.done, eff);
        SND_REQUIRE_EQ(r.late, 0u);
        SND_REQUIRE_EQ(r.clamped, count > kMaxChunks ? 1u : 0u);
        SND_REQUIRE(r.byCaller <= eff);
        if (workers == 0u) SND_REQUIRE_EQ(r.byCaller, eff);
        for (std::uint32_t i = 0; i < OnceCtx::kMax; ++i) {
          SND_REQUIRE_EQ(ctx->runs[i].load(), i < eff ? 1u : 0u);  // clamped chunks never run
          SND_REQUIRE_EQ(pool.ChunkDone(i), i < eff);
          if (i < eff) SND_REQUIRE_EQ(ctx->doneMark[i].load(std::memory_order_acquire), nextGen);  // result visible
        }
      }
    }
    SND_CHECK_EQ(ctx->badGeneration.load(), 0u);
    SND_CHECK_EQ(ctx->badThread.load(), 0u);
    SND_CHECK(pool.IsQuiescent() || workers > 0u);
    const RtWorkerPool::Stats s = pool.GetStats();
    SND_CHECK_EQ(s.runs, expectRuns);
    SND_CHECK_EQ(s.chunks, expectChunks);
    SND_CHECK_EQ(s.lateChunks, 0u);
    SND_CHECK_EQ(s.discardedResults, 0u);
    SND_CHECK_EQ(s.clampedRuns, clampedRuns);
    // Worker-run chunks are counted after the worker returns to its loop; wait a moment.
    const std::uint64_t t0 = MonotonicNanoseconds();
    while (pool.GetStats().chunksByWorkers + byCallerTotal != expectChunks && MonotonicNanoseconds() - t0 < 2000 * kMs) {
      std::this_thread::yield();
    }
    SND_CHECK_EQ(pool.GetStats().chunksByWorkers + byCallerTotal, expectChunks);
    pool.Shutdown();
  }
}

SND_TEST(AdvRtWorkers, NullFunctionAndUninitialisedPool) {
  RtWorkerPool none;
  const RtRunResult r0 = none.Run(4, &OnceFn, nullptr);  // not initialised
  SND_CHECK_EQ(r0.chunks, 0u);
  SND_CHECK(!none.ChunkDone(0));
  SND_CHECK(none.IsQuiescent());
  none.Shutdown();
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(2, 8, 1000)));
  const RtRunResult r1 = pool.Run(4, nullptr, nullptr);
  SND_CHECK_EQ(r1.done, 0u);
  SND_CHECK_EQ(pool.WorkerPriority(5), snd::platform::PriorityResult::Failed);
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(1, 4, 1000)));  // re-Init shuts down first
  SND_CHECK_EQ(pool.Workers(), 1u);
  SND_CHECK_EQ(pool.MaxChunks(), 4u);
}

namespace {
struct BlockCtx {
  std::atomic<bool> blockArmed{true};
  std::atomic<bool> release{false};
  std::atomic<std::uint32_t> blockedGen{0}, blockedChunk{0xFFFFFFFFu};
  std::atomic<std::uint32_t> runs[16];
  std::atomic<std::uint32_t> genOf[16];
  std::atomic<std::uint32_t> stragglerFinished{0};
};

// The first chunk a WORKER takes blocks until released (never the caller:
// that would deadlock the test).
void BlockFn(void* p, const RtChunkContext& c) noexcept {
  auto* x = static_cast<BlockCtx*>(p);
  x->runs[c.chunkIndex].fetch_add(1);
  // The caller (threadIndex 0) holds its chunk until a worker has taken one.
  // Without this the caller often finishes a batch of trivial chunks before
  // any worker wakes up, and the test never gets its late chunk (it did not,
  // in about one run in four). Bounded so a pool without workers cannot hang.
  if (c.threadIndex == 0u) {
    const std::uint64_t t0 = MonotonicNanoseconds();
    while (x->blockArmed.load(std::memory_order_acquire) && MonotonicNanoseconds() - t0 < 2000 * kMs) {
      std::this_thread::yield();
    }
  }
  if (c.threadIndex != 0u && x->blockArmed.exchange(false)) {
    x->blockedGen.store(c.generation);
    x->blockedChunk.store(c.chunkIndex);
    while (!x->release.load(std::memory_order_acquire)) std::this_thread::yield();
    x->genOf[c.chunkIndex].store(c.generation);  // writes the (late) result after the deadline
    x->stragglerFinished.fetch_add(1);
    return;
  }
  x->genOf[c.chunkIndex].store(c.generation);
}
}  // namespace

SND_TEST(AdvRtWorkers, LateChunkThenSameChunkIndexInNextBlockAndDiscardCount) {
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(2, 16, 20000)));  // 20 ms deadline spin
  auto ctx = std::make_unique<BlockCtx>();
  RtRunResult late{};
  bool gotLate = false;
  for (int i = 0; i < 2000 && !gotLate; ++i) {
    for (auto& r : ctx->runs) r.store(0);
    late = pool.Run(8, &BlockFn, ctx.get());
    gotLate = late.late > 0u;
  }
  SND_REQUIRE(gotLate);
  const std::uint32_t L = ctx->blockedChunk.load();
  SND_REQUIRE(L < 8u);
  SND_CHECK_EQ(ctx->blockedGen.load(), late.generation);
  SND_CHECK_EQ(late.late, 1u);
  SND_CHECK_EQ(late.done, 7u);
  SND_CHECK(!pool.ChunkDone(L));
  SND_CHECK_EQ(pool.StragglersInFlight(), 0u);  // same generation: not a straggler yet
  SND_CHECK(!pool.IsQuiescent());
  // Next block: the same chunk index must run normally (the other worker or the caller).
  for (auto& r : ctx->runs) r.store(0);
  const RtRunResult next = pool.Run(8, &BlockFn, ctx.get());
  SND_CHECK_EQ(next.generation, late.generation + 1u);
  SND_CHECK_EQ(next.done, 8u);
  SND_CHECK_EQ(next.late, 0u);
  SND_CHECK_EQ(ctx->runs[L].load(), 1u);
  SND_CHECK(pool.ChunkDone(L));
  SND_CHECK_EQ(ctx->genOf[L].load(), next.generation);
  SND_CHECK_EQ(pool.StragglersInFlight(), 1u);  // the blocked worker runs an older generation
  // Release the straggler: its Done transition must fail (counted as discarded)
  // and must not turn this block's chunk L into anything else.
  ctx->release.store(true, std::memory_order_release);
  const std::uint64_t t0 = MonotonicNanoseconds();
  while ((!pool.IsQuiescent() || pool.GetStats().discardedResults == 0u) && MonotonicNanoseconds() - t0 < 5000 * kMs) {
    std::this_thread::yield();
  }
  SND_CHECK(pool.IsQuiescent());
  SND_CHECK_EQ(pool.StragglersInFlight(), 0u);
  SND_CHECK_EQ(ctx->stragglerFinished.load(), 1u);
  const RtWorkerPool::Stats s = pool.GetStats();
  SND_CHECK_EQ(s.lateChunks, 1u);
  SND_CHECK_EQ(s.discardedResults, 1u);
  SND_CHECK(pool.ChunkDone(L));  // still this block's result, not the straggler's
  // And the pool keeps working normally afterwards.
  for (int i = 0; i < 100; ++i) {
    const RtRunResult r = pool.Run(8, &BlockFn, ctx.get());
    SND_REQUIRE_EQ(r.done, 8u);
  }
}

SND_TEST(AdvRtWorkers, ShutdownWaitsForStragglerAndFinishesIdleWorkers) {
  {
    RtWorkerPool idle;
    SND_REQUIRE(idle.Init(snd::base::DefaultAllocator(), Cfg(4, 8, 1000)));
    const std::uint64_t t0 = MonotonicNanoseconds();
    idle.Shutdown();  // all workers blocked in Wait
    SND_CHECK(MonotonicNanoseconds() - t0 < 2000 * kMs);
    idle.Shutdown();  // idempotent
  }
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(3, 16, 2000)));
  auto ctx = std::make_unique<BlockCtx>();
  bool gotLate = false;
  for (int i = 0; i < 2000 && !gotLate; ++i) gotLate = pool.Run(8, &BlockFn, ctx.get()).late > 0u;
  SND_REQUIRE(gotLate);
  std::thread releaser([&] {
    snd::platform::SleepFor(100 * kMs);
    ctx->release.store(true, std::memory_order_release);
  });
  const std::uint64_t t0 = MonotonicNanoseconds();
  pool.Shutdown();  // a worker is inside a chunk: Shutdown must wait for it, then return
  const std::uint64_t dt = MonotonicNanoseconds() - t0;
  releaser.join();
  SND_CHECK_EQ(ctx->stragglerFinished.load(), 1u);
  SND_CHECK(dt >= 50 * kMs);
  SND_CHECK_EQ(pool.Workers(), 0u);
}

namespace {
struct ScratchCtx {
  std::uint32_t lastGen[16] = {};      // per threadIndex: written only by that thread
  std::size_t expectUsed[16] = {};
  std::atomic<std::uint32_t> bad{0};
  std::atomic<std::uint32_t> nulls{0};
};
void ScratchFn(void* p, const RtChunkContext& c) noexcept {
  auto* x = static_cast<ScratchCtx*>(p);
  const std::uint32_t t = c.threadIndex;
  if (x->lastGen[t] != c.generation) {  // first chunk of this batch on this thread
    if (c.scratch->used != 0u) x->bad.fetch_add(1);
    x->lastGen[t] = c.generation;
    x->expectUsed[t] = 0;
  } else if (c.scratch->used != x->expectUsed[t]) {
    x->bad.fetch_add(1);
  }
  void* m = c.scratch->Take(64);
  if (m == nullptr) {
    x->nulls.fetch_add(1);
  } else {
    std::memset(m, static_cast<int>(c.chunkIndex), 64);  // own memory (ASan/TSan: no overlap between threads)
    x->expectUsed[t] = c.scratch->used;
  }
}
}  // namespace

SND_TEST(AdvRtWorkers, ThreadScratchIsResetPerBatchAndPrivate) {
  RtWorkerPool pool;
  RtWorkerConfig cfg = Cfg(3, 64, 5000000);
  cfg.threadArenaBytes = 64u * 64u;  // exactly 64 Take(64) per thread
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  auto ctx = std::make_unique<ScratchCtx>();
  for (int i = 0; i < 3000; ++i) {
    const RtRunResult r = pool.Run(1u + static_cast<std::uint32_t>(i % 64), &ScratchFn, ctx.get());
    SND_REQUIRE_EQ(r.late, 0u);
  }
  SND_CHECK_EQ(ctx->bad.load(), 0u);
  SND_CHECK_EQ(ctx->nulls.load(), 0u);  // never more than 64 chunks per thread per batch
  SND_CHECK(pool.CallerScratch().bytes >= 64u * 64u);
}

namespace {
struct BarrierCtx {
  std::atomic<std::uint32_t> arrived{0};
  std::atomic<std::uint32_t> timeouts{0};
  std::uint32_t need = 0;
};
void BarrierFn(void* p, const RtChunkContext&) noexcept {
  auto* x = static_cast<BarrierCtx*>(p);
  x->arrived.fetch_add(1);
  const std::uint64_t t0 = MonotonicNanoseconds();
  while (x->arrived.load() < x->need) {
    if (MonotonicNanoseconds() - t0 > 500 * kMs) {
      x->timeouts.fetch_add(1);
      return;
    }
    std::this_thread::yield();
  }
}
void NopFn(void*, const RtChunkContext&) noexcept {}
double CpuSeconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
         static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}
}  // namespace

SND_TEST(AdvRtWorkers, EveryRunWakesTheWorkersItNeedsAndIdleWorkersSleep) {
  // chunkCount = workers + 1 and every chunk waits until all of them started:
  // a block completes only if all workers were woken for it (a lost wake-up
  // leaves an unclaimed chunk and both waiting chunks time out after 500 ms,
  // a bound no scheduling delay on a loaded machine should reach).
  constexpr std::uint32_t kWorkers = 2;
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(kWorkers, 8, 5000000)));
  auto ctx = std::make_unique<BarrierCtx>();
  ctx->need = kWorkers + 1u;
  constexpr int kRuns = 400;
  for (int i = 0; i < kRuns; ++i) {
    ctx->arrived.store(0);
    const RtRunResult r = pool.Run(kWorkers + 1u, &BarrierFn, ctx.get());
    SND_REQUIRE_EQ(r.done, kWorkers + 1u);
  }
  snd::test::Note("barrier timeouts: %u of %d blocks", ctx->timeouts.load(), kRuns);
  SND_CHECK(ctx->timeouts.load() <= static_cast<std::uint32_t>(kRuns / 100));
  // Idle: after the blocks, the workers sleep on their semaphores. 300 ms of
  // idle may cost at most 60 ms of CPU for the whole process (a worker
  // spinning on stale wake-ups would burn ~300 ms per thread).
  for (int i = 0; i < 2000; ++i) pool.Run(8, &NopFn, nullptr);
  snd::platform::SleepFor(50 * kMs);
  const double c0 = CpuSeconds();
  snd::platform::SleepFor(300 * kMs);
  const double used = CpuSeconds() - c0;
  snd::test::Note("idle CPU over 300 ms: %.1f ms", used * 1e3);
  SND_CHECK(used < 0.060);
}

namespace {
// Stress bookkeeping: one counter per (run, chunk); gen == run index + 1.
struct StressCtx {
  std::uint32_t runs = 0, maxChunks = 0;
  std::vector<std::atomic<std::uint8_t>> exec;      // fn calls per (run, chunk)
  std::vector<std::atomic<std::uint8_t>> finished;  // fn returned per (run, chunk)
  std::atomic<std::uint64_t> outOfRange{0};
  std::uint32_t spinIters[4] = {0, 10, 100, 1000};
};
void StressFn(void* p, const RtChunkContext& c) noexcept {
  auto* x = static_cast<StressCtx*>(p);
  if (c.generation == 0u || c.generation > x->runs || c.chunkIndex >= x->maxChunks) {
    x->outOfRange.fetch_add(1);
    return;
  }
  const std::size_t k = static_cast<std::size_t>(c.generation - 1u) * x->maxChunks + c.chunkIndex;
  x->exec[k].fetch_add(1, std::memory_order_relaxed);
  volatile std::uint32_t sink = 0;
  for (std::uint32_t i = 0; i < x->spinIters[(c.chunkIndex + c.generation) & 3u]; ++i) sink = sink + i;
  x->finished[k].store(1, std::memory_order_release);
}
}  // namespace

SND_TEST(AdvRtWorkers, StressRandomBlocksWithLatesExactAccounting) {
  constexpr std::uint32_t kMaxChunks = 16;
  const std::uint32_t kRuns = 20000;
  for (std::uint32_t spinUs : {0u, 30u}) {
    RtWorkerPool pool;
    SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Cfg(3, kMaxChunks, spinUs)));
    auto ctx = std::make_unique<StressCtx>();
    ctx->runs = kRuns;
    ctx->maxChunks = kMaxChunks;
    ctx->exec = std::vector<std::atomic<std::uint8_t>>(static_cast<std::size_t>(kRuns) * kMaxChunks);
    ctx->finished = std::vector<std::atomic<std::uint8_t>>(static_cast<std::size_t>(kRuns) * kMaxChunks);
    std::vector<std::uint8_t> wasDone(static_cast<std::size_t>(kRuns) * kMaxChunks, 0u);
    std::vector<std::uint32_t> counts(kRuns, 0u);
    Rng rng{spinUs + 12345u};
    std::uint64_t doneTotal = 0, lateTotal = 0, earlyFinishMissing = 0, resultMismatch = 0;
    for (std::uint32_t i = 0; i < kRuns; ++i) {
      const std::uint32_t count = rng.Below(kMaxChunks + 3u);
      RtRunResult r;
      {
        SND_RT_SCOPE();
        r = pool.Run(count, &StressFn, ctx.get());
      }
      SND_REQUIRE_EQ(r.generation, i + 1u);
      const std::uint32_t eff = count > kMaxChunks ? kMaxChunks : count;
      counts[i] = eff;
      SND_REQUIRE_EQ(r.done + r.late, eff);
      doneTotal += r.done;
      lateTotal += r.late;
      std::uint32_t doneSeen = 0;
      for (std::uint32_t c = 0; c < eff; ++c) {
        if (!pool.ChunkDone(c)) continue;
        ++doneSeen;
        const std::size_t k = static_cast<std::size_t>(i) * kMaxChunks + c;
        wasDone[k] = 1;
        // Done implies the chunk function returned and its writes are visible.
        if (ctx->finished[k].load(std::memory_order_acquire) != 1u) ++earlyFinishMissing;
      }
      if (doneSeen != r.done) ++resultMismatch;
    }
    // Let stragglers finish; Shutdown joins every worker.
    pool.Shutdown();
    std::uint64_t execTotal = 0, dup = 0, doneNotRun = 0, ranBeyond = 0;
    for (std::uint32_t i = 0; i < kRuns; ++i) {
      for (std::uint32_t c = 0; c < kMaxChunks; ++c) {
        const std::size_t k = static_cast<std::size_t>(i) * kMaxChunks + c;
        const std::uint32_t n = ctx->exec[k].load();
        execTotal += n;
        if (n > 1u) ++dup;
        if (wasDone[k] && n != 1u) ++doneNotRun;
        if (c >= counts[i] && n != 0u) ++ranBeyond;
      }
    }
    const RtWorkerPool::Stats s = pool.GetStats();
    snd::test::Note("spin=%u us: done=%llu late=%llu discarded=%llu", spinUs, static_cast<unsigned long long>(doneTotal),
                    static_cast<unsigned long long>(lateTotal), static_cast<unsigned long long>(s.discardedResults));
    SND_CHECK_EQ(dup, 0u);
    SND_CHECK_EQ(doneNotRun, 0u);
    SND_CHECK_EQ(ranBeyond, 0u);
    SND_CHECK_EQ(earlyFinishMissing, 0u);
    SND_CHECK_EQ(resultMismatch, 0u);
    SND_CHECK_EQ(ctx->outOfRange.load(), 0u);
    SND_CHECK_EQ(s.lateChunks, lateTotal);
    // Every fn call either produced a Done chunk or was a discarded late result.
    SND_CHECK_EQ(execTotal, doneTotal + s.discardedResults);
    SND_CHECK(s.discardedResults <= lateTotal);
  }
}

SND_TEST(AdvRtWorkers, RealtimePriorityRequestReportsResult) {
  RtWorkerPool pool;
  RtWorkerConfig cfg = Cfg(2, 4, 1000);
  cfg.realtimePriority = true;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  for (std::uint32_t w = 0; w < 2; ++w) {
    const auto r = pool.WorkerPriority(w);
    SND_CHECK(r == snd::platform::PriorityResult::Applied || r == snd::platform::PriorityResult::FellBack);
  }
  auto ctx = std::make_unique<OnceCtx>();
  ctx->workers = 2;
  ctx->expectGen.store(1);
  SND_CHECK_EQ(pool.Run(4, &OnceFn, ctx.get()).chunks, 4u);
}
