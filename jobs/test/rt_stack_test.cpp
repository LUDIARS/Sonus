// Contract 1.1 D-47: RT workers get at least 256 KiB of stack.
#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/jobs/rt_workers.hpp"
#include "snd/platform/clock.hpp"
#include "snd/platform/thread.hpp"
#include "snd/test/test.hpp"

#if defined(__linux__)
#include <pthread.h>
#endif

using namespace snd::jobs;

#if defined(__linux__)
namespace {
struct Seen {
  std::atomic<std::uint32_t> count{0};
  pthread_t ids[64];
  std::atomic<std::uint32_t> flags[64];
};

void Record(void* p, const RtChunkContext& c) noexcept {
  auto* s = static_cast<Seen*>(p);
  if (c.threadIndex == 0u) return;
  const std::uint32_t i = s->count.fetch_add(1, std::memory_order_relaxed);
  if (i < 64u) {
    s->ids[i] = pthread_self();  // no allocation: the stack size is read on the test thread
    s->flags[i].store(1u, std::memory_order_release);
  }
  // Spend a little time so that the workers take chunks too.
  const std::uint64_t until = snd::platform::MonotonicNanoseconds() + 200000u;  // 200 us
  while (snd::platform::MonotonicNanoseconds() < until) {
  }
}

std::size_t StackOf(pthread_t t) {
  pthread_attr_t a;
  if (pthread_getattr_np(t, &a) != 0) return 0;
  std::size_t bytes = 0;
  pthread_attr_getstacksize(&a, &bytes);
  pthread_attr_destroy(&a);
  return bytes;
}
}  // namespace

SND_TEST(RtStack, WorkersHaveAtLeastTheContractStack) {
  static_assert(snd::platform::kRtThreadMinStackBytes == 256u * 1024u);
  for (std::size_t asked : {std::size_t{0}, std::size_t{64u << 10}, std::size_t{1u << 20}}) {
    RtWorkerPool pool;
    RtWorkerConfig cfg;
    cfg.workers = 2;
    cfg.realtimePriority = false;
    cfg.minStackBytes = asked;
    SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
    Seen seen;
    // Under load the caller may take every chunk for a while: retry up to 10 s.
    const std::uint64_t giveUp = snd::platform::MonotonicNanoseconds() + 10000000000ull;
    while (seen.count.load() == 0u && snd::platform::MonotonicNanoseconds() < giveUp) pool.Run(16, &Record, &seen);
    while (!pool.IsQuiescent()) {
    }
    const std::uint32_t n = seen.count.load() < 64u ? seen.count.load() : 64u;
    SND_CHECK_MSG(n > 0u, "no chunk ran on a worker");
    const std::size_t want = asked > snd::platform::kRtThreadMinStackBytes ? asked : snd::platform::kRtThreadMinStackBytes;
    for (std::uint32_t i = 0; i < n; ++i) {
      if (seen.flags[i].load(std::memory_order_acquire) == 0u) continue;
      const std::size_t got = StackOf(seen.ids[i]);  // workers are alive until Shutdown
      SND_CHECK_MSG(got >= want, "worker stack %zu < %zu", got, want);
    }
    pool.Shutdown();
  }
}

SND_TEST(RtStack, ThreadMinStackRaisesSmallRequests) {
  struct Probe {
    static void Main(void* p) {
      std::size_t* out = static_cast<std::size_t*>(p);
      *out = StackOf(pthread_self());
    }
  };
  for (std::size_t stack : {std::size_t{0}, std::size_t{64u << 10}, std::size_t{2u << 20}}) {
    std::size_t got = 0;
    snd::platform::Thread t;
    snd::platform::ThreadDesc d;
    d.name = "snd-stack-probe";
    d.stackBytes = stack;
    d.minStackBytes = snd::platform::kRtThreadMinStackBytes;
    SND_REQUIRE(t.Start(d, &Probe::Main, &got));
    t.Join();
    SND_CHECK_MSG(got >= snd::platform::kRtThreadMinStackBytes, "stackBytes %zu: got %zu", stack, got);
    if (stack > snd::platform::kRtThreadMinStackBytes) SND_CHECK(got >= stack);
  }
}
#else
SND_TEST(RtStack, NotLinux) { SND_CHECK(true); }
#endif
