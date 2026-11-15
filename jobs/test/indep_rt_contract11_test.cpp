// Independent tests of the RT worker pool against contract 1.1 (D-43, D-47).
//  - D-47: RT workers get >= 256 KiB of stack even when the process default
//    stack is small, and a chunk can really use ~200 KiB of it.
//  - D-43 / SAFE POINT: what does detect a late chunk of the last Run:
//    IsQuiescent(), or StragglersInFlight() after any later Run() (Run(0)
//    starts a new generation too). Remedies for the D-43 wording defect
//    (Defect_jobs_d43_stragglers_test).
#include <atomic>
#include <cstdint>
#include <cstring>

#include "snd/base/allocator.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/thread.hpp"
#include "snd/test/test.hpp"

#if defined(__linux__)
#include <pthread.h>
#endif

using namespace snd::jobs;

namespace {

struct Late {
  std::atomic<std::uint32_t> workerRunning{0};
  std::atomic<std::uint32_t> release{0};
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
  while (s->release.load(std::memory_order_acquire) == 0u) {
  }
  s->finished.store(1u, std::memory_order_release);
}

void Nothing(void*, const RtChunkContext&) noexcept {}

}  // namespace

SND_TEST(IndepRt11, LateChunkOfTheLastRunIsSeenByIsQuiescentAndAfterALaterRun) {
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.workers = 1;
  cfg.realtimePriority = false;
  cfg.spinMicroseconds = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Late s;
  RtRunResult r;
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
  SND_REQUIRE(shown);
  SND_CHECK(!pool.IsQuiescent());
  // A later Run (even with no chunk) makes the late chunk an older generation.
  const RtRunResult empty = pool.Run(0, &Nothing, nullptr);
  SND_CHECK_EQ(empty.generation, r.generation + 1u);
  SND_CHECK_EQ(pool.StragglersInFlight(), 1u);
  s.release.store(1u, std::memory_order_release);
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 5000000000ull;
  while ((pool.StragglersInFlight() != 0u || !pool.IsQuiescent()) && snd::platform::MonotonicNanoseconds() < until) {
  }
  SND_CHECK_EQ(pool.StragglersInFlight(), 0u);
  SND_CHECK(pool.IsQuiescent());
  SND_CHECK_EQ(s.finished.load(), 1u);
  const RtWorkerPool::Stats st = pool.GetStats();
  SND_CHECK(st.lateChunks >= 1u);
  SND_CHECK(st.discardedResults >= 1u);  // the late result failed its Done CAS
  pool.Shutdown();
}

#if defined(__linux__)
namespace {
std::size_t StackOfSelf() {
  pthread_attr_t a;
  if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
  std::size_t bytes = 0;
  pthread_attr_getstacksize(&a, &bytes);
  pthread_attr_destroy(&a);
  return bytes;
}

std::size_t StackOf(pthread_t t) {
  pthread_attr_t a;
  if (pthread_getattr_np(t, &a) != 0) return 0;
  std::size_t bytes = 0;
  pthread_attr_getstacksize(&a, &bytes);
  pthread_attr_destroy(&a);
  return bytes;
}

struct StackProbe {
  std::atomic<std::uint32_t> ran{0};
  pthread_t ids[64];
  std::atomic<std::uint32_t> used[64];
  std::atomic<std::uint32_t> sum{0};
};

// Uses ~192 KiB of stack (an array the compiler cannot drop), like the sinc
// resampler's 32 KiB table plus effects / source scratch (D-47).
__attribute__((noinline)) std::uint32_t UseStack(std::uint32_t seed) {
  volatile std::uint8_t big[192u * 1024u];
  for (std::size_t i = 0; i < sizeof(big); i += 512) big[i] = static_cast<std::uint8_t>(seed + i);
  std::uint32_t s = 0;
  for (std::size_t i = 0; i < sizeof(big); i += 4096) s += big[i];
  return s;
}

void StackChunk(void* p, const RtChunkContext& c) noexcept {
  auto* s = static_cast<StackProbe*>(p);
  if (c.threadIndex == 0u) return;
  s->sum.fetch_add(UseStack(c.chunkIndex));  // survives: the stack is big enough
  const std::uint32_t i = s->ran.fetch_add(1u);
  if (i < 64u) {
    s->ids[i] = pthread_self();  // the size is read on the test thread (pthread_getattr_np allocates)
    s->used[i].store(1u, std::memory_order_release);
  }
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 100000u;
  while (snd::platform::MonotonicNanoseconds() < until) {
  }
}
}  // namespace

// With a process default stack of 64 KiB (as on some platforms / for threads
// of hosts that lower it), the workers still get the contract's 256 KiB.
SND_TEST(IndepRt11, WorkerStackIsRaisedAboveASmallProcessDefault) {
  pthread_attr_t old;
  SND_REQUIRE(pthread_getattr_default_np(&old) == 0);
  pthread_attr_t small;
  pthread_attr_init(&small);
  SND_REQUIRE(pthread_attr_setstacksize(&small, 64u * 1024u) == 0);
  SND_REQUIRE(pthread_setattr_default_np(&small) == 0);
  RtWorkerPool pool;
  RtWorkerConfig cfg;
  cfg.workers = 2;
  cfg.realtimePriority = false;
  cfg.minStackBytes = 0;  // even when the caller asks for less
  const bool ok = pool.Init(snd::base::DefaultAllocator(), cfg);
  pthread_setattr_default_np(&old);  // restore before any check can return
  pthread_attr_destroy(&small);
  pthread_attr_destroy(&old);
  SND_REQUIRE(ok);
  StackProbe probe;
  const std::uint64_t giveUp = snd::platform::MonotonicNanoseconds() + 10000000000ull;
  while (probe.ran.load() < 2u && snd::platform::MonotonicNanoseconds() < giveUp) pool.Run(8, &StackChunk, &probe);
  while (!pool.IsQuiescent()) {
  }
  SND_REQUIRE_EQ(probe.ran.load() >= 1u, true);
  const std::uint32_t n = probe.ran.load() < 64u ? probe.ran.load() : 64u;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (probe.used[i].load(std::memory_order_acquire) == 0u) continue;
    const std::size_t got = StackOf(probe.ids[i]);  // workers live until Shutdown
    SND_CHECK_MSG(got >= snd::platform::kRtThreadMinStackBytes, "worker stack %zu < %zu", got,
                  snd::platform::kRtThreadMinStackBytes);
  }
  pool.Shutdown();
}

// platform::Thread itself: a small explicit stackBytes is raised to minStackBytes.
SND_TEST(IndepRt11, ThreadDescMinimumWinsOverSmallStackBytes) {
  struct P {
    static void Main(void* u) { *static_cast<std::size_t*>(u) = StackOfSelf(); }
  };
  std::size_t got = 0;
  snd::platform::Thread t;
  snd::platform::ThreadDesc d;
  d.stackBytes = 32u * 1024u;
  d.minStackBytes = snd::platform::kRtThreadMinStackBytes;
  SND_REQUIRE(t.Start(d, &P::Main, &got));
  t.Join();
  SND_CHECK_MSG(got >= snd::platform::kRtThreadMinStackBytes, "stack %zu", got);
}
#endif
