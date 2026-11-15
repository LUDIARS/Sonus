// RtWorkerPool: chunk states, deadline, late results, RT safety (design.md 2.3).
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
constexpr std::uint32_t kMaxChunks = 32;
constexpr std::uint32_t kFrames = 64;

// Per-chunk accumulators owned by the test (as core's chunk accumulators would be).
struct RenderCtx {
  std::atomic<std::uint32_t> runs[kMaxChunks];
  float acc[kMaxChunks][kFrames];
  std::uint32_t generationSeen[kMaxChunks];
  std::atomic<std::uint32_t> byThread[8];
  std::atomic<bool> scratchBad{false};
  float base = 0.0f;
};

void RenderChunk(void* p, const RtChunkContext& c) noexcept {
  auto* r = static_cast<RenderCtx*>(p);
  r->runs[c.chunkIndex].fetch_add(1, std::memory_order_relaxed);
  if (c.threadIndex < 8u) r->byThread[c.threadIndex].fetch_add(1, std::memory_order_relaxed);
  auto* tmp = static_cast<float*>(c.scratch->Take(sizeof(float) * kFrames));
  if (tmp == nullptr) {
    r->scratchBad = true;
    return;
  }
  for (std::uint32_t i = 0; i < kFrames; ++i) tmp[i] = r->base + static_cast<float>(c.chunkIndex * 1000u + i);
  for (std::uint32_t i = 0; i < kFrames; ++i) r->acc[c.chunkIndex][i] = tmp[i];
  r->generationSeen[c.chunkIndex] = c.generation;
}

RtWorkerConfig Config(std::uint32_t workers, std::uint32_t spinUs) {
  RtWorkerConfig cfg;
  cfg.workers = workers;
  cfg.maxChunks = kMaxChunks;
  cfg.spinMicroseconds = spinUs;
  cfg.threadArenaBytes = 64u << 10;
  return cfg;
}
}  // namespace

SND_TEST(RtWorkers, AllChunksOnceAndResultsVisible) {
  snd::base::AccountingAllocator acc(snd::base::DefaultAllocator());
  {
    RtWorkerPool pool;
    SND_REQUIRE(pool.Init(acc, Config(2, 1000000)));  // 1 s spin: never late here
    SND_CHECK_EQ(pool.Workers(), 2u);
    snd::test::Note("rt worker priority: %s", snd::platform::PriorityResultName(pool.WorkerPriority(0)));
    auto* ctx = new RenderCtx();
    RtRunResult r;
    {
      SND_RT_SCOPE();  // the audio-thread side of the chunk render obeys principle 1
      r = pool.Run(20, &RenderChunk, ctx);
    }
    SND_CHECK_EQ(r.chunks, 20u);
    SND_CHECK_EQ(r.done, 20u);
    SND_CHECK_EQ(r.late, 0u);
    SND_CHECK_EQ(r.clamped, 0u);
    for (std::uint32_t c = 0; c < 20; ++c) {
      SND_CHECK_EQ(ctx->runs[c].load(), 1u);
      SND_CHECK(pool.ChunkDone(c));
      SND_CHECK_EQ(ctx->acc[c][kFrames - 1], static_cast<float>(c * 1000u + kFrames - 1u));
      SND_CHECK_EQ(ctx->generationSeen[c], r.generation);
    }
    SND_CHECK(!pool.ChunkDone(20));
    SND_CHECK(!ctx->scratchBad.load());
    const RtWorkerPool::Stats s = pool.GetStats();
    SND_CHECK_EQ(s.runs, 1u);
    SND_CHECK_EQ(s.chunks, 20u);
    SND_CHECK_EQ(s.chunksByWorkers + r.byCaller, 20u);
    delete ctx;
  }
  SND_CHECK_EQ(acc.TotalUsed(), 0u);
}

SND_TEST(RtWorkers, CallerOnlyAndClampAndEmpty) {
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Config(0, 50)));
  auto* ctx = new RenderCtx();
  {
  SND_RT_SCOPE();
  RtRunResult r = pool.Run(0, &RenderChunk, ctx);
  SND_CHECK_EQ(r.chunks, 0u);
  r = pool.Run(kMaxChunks + 5u, &RenderChunk, ctx);
  SND_CHECK_EQ(r.clamped, 1u);
  SND_CHECK_EQ(r.chunks, kMaxChunks);
  SND_CHECK_EQ(r.done, kMaxChunks);
  SND_CHECK_EQ(r.byCaller, kMaxChunks);
  SND_CHECK_EQ(pool.GetStats().clampedRuns, 1u);
  // The caller's scratch is reset per batch: many runs never exhaust it.
  for (int i = 0; i < 100; ++i) pool.Run(kMaxChunks, &RenderChunk, ctx);
  SND_CHECK(!ctx->scratchBad.load());
  SND_CHECK(pool.CallerScratch().used <= kMaxChunks * (sizeof(float) * kFrames + 64u));
  }
  delete ctx;
}

namespace {
// Chunks run by a worker block until `release`; the caller waits (bounded)
// until a worker has started one, so the deadline must expire for it.
struct LateCtx {
  std::atomic<bool> release{false};
  std::atomic<std::uint32_t> workerChunks{0};
  std::atomic<std::uint32_t> finished{0};
  std::atomic<std::uint32_t> blocking{1};  // how many worker chunks still block
};
void LateChunk(void* p, const RtChunkContext& c) noexcept {
  auto* l = static_cast<LateCtx*>(p);
  if (c.threadIndex != 0u) {
    l->workerChunks.fetch_add(1);
    std::uint32_t b = l->blocking.load();
    if (b > 0u && l->blocking.compare_exchange_strong(b, b - 1u)) {
      while (!l->release.load(std::memory_order_acquire)) snd::base::CpuRelax();
    }
  } else {
    const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
    while (l->workerChunks.load() == 0u && snd::platform::MonotonicNanoseconds() - t0 < 2000000000ull) {
      snd::base::CpuRelax();
    }
  }
  l->finished.fetch_add(1);
}
}  // namespace

SND_TEST(RtWorkers, LateChunkIsReportedAndItsResultDiscarded) {
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Config(1, 200)));
  LateCtx l;
  RtRunResult r;
  {
    SND_RT_SCOPE();
    r = pool.Run(8, &LateChunk, &l);
  }
  SND_REQUIRE(l.workerChunks.load() >= 1u);  // the worker did wake up and take a chunk
  SND_CHECK_EQ(r.late, 1u);
  SND_CHECK_EQ(r.done, 7u);
  SND_CHECK(r.spinNanoseconds >= 200000u);
  std::uint32_t notDone = 0;
  for (std::uint32_t c = 0; c < 8; ++c) notDone += pool.ChunkDone(c) ? 0u : 1u;
  SND_CHECK_EQ(notDone, 1u);
  SND_CHECK(!pool.IsQuiescent());
  SND_CHECK_EQ(pool.GetStats().lateChunks, 1u);

  // Next block while the straggler still runs: the worker is busy, the caller does everything.
  LateCtx l2;
  l2.blocking = 0;
  l2.workerChunks = 1;  // do not wait for a worker
  {
    SND_RT_SCOPE();
    const RtRunResult r2 = pool.Run(4, &LateChunk, &l2);
    SND_CHECK_EQ(r2.done, 4u);
    SND_CHECK_EQ(r2.late, 0u);
  }
  SND_CHECK_EQ(pool.StragglersInFlight(), 1u);

  l.release = true;
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (!pool.IsQuiescent() && snd::platform::MonotonicNanoseconds() - t0 < 5000000000ull) std::this_thread::yield();
  SND_CHECK(pool.IsQuiescent());
  SND_CHECK_EQ(pool.GetStats().discardedResults, 1u);  // finished after its generation ended
  SND_CHECK_EQ(l.finished.load(), 8u);
  SND_CHECK_EQ(pool.StragglersInFlight(), 0u);
}

SND_TEST(RtWorkers, ManyBlocksUnderLoad) {
  // Stress for ThreadSanitizer: many short batches; every Done chunk must hold
  // this block's data, every chunk is either done or late, never both.
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), Config(3, 20000)));
  auto* ctx = new RenderCtx();
  std::uint64_t done = 0, late = 0;
  bool bad = false;
  for (int block = 0; block < 1500; ++block) {
    ctx->base = static_cast<float>(block % 7);
    const std::uint32_t chunks = 1u + static_cast<std::uint32_t>(block % 24);
    RtRunResult r;
    {
      SND_RT_SCOPE();
      r = pool.Run(chunks, &RenderChunk, ctx);
    }
    if (r.done + r.late != chunks) bad = true;
    done += r.done;
    late += r.late;
    for (std::uint32_t c = 0; c < chunks; ++c) {
      if (!pool.ChunkDone(c)) continue;
      if (ctx->generationSeen[c] != r.generation) bad = true;
      if (ctx->acc[c][3] != ctx->base + static_cast<float>(c * 1000u + 3u)) bad = true;
    }
    if (r.late != 0u) {
      // Keep the test's buffers safe from stragglers (see the RtChunkFn contract).
      while (!pool.IsQuiescent()) std::this_thread::yield();
    }
  }
  SND_CHECK(!bad);
  SND_CHECK(!ctx->scratchBad.load());
  const RtWorkerPool::Stats s = pool.GetStats();
  snd::test::Note("blocks=1500 chunks=%llu late=%llu byWorkers=%llu", static_cast<unsigned long long>(s.chunks),
                  static_cast<unsigned long long>(s.lateChunks), static_cast<unsigned long long>(s.chunksByWorkers));
  SND_CHECK_EQ(s.lateChunks, late);
  SND_CHECK_EQ(done + late, s.chunks);
  SND_CHECK(s.chunksByWorkers > 0u);
  pool.Shutdown();
  delete ctx;
}

SND_TEST(RtWorkers, InitRejectsZeroChunks) {
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.maxChunks = 0;
  SND_CHECK(!pool.Init(snd::base::DefaultAllocator(), cfg));
  RtRunResult r = pool.Run(3, &RenderChunk, nullptr);  // not initialised: no-op
  SND_CHECK_EQ(r.chunks, 0u);
}
