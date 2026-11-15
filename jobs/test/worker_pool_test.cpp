// WorkerPool specifics: priorities within a lane, lanes, work stealing,
// shutdown semantics, RT misuse detection (design.md 2.3).
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/base/mutex.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
struct Gate {
  std::atomic<bool> open{false};
  std::atomic<bool> entered{false};
};
void GateFn(void* p) {
  auto* g = static_cast<Gate*>(p);
  g->entered = true;
  while (!g->open.load(std::memory_order_acquire)) std::this_thread::yield();
}
struct OrderLog {
  snd::base::Mutex m;
  std::vector<int> order;
};
struct OrderItem {
  OrderLog* log;
  int id;
};
void OrderFn(void* p) {
  auto* it = static_cast<OrderItem*>(p);
  snd::base::LockGuard g(it->log->m);
  it->log->order.push_back(it->id);
}
bool WaitTrue(const std::atomic<bool>& b) {
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (!b.load()) {
    if (snd::platform::MonotonicNanoseconds() - t0 > 5000000000ull) return false;
    std::this_thread::yield();
  }
  return true;
}
}  // namespace

SND_TEST(WorkerPool, PriorityOrderWithinLaneAndPrefetchFirst) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Gate gate;
  snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
  pool.Submit(&g, 1, snd::JobLane::Background);
  SND_REQUIRE(WaitTrue(gate.entered));  // the only worker is busy now
  OrderLog log;
  OrderItem items[6] = {{&log, 1}, {&log, 2}, {&log, 3}, {&log, 4}, {&log, 5}, {&log, 6}};
  // background: priorities 1, 9, 9 ; prefetch: 0, 5, 5
  snd::JobDesc bg[3] = {{&OrderFn, &items[0], nullptr, nullptr, 1, 0},
                        {&OrderFn, &items[1], nullptr, nullptr, 9, 0},
                        {&OrderFn, &items[2], nullptr, nullptr, 9, 0}};
  snd::JobDesc pf[3] = {{&OrderFn, &items[3], nullptr, nullptr, 0, 0},
                        {&OrderFn, &items[4], nullptr, nullptr, 5, 0},
                        {&OrderFn, &items[5], nullptr, nullptr, 5, 0}};
  pool.Submit(bg, 3, snd::JobLane::Background);
  pool.Submit(pf, 3, snd::JobLane::Prefetch);
  gate.open = true;
  SND_REQUIRE(pool.WaitIdle(5000000000ull));
  SND_REQUIRE_EQ(log.order.size(), 6u);
  const int expect[6] = {5, 6, 4, 2, 3, 1};  // prefetch lane first; higher priority first; FIFO among equals
  for (int i = 0; i < 6; ++i) SND_CHECK_EQ(log.order[i], expect[i]);
}

namespace {
struct StealData {
  std::atomic<std::uint32_t> ran{0};
  std::uint32_t* done;
  WorkerPool* pool;
};
void SlowChild(void* p) {
  auto* d = static_cast<StealData*>(p);
  snd::platform::SleepFor(200000);  // 0.2 ms
  d->ran.fetch_add(1);
}
void Spawner(void* p) {
  auto* d = static_cast<StealData*>(p);
  std::vector<snd::JobDesc> kids(64, snd::JobDesc{&SlowChild, d, d->done, nullptr, 0, 0});
  d->pool->Submit(kids.data(), 64, snd::JobLane::Prefetch);  // all into the spawner's own queue
}
}  // namespace

SND_TEST(WorkerPool, IdleWorkersStealFromABusyQueue) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 4;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  std::uint32_t done = 64;
  StealData d{{}, &done, &pool};
  snd::JobDesc spawn{&Spawner, &d, nullptr, nullptr, 0, 0};
  pool.Submit(&spawn, 1, snd::JobLane::Background);
  SND_REQUIRE(pool.WaitIdle(10000000000ull));
  SND_CHECK_EQ(d.ran.load(), 64u);
  SND_CHECK_EQ(std::atomic_ref<std::uint32_t>(done).load(), 0u);
  SND_CHECK(pool.GetStats().stolen > 0u);
  snd::test::Note("stolen: %llu", static_cast<unsigned long long>(pool.GetStats().stolen));
}

SND_TEST(WorkerPool, ShutdownCancelsPendingAndSubmitAfterIsInline) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Gate gate;
  snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
  pool.Submit(&g, 1, snd::JobLane::Prefetch);
  SND_REQUIRE(WaitTrue(gate.entered));
  std::atomic<std::uint32_t> ran{0};
  std::uint32_t done = 10;
  std::vector<snd::JobDesc> jobs(10, snd::JobDesc{[](void* p) { static_cast<std::atomic<std::uint32_t>*>(p)->fetch_add(1); },
                                                  &ran, &done, nullptr, 0, 0});
  pool.Submit(jobs.data(), 10, snd::JobLane::Background);
  std::thread opener([&] {
    snd::platform::SleepFor(20000000);
    gate.open = true;
  });
  pool.Shutdown(false);  // pending jobs complete as cancelled
  opener.join();
  SND_CHECK_EQ(ran.load(), 0u);
  SND_CHECK_EQ(std::atomic_ref<std::uint32_t>(done).load(), 0u);
  SND_CHECK_EQ(pool.GetStats().skipped, 10u);

  std::uint32_t done2 = 1;
  snd::JobDesc late{[](void* p) { static_cast<std::atomic<std::uint32_t>*>(p)->fetch_add(1); }, &ran, &done2,
                    nullptr, 0, 0};
  pool.Submit(&late, 1, snd::JobLane::Prefetch);  // after shutdown: inline
  SND_CHECK_EQ(ran.load(), 1u);
  SND_CHECK_EQ(done2, 0u);
  SND_CHECK_EQ(pool.GetStats().inline_, 1u);
  pool.Shutdown();  // idempotent
}

SND_TEST(WorkerPool, ReinitAndDrainOnShutdown) {
  WorkerPool pool;
  for (int round = 0; round < 3; ++round) {
    SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{}));
    std::atomic<std::uint32_t> ran{0};
    std::uint32_t done = 500;
    std::vector<snd::JobDesc> jobs(
        500, snd::JobDesc{[](void* p) { static_cast<std::atomic<std::uint32_t>*>(p)->fetch_add(1); }, &ran, &done,
                          nullptr, 0, 0});
    pool.Submit(jobs.data(), 500, snd::JobLane::Prefetch);
    pool.Shutdown(true);  // runs everything before returning
    SND_CHECK_EQ(ran.load(), 500u);
    SND_CHECK_EQ(done, 0u);
  }
  SND_CHECK(!pool.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{0, 16, true, "x"}));
}

SND_TEST(WorkerPool, SubmitFromRtScopeIsReported) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{}));
  std::uint32_t done = 1;
  snd::JobDesc j{[](void*) {}, nullptr, &done, nullptr, 0, 0};
  snd::test::rt::ExpectViolations expect;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();  // D-08: the audio thread must use JobPump instead
    pool.Submit(&j, 1, snd::JobLane::Prefetch);
  }
  SND_CHECK(snd::test::rt::Count(snd::rt::Violation::Lock) >= 1u);
  snd::test::rt::Reset();
  SND_CHECK(pool.WaitIdle(5000000000ull));
  SND_CHECK_EQ(pool.CurrentWorkerIndex(), -1);
}

// Decision (a): lane-wide priority with several workers. Both workers are held
// by gates; the jobs land round-robin in both queues; then only ONE worker is
// released, so the start order is fully observable: it must take the jobs of
// the lane in priority order (FIFO among equals) from both queues, never a
// lower-priority job of its own queue before a higher one of the other queue.
SND_TEST(WorkerPool, LaneWidePriorityAcrossQueuesWithTwoWorkers) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 2;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Gate g1, g2;
  snd::JobDesc gates[2] = {{&GateFn, &g1, nullptr, nullptr, 0, 0}, {&GateFn, &g2, nullptr, nullptr, 0, 0}};
  pool.Submit(gates, 2, snd::JobLane::Prefetch);
  SND_REQUIRE(WaitTrue(g1.entered));
  SND_REQUIRE(WaitTrue(g2.entered));
  OrderLog log;
  constexpr int kJobs = 24;
  OrderItem items[kJobs];
  std::uint32_t prio[kJobs];
  for (int i = 0; i < kJobs; ++i) {
    items[i] = OrderItem{&log, i};
    prio[i] = static_cast<std::uint32_t>((i * 5) % 7);
    snd::JobDesc d{&OrderFn, &items[i], nullptr, nullptr, prio[i], 0};
    pool.Submit(&d, 1, snd::JobLane::Prefetch);  // one by one: alternates the queues
  }
  g1.open = true;  // only the worker holding g1 runs
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  for (;;) {
    {
      snd::base::LockGuard lk(log.m);
      if (log.order.size() == static_cast<std::size_t>(kJobs)) break;
    }
    if (snd::platform::MonotonicNanoseconds() - t0 > 10000000000ull) break;
    std::this_thread::yield();
  }
  g2.open = true;
  SND_REQUIRE(pool.WaitIdle(5000000000ull));
  SND_REQUIRE_EQ(log.order.size(), static_cast<std::size_t>(kJobs));
  bool ok = true;
  for (int i = 1; i < kJobs; ++i) {
    const int a = log.order[static_cast<std::size_t>(i - 1)], b = log.order[static_cast<std::size_t>(i)];
    const bool inOrder = prio[a] != prio[b] ? prio[a] > prio[b] : a < b;
    ok = ok && inOrder;
  }
  SND_CHECK_MSG(ok, "jobs did not start in lane-wide priority order");
  SND_CHECK(pool.GetStats().stolen > 0u);
}

// Decision (b): with ONE worker, after 8 Prefetch jobs in a row one waiting
// Background job runs.
SND_TEST(WorkerPool, BackgroundGetsOneJobAfterEveryPrefetchBurst) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 1;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  SND_CHECK_EQ(cfg.prefetchBurst, 8u);
  Gate gate;
  snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
  pool.Submit(&g, 1, snd::JobLane::Background);  // the gate itself resets the streak
  SND_REQUIRE(WaitTrue(gate.entered));
  OrderLog log;
  OrderItem pf[40], bg[20];
  for (int i = 0; i < 20; ++i) {
    bg[i] = OrderItem{&log, 1000 + i};
    snd::JobDesc d{&OrderFn, &bg[i], nullptr, nullptr, 0, 0};
    pool.Submit(&d, 1, snd::JobLane::Background);
  }
  for (int i = 0; i < 40; ++i) {
    pf[i] = OrderItem{&log, i};
    snd::JobDesc d{&OrderFn, &pf[i], nullptr, nullptr, 0, 0};
    pool.Submit(&d, 1, snd::JobLane::Prefetch);
  }
  gate.open = true;
  SND_REQUIRE(pool.WaitIdle(5000000000ull));
  SND_REQUIRE_EQ(log.order.size(), 60u);
  std::vector<int> expect;
  for (int burst = 0; burst < 5; ++burst) {
    for (int i = 0; i < 8; ++i) expect.push_back(burst * 8 + i);
    expect.push_back(1000 + burst);
  }
  for (int i = 5; i < 20; ++i) expect.push_back(1000 + i);
  bool same = true;
  for (std::size_t i = 0; i < 60; ++i) same = same && log.order[i] == expect[i];
  SND_CHECK_MSG(same, "prefetch/background interleaving is not 8:1");
}

// Submit from non-worker threads concurrently with Shutdown (and before Init /
// after Shutdown): every job's done counter drops exactly once, no use after
// free (ASan / TSan).
SND_TEST(WorkerPool, SubmitRacingShutdownAlwaysCompletes) {
  WorkerPool pool;
  std::uint32_t outstanding = 0;  // accessed through atomic_ref
  std::atomic<std::uint64_t> ran{0};
  auto add = [&](std::uint32_t n) { std::atomic_ref<std::uint32_t>(outstanding).fetch_add(n); };
  const snd::JobDesc proto{[](void* p) { static_cast<std::atomic<std::uint64_t>*>(p)->fetch_add(1); }, &ran,
                           &outstanding, nullptr, 0, 0};
  add(1);
  pool.Submit(&proto, 1, snd::JobLane::Prefetch);  // before Init: inline
  for (int round = 0; round < 60; ++round) {
    WorkerPoolConfig cfg;
    cfg.workers = 1u + static_cast<std::uint32_t>(round % 3);
    cfg.initialQueueCapacity = 2;
    SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
    std::atomic<bool> stop{false};
    std::vector<std::thread> submitters;
    for (int t = 0; t < 3; ++t) {
      submitters.emplace_back([&, t] {
        snd::JobDesc batch[4] = {proto, proto, proto, proto};
        for (auto& b : batch) b.priority = static_cast<std::uint32_t>(t);
        while (!stop.load(std::memory_order_relaxed)) {
          add(4);
          pool.Submit(batch, 4, (t & 1) != 0 ? snd::JobLane::Background : snd::JobLane::Prefetch);
        }
      });
    }
    snd::platform::SleepFor(static_cast<std::uint64_t>(200000 + (round % 5) * 300000));
    pool.Shutdown((round & 1) == 0);  // while the submitters keep going
    snd::platform::SleepFor(200000);  // they now run inline
    stop = true;
    for (auto& th : submitters) th.join();
    SND_REQUIRE_EQ(std::atomic_ref<std::uint32_t>(outstanding).load(), 0u);
  }
  SND_CHECK(pool.GetStats().inline_ > 0u);
}
