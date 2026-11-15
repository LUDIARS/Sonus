// FAILS (intermittently) on the current implementation; kept failing on
// purpose, see the report.
//
// rt_workers.hpp: "The caller must keep the inputs and output buffers of a
// Late chunk untouched until IsQuiescent()". IsQuiescent() reads each
// worker's busyGeneration, but Participate() publishes it only AFTER the
// chunk was claimed (state CAS Pending -> Taken) and with a relaxed store:
//
//   worker                                   audio thread
//   CAS chunk[i]: Pending -> Taken (gen g)
//   <preempted>                              spin times out, chunk i -> Late
//                                            IsQuiescent(): busyGeneration == 0 -> true
//                                            reuses chunk i's buffers for block g+1
//   busyGeneration = g (relaxed)
//   fn(ctx, chunk i of g)  <- reads/writes buffers the caller already reused
//
// The test marks a generation "quiesced" once IsQuiescent() returned true
// after that block, and counts chunk functions of a quiesced generation that
// start afterwards. Any such call is a use of buffers the contract handed
// back to the caller. The window is a few instructions wide, so the test
// oversubscribes the CPUs (8 workers) and runs many blocks with a zero spin
// (every chunk a worker takes becomes Late).
#include <atomic>
#include <cstdint>
#include <thread>

#include "snd/base/allocator.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
struct Ctx {
  std::atomic<std::uint32_t> quiesced{0};  // every generation <= this is "returned to the caller"
  std::atomic<std::uint64_t> violations{0};
  std::atomic<std::uint64_t> calls{0};
};
void Fn(void* p, const RtChunkContext& c) noexcept {
  auto* x = static_cast<Ctx*>(p);
  x->calls.fetch_add(1, std::memory_order_relaxed);
  if (c.generation <= x->quiesced.load(std::memory_order_seq_cst)) x->violations.fetch_add(1);
  volatile std::uint32_t sink = 0;
  for (std::uint32_t i = 0; i < 200u; ++i) sink = sink + i;
}
}  // namespace

SND_TEST(DefectRtQuiescent, NoChunkOfAQuiescedGenerationStartsAfterIsQuiescent) {
  RtWorkerConfig cfg;
  cfg.workers = 8;
  cfg.maxChunks = 16;
  cfg.spinMicroseconds = 0;
  cfg.threadArenaBytes = 1024;
  cfg.realtimePriority = false;
  RtWorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Ctx ctx;
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  std::uint64_t runs = 0, lateRuns = 0;
  while (snd::platform::MonotonicNanoseconds() - t0 < 8000000000ull) {
    const RtRunResult r = pool.Run(16, &Fn, &ctx);
    ++runs;
    if (r.late == 0u) continue;
    ++lateRuns;
    // Wait as the contract says, then declare every generation so far reusable.
    while (!pool.IsQuiescent()) std::this_thread::yield();
    ctx.quiesced.store(r.generation, std::memory_order_seq_cst);
  }
  pool.Shutdown();
  snd::test::Note("runs=%llu with late chunks=%llu chunk calls=%llu violations=%llu",
                  static_cast<unsigned long long>(runs), static_cast<unsigned long long>(lateRuns),
                  static_cast<unsigned long long>(ctx.calls.load()), static_cast<unsigned long long>(ctx.violations.load()));
  SND_CHECK_MSG(ctx.violations.load() == 0u,
                "%llu chunk functions of an already quiesced generation started after IsQuiescent() returned true",
                static_cast<unsigned long long>(ctx.violations.load()));
}
