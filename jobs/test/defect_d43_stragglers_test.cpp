// Independent test (contract 1.1, D-43 vs rt_workers.hpp SAFE POINT).
//
// D-43 (CONTRACTS.md, source_if.hpp, common.hpp VoiceRenderHold) uses
// `RtWorkerPool::StragglersInFlight() == 0` as "the worker of the late chunk is
// quiescent": the hold of the late chunk's voices is released and the set of
// render lists / accumulators it wrote is reused once it reads 0.
//
// rt_workers.hpp only guarantees that for generations OLDER than the last
// Run(); a late chunk of the LAST Run is never counted. So right after the
// block that produced the late chunk (and in every following block that renders
// serially without calling Run()), StragglersInFlight() reads 0 while the late
// chunk function is still reading its inputs and writing its outputs.
//
// Defect_D43_*: fails on purpose. It applies D-43's rule literally and shows
// that the rule's predicate is true while the late chunk still runs.
// Indep_D43_*: what the SAFE POINT does give (IsQuiescent, or a later Run).
#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

#define SND_REQUIRE_MSG(cond, ...)      \
  do {                                  \
    if (!(cond)) {                      \
      SND_CHECK_MSG(cond, __VA_ARGS__); \
      return;                           \
    }                                   \
  } while (0)

namespace {

// Chunk 0 runs on the caller and waits (bounded) until a worker took chunk 1.
// Chunk 1 runs on a worker and blocks until the test releases it, i.e. it
// misses the deadline and keeps running after Run() returned.
struct Late {
  std::atomic<std::uint32_t> workerRunning{0};
  std::atomic<std::uint32_t> release{0};
  std::atomic<std::uint32_t> workerWrites{0};  // "outputs" the late chunk keeps writing
  std::atomic<std::uint32_t> finished{0};
};

void LateChunk(void* p, const RtChunkContext& c) noexcept {
  auto* s = static_cast<Late*>(p);
  if (c.threadIndex == 0u) {
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 2000000000ull;  // 2 s
    while (s->workerRunning.load(std::memory_order_acquire) == 0u && snd::platform::MonotonicNanoseconds() < until) {
    }
    return;
  }
  s->workerRunning.store(1u, std::memory_order_release);
  while (s->release.load(std::memory_order_acquire) == 0u) {
    s->workerWrites.fetch_add(1u, std::memory_order_relaxed);
  }
  s->finished.store(1u, std::memory_order_release);
}

bool InitPool(RtWorkerPool& pool) {
  RtWorkerConfig cfg;
  cfg.workers = 1;
  cfg.realtimePriority = false;
  cfg.spinMicroseconds = 1;  // the worker's chunk misses the deadline
  return pool.Init(snd::base::DefaultAllocator(), cfg);
}

// Runs one block whose worker chunk is late and still running. false if the
// machine did not let the worker take its chunk (then the case is not shown).
bool LateBlock(RtWorkerPool& pool, Late& s, RtRunResult* r, std::uint32_t chunks = 2) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    *r = pool.Run(chunks, &LateChunk, &s);
    if (r->late == 1u && s.workerRunning.load(std::memory_order_acquire) != 0u &&
        s.finished.load(std::memory_order_acquire) == 0u) {
      return true;
    }
    s.release.store(1u, std::memory_order_release);
    while (s.workerRunning.load() != 0u && s.finished.load() == 0u) {
    }
    s.release.store(0u);
    s.workerRunning.store(0u);
    s.finished.store(0u);
  }
  return false;
}

}  // namespace

// D-43 literally: after the block with the late chunk, the audio thread asks
// StragglersInFlight(); 0 means "quiescent" (release the hold, reuse the set).
SND_TEST(D43, Defect_D43_StragglersInFlightZeroWhileTheLateChunkStillRuns) {
  RtWorkerPool pool;
  SND_REQUIRE(InitPool(pool));
  Late s;
  RtRunResult r;
  SND_REQUIRE_MSG(LateBlock(pool, s, &r), "could not produce a late chunk on the worker");
  // Next block, serial (no Run() yet) - or the hold check of D-43 (3).
  const std::uint32_t stragglers = pool.StragglersInFlight();
  const bool stillRunning = s.finished.load(std::memory_order_acquire) == 0u;
  const std::uint32_t before = s.workerWrites.load(std::memory_order_relaxed);
  // A second look a little later: the late chunk is still writing.
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 2000000u;  // 2 ms
  while (snd::platform::MonotonicNanoseconds() < until) {
  }
  const std::uint32_t after = s.workerWrites.load(std::memory_order_relaxed);
  SND_CHECK_MSG(!(stragglers == 0u && stillRunning),
                "D-43 says the late chunk's worker is quiescent when StragglersInFlight() == 0, but it is 0 while "
                "the late chunk of the last Run (generation %u) still runs (it wrote %u more times meanwhile). "
                "Releasing Voice.renderHold or reusing that chunk's render list / accumulators now is a data race.",
                r.generation, after - before);
  SND_CHECK(!pool.IsQuiescent());  // the SAFE POINT predicate does see it
  s.release.store(1u, std::memory_order_release);
  while (!pool.IsQuiescent()) {
  }
  pool.Shutdown();
}

// Two late blocks in a row (D-43 (2): "both sets in use"): after the second
// Run, the set of the FIRST late block is visible to StragglersInFlight(), the
// set of the second is not. D-43 tells the audio thread to render serially
// into "the set not in use", but the only set StragglersInFlight() reports as
// free is the one the second late chunk is still writing.
SND_TEST(D43, Defect_D43_BothSetsInUseIsNotObservable) {
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.workers = 2;
  cfg.realtimePriority = false;
  cfg.spinMicroseconds = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Late a, b;
  RtRunResult ra, rb;
  SND_REQUIRE_MSG(LateBlock(pool, a, &ra), "could not produce the first late chunk");  // set A
  // D-43 (2): set B (generation G-1, unused) may be reused: StragglersInFlight() == 0.
  // 3 chunks wake both workers; the first one is still stuck in set A.
  SND_REQUIRE_MSG(LateBlock(pool, b, &rb, 3), "could not produce the second late chunk");  // set B
  // Block 3 wants set A again: busy (the first late chunk is an older generation)...
  const std::uint32_t seen = pool.StragglersInFlight();
  // ... so D-43 renders serially into "the set not in use". Which one is that?
  const bool bStillWritten = b.finished.load(std::memory_order_acquire) == 0u;
  const bool aStillWritten = a.finished.load(std::memory_order_acquire) == 0u;
  SND_CHECK_MSG(!(aStillWritten && bStillWritten && seen == 1u),
                "both sets are being written by late chunks (generations %u and %u) but StragglersInFlight() = %u "
                "only counts the first; the contract gives no set to render the serial block into",
                ra.generation, rb.generation, seen);
  a.release.store(1u, std::memory_order_release);
  b.release.store(1u, std::memory_order_release);
  while (!pool.IsQuiescent()) {
  }
  pool.Shutdown();
}
