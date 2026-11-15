// FAILS (intermittently, a few blocks per 300k) on the current implementation:
// a defect in RtWorkerPool's in-flight counter (see the
// report). Participate() marks a chunk Done with a CAS and only afterwards
// does remaining_.fetch_sub(1). If the worker is preempted between the two,
// the audio thread can see the chunk Done in its deadline loop, return, and
// start the next Run(), which stores remaining_ = chunkCount; the delayed
// fetch_sub then lands on the NEW batch. Consequences for that block:
//   - remaining_ reaches 0 one chunk early: the bounded spin ends before its
//     limit and a chunk that would have finished in time is marked Late, or
//   - remaining_ wraps below 0 (0xFFFFFFFF) and the block spins its whole limit.
// Invariant checked: a block that reports Late chunks must have spun for the
// full limit (Late is only allowed "after the bounded spin", 2.3).
#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
void Work(void*, const RtChunkContext& c) noexcept {
  volatile std::uint32_t sink = 0;
  for (std::uint32_t i = 0; i < 50u + (c.chunkIndex & 3u) * 50u; ++i) sink = sink + i;
}
}  // namespace

SND_TEST(DefectRtRemaining, LateOnlyAfterTheFullSpin) {
  RtWorkerConfig cfg;
  cfg.workers = 8;  // oversubscribed: workers get preempted at random points
  cfg.maxChunks = 16;
  cfg.spinMicroseconds = 20;
  cfg.threadArenaBytes = 1024;
  cfg.realtimePriority = false;
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  const std::uint64_t limitNs = static_cast<std::uint64_t>(cfg.spinMicroseconds) * 1000u;
  std::uint64_t runs = 0, lateRuns = 0, earlyLate = 0;
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (snd::platform::MonotonicNanoseconds() - t0 < 8000000000ull) {
    const RtRunResult r = pool.Run(16, &Work, nullptr);
    ++runs;
    if (r.late == 0u) continue;
    ++lateRuns;
    if (r.spinNanoseconds < limitNs) {
      if (earlyLate < 5u) {
        snd::test::Note("block %u: late=%u done=%u spin=%llu ns", r.generation, r.late, r.done,
                        static_cast<unsigned long long>(r.spinNanoseconds));
      }
      ++earlyLate;
    }
  }
  pool.Shutdown();
  snd::test::Note("runs=%llu late runs=%llu late before the spin limit=%llu", static_cast<unsigned long long>(runs),
                  static_cast<unsigned long long>(lateRuns), static_cast<unsigned long long>(earlyLate));
  SND_CHECK_MSG(earlyLate == 0u, "%llu blocks marked chunks Late before spinning %llu ns",
                static_cast<unsigned long long>(earlyLate), static_cast<unsigned long long>(limitNs));
}
