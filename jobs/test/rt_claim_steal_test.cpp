// A1-14 (stage 2): a worker that claimed a chunk number and stalled before
// marking the chunk Taken must not make the chunk Late: the caller runs it.
// The stall is reproduced with RtWorkerConfig::debugClaimDelayMicroseconds.
#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
struct Ctx {
  std::atomic<std::uint32_t> calls[4];
  std::atomic<std::uint32_t> byThread[4];
};
void Work(void* p, const RtChunkContext& c) noexcept {
  auto* ctx = static_cast<Ctx*>(p);
  ctx->calls[c.chunkIndex].fetch_add(1u, std::memory_order_relaxed);
  ctx->byThread[c.chunkIndex].store(c.threadIndex, std::memory_order_relaxed);
  if (c.chunkIndex == 0u) {  // keep the caller busy so that a worker claims chunk 1
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 2000000u;
    while (snd::platform::MonotonicNanoseconds() < until) {
    }
  }
}
}  // namespace

SND_TEST(RtClaimSteal, StalledClaimIsRunByTheCaller) {
  RtWorkerConfig cfg;
  cfg.workers = 1;
  cfg.maxChunks = 4;
  cfg.spinMicroseconds = 50;
  cfg.threadArenaBytes = 1024;
  cfg.realtimePriority = false;
  cfg.debugClaimDelayMicroseconds = 20000;  // 20 ms: far beyond the 50 us spin
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  std::uint32_t stolenRuns = 0;
  for (int run = 0; run < 10; ++run) {
    Ctx ctx{};
    RtRunResult r;
    {
      SND_RT_SCOPE();
      r = pool.Run(2, &Work, &ctx);
    }
    SND_CHECK_EQ(r.late, 0u);
    SND_CHECK_EQ(r.done, 2u);
    SND_CHECK(pool.ChunkDone(0) && pool.ChunkDone(1));
    if (r.stolen != 0u) {
      ++stolenRuns;
      SND_CHECK_EQ(ctx.byThread[1].load(), 0u);  // the caller ran it
    }
    // Wait for the stalled worker to give up its claim; it must never run the chunk.
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 1000000000ull;
    while (!pool.IsQuiescent() && snd::platform::MonotonicNanoseconds() < until) {
    }
    SND_CHECK(pool.IsQuiescent());
    SND_CHECK_EQ(ctx.calls[0].load(), 1u);
    SND_CHECK_EQ(ctx.calls[1].load(), 1u);
  }
  const RtWorkerPool::Stats st = pool.GetStats();
  snd::test::Note("runs with a stolen chunk: %u / 10, stats.chunksStolen=%llu", stolenRuns,
                  static_cast<unsigned long long>(st.chunksStolen));
  SND_CHECK(stolenRuns > 0u);
  SND_CHECK_EQ(st.chunksStolen, static_cast<std::uint64_t>(stolenRuns));
  SND_CHECK_EQ(st.lateChunks, 0u);
  pool.Shutdown();
}
