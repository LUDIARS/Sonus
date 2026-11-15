// Independent test (contract 1.1, D-43 as decided: IsQuiescent()).
//
// Rule under test (CONTRACTS.md D-43, rt_workers.hpp):
//  - render in parallel (Run) only in a block where IsQuiescent() is true;
//  - otherwise render serially on the audio thread into the set NOT used by
//    the last parallel block (the one a late chunk may still write);
//  - release held voices / reuse a set only once IsQuiescent() is true.
// StragglersInFlight() is statistics only (it never counts the last Run).
//
// Properties checked against the real RtWorkerPool:
//  P1  while a late chunk still runs, IsQuiescent() is false; whenever it is
//      true, every late chunk has returned and its writes are visible.
//  P2  a core loop that follows the rule never writes a set (render list /
//      accumulators, here plain non-atomic memory: TSan would flag a race)
//      while a late chunk writes it, and at most one set is ever in use by
//      late chunks - also with late chunks in many blocks in a row.
//  P3  StragglersInFlight() does not see a late chunk of the last Run (the
//      reason it is not the rule's predicate).
#include <atomic>
#include <cstdint>
#include <cstring>

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

void SpinNs(std::uint64_t ns) {
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + ns;
  while (snd::platform::MonotonicNanoseconds() < until) {
  }
}

// --- P1 / P3: one late chunk held by the test ---------------------------------------
struct Late {
  std::atomic<std::uint32_t> workerRunning{0};
  std::atomic<std::uint32_t> release{0};
  std::uint32_t written = 0;  // plain: the late chunk's output
  std::atomic<std::uint32_t> finished{0};
};

void LateChunk(void* p, const RtChunkContext& c) noexcept {
  auto* s = static_cast<Late*>(p);
  if (c.threadIndex == 0u) {
    const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 2000000000ull;
    while (s->workerRunning.load(std::memory_order_acquire) == 0u && snd::platform::MonotonicNanoseconds() < until) {
    }
    return;
  }
  s->workerRunning.store(1u, std::memory_order_release);
  while (s->release.load(std::memory_order_acquire) == 0u) ++s->written;
  s->written += 1000000u;
  s->finished.store(1u, std::memory_order_relaxed);  // relaxed: visibility must come from IsQuiescent()
}

// --- P2: a core loop over two sets --------------------------------------------------
constexpr std::uint32_t kChunks = 6;
constexpr std::uint32_t kWords = 64;

struct Set {
  std::uint32_t acc[kChunks][kWords];  // plain memory: chunks and the serial render write it
  std::atomic<std::int32_t> writers{0};
};

struct Core {
  Set sets[2];
  std::atomic<std::uint32_t> violations{0};
  std::uint32_t block = 0;
  std::uint32_t setOfRun = 0;  // set the current Run writes
};

void CoreChunk(void* p, const RtChunkContext& c) noexcept {
  auto* core = static_cast<Core*>(p);
  Set& set = core->sets[core->setOfRun];  // read before the claim: Run published it
  set.writers.fetch_add(1, std::memory_order_acq_rel);
  // Workers are sometimes slow enough to miss the deadline by up to ~3 blocks.
  const std::uint32_t mix = (c.chunkIndex * 2654435761u) ^ (c.generation * 40503u);
  const std::uint64_t ns = c.threadIndex == 0u ? 2000u : (mix % 7u == 0u ? 600000u + (mix % 5u) * 300000u : 20000u);
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + ns;
  std::uint32_t k = 0;
  while (snd::platform::MonotonicNanoseconds() < until) {
    set.acc[c.chunkIndex][k % kWords] += c.generation;  // a data race if anyone else touches the set now
    ++k;
  }
  set.writers.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace

SND_TEST(IndepD43, IsQuiescentIsFalseUntilTheLateChunkReturned) {
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.workers = 1;
  cfg.realtimePriority = false;
  cfg.spinMicroseconds = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Late s;
  RtRunResult r{};
  bool shown = false;
  for (int attempt = 0; attempt < 20 && !shown; ++attempt) {
    r = pool.Run(2, &LateChunk, &s);
    shown = r.late == 1u && s.workerRunning.load() != 0u && s.finished.load() == 0u;
    if (!shown) {
      s.release.store(1u);
      while (!pool.IsQuiescent()) {
      }
      s.release.store(0u);
      s.workerRunning.store(0u);
      s.finished.store(0u);
    }
  }
  SND_REQUIRE_MSG(shown, "could not produce a late chunk on the worker");
  // P3: the statistic does not see it; P1: the rule's predicate does.
  SND_CHECK_EQ(pool.StragglersInFlight(), 0u);
  for (int i = 0; i < 200; ++i) {
    SND_CHECK(!pool.IsQuiescent());
    SpinNs(5000);
  }
  s.release.store(1u, std::memory_order_release);
  const std::uint64_t giveUp = snd::platform::MonotonicNanoseconds() + 5000000000ull;
  while (!pool.IsQuiescent() && snd::platform::MonotonicNanoseconds() < giveUp) {
  }
  SND_REQUIRE(pool.IsQuiescent());
  // SAFE POINT (b): the late chunk's writes happen-before IsQuiescent() returning true.
  SND_CHECK_EQ(s.finished.load(std::memory_order_relaxed), 1u);
  SND_CHECK(s.written >= 1000000u);
  pool.Shutdown();
}

SND_TEST(IndepD43, CoreLoopFollowingTheRuleNeverTouchesASetInUse) {
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.workers = 3;
  cfg.realtimePriority = false;
  cfg.spinMicroseconds = 50;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  auto* core = new Core();
  std::memset(core->sets[0].acc, 0, sizeof(core->sets[0].acc));
  std::memset(core->sets[1].acc, 0, sizeof(core->sets[1].acc));
  std::uint32_t lastParallelSet = 1;  // the set the last Run wrote (a late chunk may still write it)
  std::uint32_t parallel = 0, serial = 0, lateBlocks = 0, lateInARow = 0, maxLateInARow = 0;
  std::uint32_t inUseViolations = 0, serialIntoBusy = 0, parallelWhileBusy = 0;
  const std::uint64_t blockNs = 250000;  // ~ a 256-frame block at 48 kHz, shortened
  for (std::uint32_t blk = 0; blk < 1500; ++blk) {
    const std::uint64_t start = snd::platform::MonotonicNanoseconds();
    // Sets with a writer right now (ground truth, not available to a real core).
    const bool busy0 = core->sets[0].writers.load(std::memory_order_acquire) != 0;
    const bool busy1 = core->sets[1].writers.load(std::memory_order_acquire) != 0;
    if (busy0 && busy1) ++inUseViolations;  // "at most one set in use"
    if (pool.IsQuiescent()) {
      // Parallel into the other set; both sets are free (and the holds can be released).
      if (busy0 || busy1) ++parallelWhileBusy;
      const std::uint32_t target = lastParallelSet ^ 1u;
      core->setOfRun = target;
      const RtRunResult r = pool.Run(kChunks, &CoreChunk, core);
      lastParallelSet = target;
      ++parallel;
      if (r.late != 0u) {
        ++lateBlocks;
        ++lateInARow;
      } else {
        lateInARow = 0;
      }
    } else {
      // Serial on the audio thread into the set the last Run did NOT write.
      const std::uint32_t target = lastParallelSet ^ 1u;
      Set& set = core->sets[target];
      if (set.writers.load(std::memory_order_acquire) != 0) ++serialIntoBusy;
      for (std::uint32_t c = 0; c < kChunks; ++c) {
        for (std::uint32_t w = 0; w < kWords; ++w) set.acc[c][w] += 1u;
      }
      ++serial;
      ++lateInARow;
    }
    if (lateInARow > maxLateInARow) maxLateInARow = lateInARow;
    while (snd::platform::MonotonicNanoseconds() - start < blockNs) {
    }
  }
  while (!pool.IsQuiescent()) {
  }
  SND_CHECK_EQ(inUseViolations, 0u);
  SND_CHECK_EQ(serialIntoBusy, 0u);
  SND_CHECK_EQ(parallelWhileBusy, 0u);
  SND_CHECK_MSG(lateBlocks > 0u && serial > 0u, "the case was not exercised (late %u, serial %u)", lateBlocks, serial);
  snd::test::Note("%u parallel, %u serial blocks, %u with late chunks, up to %u non-quiescent blocks in a row",
                  parallel, serial, lateBlocks, maxLateInARow);
  pool.Shutdown();
  delete core;
}
