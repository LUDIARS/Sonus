// Independent tests of the normal workers (IJobSystem, snd_job_desc contract,
// design.md 2.3) and the RT -> non-RT job pump (D-08).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/jobs/job_service.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;
using snd::platform::MonotonicNanoseconds;

namespace {
constexpr std::uint64_t kMs = 1000000ull;

std::uint32_t Load(const std::uint32_t* c) {
  return std::atomic_ref<std::uint32_t>(*const_cast<std::uint32_t*>(c)).load(std::memory_order_acquire);
}
void Store(std::uint32_t* c, std::uint32_t v) { std::atomic_ref<std::uint32_t>(*c).store(v, std::memory_order_release); }
bool WaitZero(const std::uint32_t* c, std::uint64_t timeoutNs = 20000 * kMs) {
  const std::uint64_t t0 = MonotonicNanoseconds();
  while (Load(c) != 0u) {
    if (MonotonicNanoseconds() - t0 > timeoutNs) return false;
    std::this_thread::yield();
  }
  return true;
}

// Gate job: blocks a worker until opened.
struct Gate {
  std::atomic<bool> open{false};
  std::atomic<int> entered{0};
  std::atomic<int> worker{-1};
  WorkerPool* pool = nullptr;
};
void GateFn(void* p) {
  auto* g = static_cast<Gate*>(p);
  if (g->pool != nullptr) g->worker.store(g->pool->CurrentWorkerIndex());
  g->entered.fetch_add(1);
  while (!g->open.load(std::memory_order_acquire)) std::this_thread::yield();
}

struct OrderLog {
  std::mutex m;
  std::vector<std::uint32_t> order;
};
struct OrderJob {
  OrderLog* log;
  std::uint32_t id;
};
void OrderFn(void* p) {
  auto* j = static_cast<OrderJob*>(p);
  std::lock_guard<std::mutex> lock(j->log->m);
  j->log->order.push_back(j->id);
}

WorkerPoolConfig PoolCfg(std::uint32_t workers) {
  WorkerPoolConfig c;
  c.workers = workers;
  c.initialQueueCapacity = 2;  // exercise growth
  return c;
}
}  // namespace

SND_TEST(AdvWorkerPool, SingleWorkerStrictPriorityThenFifoWithinLaneAndPrefetchFirst) {
  // With one worker there is one queue per lane, so the snd_job_desc order is
  // observable exactly: higher priority first, FIFO among equals, and the
  // prefetch lane before the background lane.
  WorkerPool pool;
  // The lane ratio (one background job after every `prefetchBurst` prefetch
  // jobs) is tested separately; switch it off here so that the strict
  // "prefetch lane first" order below is the expected one.
  WorkerPoolConfig cfg = PoolCfg(1);
  cfg.prefetchBurst = 1000;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  Gate gate;
  snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
  pool.Submit(&g, 1, snd::JobLane::Prefetch);
  while (gate.entered.load() == 0) std::this_thread::yield();
  OrderLog log;
  constexpr std::uint32_t kJobs = 60;
  std::vector<OrderJob> jobs(kJobs);
  std::vector<snd::JobDesc> descs(kJobs);
  std::uint32_t prio[kJobs];
  for (std::uint32_t i = 0; i < kJobs; ++i) {
    jobs[i] = OrderJob{&log, i};
    prio[i] = (i * 7u) % 5u;
    descs[i] = snd::JobDesc{&OrderFn, &jobs[i], nullptr, nullptr, prio[i], 0};
  }
  // Background jobs 40..59 are submitted FIRST; prefetch must still run before them.
  pool.Submit(descs.data() + 40, 20, snd::JobLane::Background);
  for (std::uint32_t i = 0; i < 40; ++i) pool.Submit(&descs[i], 1, snd::JobLane::Prefetch);
  gate.open = true;
  SND_REQUIRE(pool.WaitIdle(20000 * kMs));
  SND_REQUIRE_EQ(log.order.size(), static_cast<std::size_t>(kJobs));
  auto before = [&](std::uint32_t a, std::uint32_t b) {  // expected order
    const bool aPre = a < 40u, bPre = b < 40u;
    if (aPre != bPre) return aPre;
    if (prio[a] != prio[b]) return prio[a] > prio[b];
    return a < b;
  };
  bool ok = true;
  for (std::size_t i = 1; i < log.order.size(); ++i) ok = ok && before(log.order[i - 1], log.order[i]);
  SND_CHECK_MSG(ok, "execution order violates lane/priority/FIFO");
}

namespace {
struct CancelCtx {
  std::atomic<int> runs{0};
};
void CountRun(void* p) { static_cast<CancelCtx*>(p)->runs.fetch_add(1); }
}  // namespace

SND_TEST(AdvWorkerPool, CancelFlagReadAtStartNotAtSubmit) {
  for (int impl = 0; impl < 2; ++impl) {  // 0 = WorkerPool, 1 = deterministic mock (Manual)
    WorkerPool pool;
    snd::mock::DeterministicJobSystem mock(snd::mock::JobMode::Manual);
    snd::IJobSystem* js = &mock;
    Gate gate;
    if (impl == 0) {
      SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(1)));
      js = &pool;
      snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
      pool.Submit(&g, 1, snd::JobLane::Prefetch);
      while (gate.entered.load() == 0) std::this_thread::yield();
    }
    CancelCtx c;
    std::uint32_t done = 4;
    std::uint32_t cancelLater = 0, cancelNow = 1, cancelCleared = 1;
    snd::JobDesc d[4] = {
        {&CountRun, &c, &done, &cancelLater, 0, 0},    // cancelled after submit, before start -> skipped
        {&CountRun, &c, &done, &cancelNow, 0, 0},      // cancelled at submit -> skipped
        {&CountRun, &c, &done, &cancelCleared, 0, 0},  // cancel cleared before start -> runs
        {nullptr, nullptr, &done, nullptr, 0, 0},      // no fn: only the counter drops
    };
    js->Submit(d, 4, snd::JobLane::Background);
    Store(&cancelLater, 1);
    Store(&cancelCleared, 0);
    if (impl == 0) gate.open = true;
    else mock.RunAll();
    SND_CHECK_MSG(WaitZero(&done), "impl %d: counter %u", impl, Load(&done));
    SND_CHECK_MSG(c.runs.load() == 1, "impl %d: %d runs (expected only the uncancelled job)", impl, c.runs.load());
    if (impl == 0) {
      SND_CHECK(pool.WaitIdle(5000 * kMs));
      const WorkerPool::Stats s = pool.GetStats();
      SND_CHECK_EQ(s.skipped, 2u);
      SND_CHECK_EQ(s.executed, 3u);  // gate + uncancelled + fn-less job
    }
  }
}

namespace {
struct Chain {
  snd::IJobSystem* js;
  std::atomic<std::uint32_t> depth{0};
  std::uint32_t* done;
  std::uint32_t maxDepth;
};
void ChainFn(void* p) {
  auto* c = static_cast<Chain*>(p);
  const std::uint32_t d = c->depth.fetch_add(1) + 1u;
  if (d < c->maxDepth) {
    snd::JobDesc next{&ChainFn, c, c->done, nullptr, d % 4u, 0};
    c->js->Submit(&next, 1, (d & 1u) != 0u ? snd::JobLane::Background : snd::JobLane::Prefetch);
  }
}
struct Fan {
  snd::IJobSystem* js;
  std::uint32_t* done;
  std::atomic<std::uint32_t>* slots;
  std::uint32_t level;
  std::uint32_t index;
};
struct FanCtx {
  snd::IJobSystem* js;
  std::uint32_t* done;
  std::vector<std::atomic<std::uint32_t>>* slots;
  std::vector<Fan>* nodes;
};
FanCtx* gFan = nullptr;
void FanFn(void* p) {
  auto* f = static_cast<Fan*>(p);
  (*gFan->slots)[f->index].fetch_add(1);
  if (f->level == 0u) return;
  // Binary tree of jobs: node i has children 2i+1, 2i+2 (preallocated descriptors).
  snd::JobDesc kids[2];
  for (std::uint32_t k = 0; k < 2; ++k) {
    Fan* child = &(*gFan->nodes)[f->index * 2u + 1u + k];
    kids[k] = snd::JobDesc{&FanFn, child, gFan->done, nullptr, f->level, 0};
  }
  gFan->js->Submit(kids, 2, (f->index & 1u) != 0u ? snd::JobLane::Prefetch : snd::JobLane::Background);
}
}  // namespace

SND_TEST(AdvWorkerPool, DeepSubmitChainsAndJobTreesFromInsideJobs) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(3)));
  // A chain of 5000 jobs, each submitting the next from inside a job.
  std::uint32_t done = 5000;
  Chain chain{&pool, {0}, &done, 5000};
  snd::JobDesc first{&ChainFn, &chain, &done, nullptr, 0, 0};
  pool.Submit(&first, 1, snd::JobLane::Prefetch);
  SND_CHECK(WaitZero(&done));
  SND_CHECK_EQ(chain.depth.load(), 5000u);
  // A binary tree of 2^15 - 1 jobs fanned out from inside jobs (stealing spreads it).
  constexpr std::uint32_t kLevels = 15, kNodes = (1u << kLevels) - 1u;
  std::vector<std::atomic<std::uint32_t>> slots(kNodes);
  for (auto& s : slots) s.store(0);
  std::vector<Fan> nodes(kNodes);
  std::uint32_t treeDone = kNodes;
  for (std::uint32_t i = 0; i < kNodes; ++i) {
    std::uint32_t level = 0;
    for (std::uint32_t n = i + 1u; n < (1u << (kLevels - 1u)); n <<= 1) ++level;  // leaves have level 0
    nodes[i] = Fan{&pool, &treeDone, nullptr, level, i};
  }
  FanCtx ctx{&pool, &treeDone, &slots, &nodes};
  gFan = &ctx;
  snd::JobDesc root{&FanFn, &nodes[0], &treeDone, nullptr, 0, 0};
  pool.Submit(&root, 1, snd::JobLane::Background);
  SND_CHECK(WaitZero(&treeDone));
  std::uint32_t wrong = 0;
  for (auto& s : slots) wrong += s.load() != 1u ? 1u : 0u;
  SND_CHECK_EQ(wrong, 0u);
  SND_CHECK(pool.WaitIdle(5000 * kMs));
  gFan = nullptr;
}

namespace {
struct Slots {
  std::vector<std::atomic<std::uint8_t>> runs;
};
struct SlotJob {
  Slots* s;
  std::uint32_t i;
};
void SlotFn(void* p) {
  auto* j = static_cast<SlotJob*>(p);
  j->s->runs[j->i].fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

SND_TEST(AdvWorkerPool, MassiveSubmitFromManyThreadsEveryJobExactlyOnce) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(4)));
  constexpr std::uint32_t kThreads = 4, kPer = 25000;
  Slots s;
  s.runs = std::vector<std::atomic<std::uint8_t>>(kThreads * kPer);
  std::vector<SlotJob> jobs(kThreads * kPer);
  for (std::uint32_t i = 0; i < kThreads * kPer; ++i) jobs[i] = SlotJob{&s, i};
  std::uint32_t done = kThreads * kPer;
  std::vector<std::thread> ts;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      std::uint32_t i = 0;
      while (i < kPer) {
        const std::uint32_t n = (i % 7u) + 1u < kPer - i ? (i % 7u) + 1u : kPer - i;  // batches of 1..7
        snd::JobDesc batch[7];
        for (std::uint32_t k = 0; k < n; ++k) {
          batch[k] = snd::JobDesc{&SlotFn, &jobs[t * kPer + i + k], &done, nullptr, (i + k) % 3u, 0};
        }
        pool.Submit(batch, n, ((i / 7u) & 1u) != 0u ? snd::JobLane::Background : snd::JobLane::Prefetch);
        i += n;
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK(WaitZero(&done));
  SND_CHECK(pool.WaitIdle(5000 * kMs));
  std::uint32_t wrong = 0;
  for (auto& r : s.runs) wrong += r.load() != 1u ? 1u : 0u;
  SND_CHECK_EQ(wrong, 0u);
  const WorkerPool::Stats st = pool.GetStats();
  SND_CHECK_EQ(st.submitted, static_cast<std::uint64_t>(kThreads) * kPer);
  SND_CHECK_EQ(st.executed + st.skipped, static_cast<std::uint64_t>(kThreads) * kPer);
  SND_CHECK_EQ(pool.Pending(), 0u);
}

namespace {
struct Spawner {
  snd::IJobSystem* js;
  std::uint32_t* done;
  std::atomic<std::uint32_t> ran{0};
  std::atomic<std::uint32_t> children{0};
};
void LeafFn(void* p) { static_cast<Spawner*>(p)->ran.fetch_add(1); }
void SpawnFn(void* p) {
  auto* s = static_cast<Spawner*>(p);
  s->ran.fetch_add(1);
  snd::platform::SleepFor(200000);  // still running when Shutdown starts
  snd::JobDesc kids[3];
  for (auto& k : kids) k = snd::JobDesc{&LeafFn, s, s->done, nullptr, 0, 0};
  s->children.fetch_add(3);
  // The spawner's own count is still held while fn runs, so adding here is race-free.
  std::atomic_ref<std::uint32_t>(*s->done).fetch_add(3, std::memory_order_relaxed);
  s->js->Submit(kids, 3, snd::JobLane::Background);
}
}  // namespace

SND_TEST(AdvWorkerPool, ShutdownBothModesAlwaysDrainsDoneCounters) {
  for (int runPending = 0; runPending < 2; ++runPending) {
    for (int round = 0; round < 5; ++round) {
      WorkerPool pool;
      SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(2)));
      constexpr std::uint32_t kSpawners = 20, kLeaves = 2000;
      // Counter covers spawners and plain leaves; running spawners add their 3 children.
      std::uint32_t done = kSpawners + kLeaves;
      Spawner sp{&pool, &done};
      std::vector<snd::JobDesc> d;
      for (std::uint32_t i = 0; i < kSpawners; ++i) d.push_back(snd::JobDesc{&SpawnFn, &sp, &done, nullptr, 1, 0});
      for (std::uint32_t i = 0; i < kLeaves; ++i) d.push_back(snd::JobDesc{&LeafFn, &sp, &done, nullptr, 0, 0});
      pool.Submit(d.data(), static_cast<std::uint32_t>(d.size()), snd::JobLane::Prefetch);
      if (round % 2 == 1) snd::platform::SleepFor(300000);
      pool.Shutdown(runPending != 0);
      // After Shutdown returns every counter must be zero: nobody is left to run anything.
      SND_CHECK_MSG(Load(&done) == 0u, "runPending=%d round=%d: counter %u after Shutdown", runPending, round, Load(&done));
      const WorkerPool::Stats s = pool.GetStats();
      SND_CHECK_EQ(s.executed + s.skipped, s.submitted);
      if (runPending != 0) {
        SND_CHECK_EQ(s.skipped, 0u);
        SND_CHECK_EQ(sp.ran.load(), kSpawners + kLeaves + sp.children.load());
      }
      // Submit after shutdown: run inline, counter still drops.
      std::uint32_t late = 1;
      snd::JobDesc lateJob{&LeafFn, &sp, &late, nullptr, 0, 0};
      pool.Submit(&lateJob, 1, snd::JobLane::Prefetch);
      SND_CHECK_EQ(Load(&late), 0u);
    }
  }
}

SND_TEST(AdvWorkerPool, WaitIdleTimesOutWhileBusyAndSucceedsAfter) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(2)));
  SND_CHECK(pool.WaitIdle(0));  // idle from the start
  Gate gate;
  snd::JobDesc g{&GateFn, &gate, nullptr, nullptr, 0, 0};
  pool.Submit(&g, 1, snd::JobLane::Background);
  const std::uint64_t t0 = MonotonicNanoseconds();
  SND_CHECK(!pool.WaitIdle(30 * kMs));
  SND_CHECK(MonotonicNanoseconds() - t0 >= 30 * kMs);
  gate.open = true;
  SND_CHECK(pool.WaitIdle(5000 * kMs));
  SND_CHECK_EQ(pool.Pending(), 0u);
}

namespace {
struct Flood {
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> prefetchRan{0};
};
void PrefetchWork(void* p) {
  auto* f = static_cast<Flood*>(p);
  snd::platform::SleepFor(100000);  // 100 us of "decode"
  f->prefetchRan.fetch_add(1);
}
void SetFlag(void* p) { static_cast<std::atomic<bool>*>(p)->store(true); }
}  // namespace

SND_TEST(AdvWorkerPool, BackgroundLaneNotStarvedByPrefetchFlood) {
  // 2.3 two lanes: a continuous decode flood must not starve bank preparation.
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), PoolCfg(2)));
  Flood flood;
  std::thread producer([&] {
    while (!flood.stop.load()) {
      snd::JobDesc d[8];
      for (auto& x : d) x = snd::JobDesc{&PrefetchWork, &flood, nullptr, nullptr, 255, 0};
      pool.Submit(d, 8, snd::JobLane::Prefetch);
      // keep the prefetch queues non-empty at all times
      while (pool.Pending() > 64u && !flood.stop.load()) std::this_thread::yield();
    }
  });
  snd::platform::SleepFor(20 * kMs);
  std::atomic<bool> ran{false};
  snd::JobDesc bg{&SetFlag, &ran, nullptr, nullptr, 0, 0};
  const std::uint64_t t0 = MonotonicNanoseconds();
  pool.Submit(&bg, 1, snd::JobLane::Background);
  while (!ran.load() && MonotonicNanoseconds() - t0 < 3000 * kMs) std::this_thread::yield();
  const std::uint64_t dt = MonotonicNanoseconds() - t0;
  flood.stop = true;
  producer.join();
  pool.Shutdown(false);
  snd::test::Note("background job started after %.2f ms (prefetch jobs run: %llu)", static_cast<double>(dt) / 1e6,
                  static_cast<unsigned long long>(flood.prefetchRan.load()));
  SND_CHECK(ran.load());
}

// --- JobPump ------------------------------------------------------------------------------

namespace {
// Target that records what it receives (thread-safe).
class Recorder final : public snd::IJobSystem {
 public:
  void Submit(const snd::JobDesc* jobs, std::uint32_t count, snd::JobLane lane) override {
    std::lock_guard<std::mutex> lock(m_);
    for (std::uint32_t i = 0; i < count; ++i) {
      got_.push_back(Item{reinterpret_cast<std::uintptr_t>(jobs[i].data), static_cast<std::uint32_t>(lane)});
      snd::RunJob(jobs[i]);
    }
    ++batches_;
  }
  struct Item {
    std::uintptr_t id;
    std::uint32_t lane;
  };
  std::vector<Item> Items() {
    std::lock_guard<std::mutex> lock(m_);
    return got_;
  }
  std::uint32_t Batches() {
    std::lock_guard<std::mutex> lock(m_);
    return batches_;
  }

 private:
  std::mutex m_;
  std::vector<Item> got_;
  std::uint32_t batches_ = 0;
};
void Nop(void*) {}
}  // namespace

SND_TEST(AdvJobPump, FullQueueRejectsAndCountsOrderAndLanesPreserved) {
  Recorder rec;
  JobPump pump;
  SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 5, rec));  // rounded to 8
  std::uint32_t accepted = 0;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    for (std::uint32_t i = 1; i <= 12; ++i) {
      const snd::JobLane lane = i % 3u == 0u ? snd::JobLane::Background : snd::JobLane::Prefetch;
      if (pump.Post(snd::JobDesc{&Nop, reinterpret_cast<void*>(static_cast<std::uintptr_t>(i)), nullptr, nullptr, 0, 0}, lane)) {
        ++accepted;
      }
    }
    SND_CHECK(!JobPump::PostThunk(&pump, snd::JobDesc{&Nop, nullptr, nullptr, nullptr, 0, 0}, snd::JobLane::Prefetch));
  }
  SND_CHECK_EQ(snd::test::rt::Total(), 0u);
  SND_CHECK_EQ(accepted, 8u);
  SND_CHECK_EQ(pump.Posted(), 8u);
  SND_CHECK_EQ(pump.Rejected(), 5u);
  // An out-of-range lane is forwarded to the background lane (not dropped, not UB).
  SND_CHECK_EQ(pump.Pump(3), 3u);  // partial pump
  SND_CHECK_EQ(pump.Pump(), 5u);
  SND_CHECK_EQ(pump.Pump(), 0u);
  SND_CHECK_EQ(pump.Forwarded(), 8u);
  const auto items = rec.Items();
  SND_REQUIRE_EQ(items.size(), 8u);
  for (std::uint32_t i = 0; i < 8; ++i) {
    SND_CHECK_EQ(items[i].id, i + 1u);  // posting order
    SND_CHECK_EQ(items[i].lane, (i + 1u) % 3u == 0u ? 1u : 0u);
  }
  // Freed space is usable again.
  SND_CHECK(pump.Post(snd::JobDesc{&Nop, nullptr, nullptr, nullptr, 0, 0}, static_cast<snd::JobLane>(7)));
  SND_CHECK_EQ(pump.Pump(), 1u);
  SND_CHECK_EQ(rec.Items().back().lane, 1u);
}

SND_TEST(AdvJobPump, StopThreadThenDestroyForwardsEverythingPostedConcurrently) {
  WorkerPool workers;
  SND_REQUIRE(workers.Init(snd::base::DefaultAllocator(), PoolCfg(2)));
  for (int round = 0; round < 10; ++round) {
    JobPump pump;
    SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 64, workers));
    SND_REQUIRE(pump.StartThread(1));
    std::uint32_t done = 0;
    std::atomic<std::uint32_t> posted{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
      producers.emplace_back([&] {
        SND_RT_SCOPE();  // the audio thread / RT workers post
        while (!stop.load(std::memory_order_relaxed)) {
          std::atomic_ref<std::uint32_t>(done).fetch_add(1);
          if (pump.Post(snd::JobDesc{&Nop, nullptr, &done, nullptr, 0, 0}, snd::JobLane::Prefetch)) {
            posted.fetch_add(1);
          } else {
            std::atomic_ref<std::uint32_t>(done).fetch_sub(1);  // rejected: retried "next block"
          }
        }
      });
    }
    snd::platform::SleepFor(2 * kMs);
    pump.StopThread();  // races with Post
    snd::platform::SleepFor(1 * kMs);
    stop = true;
    for (auto& t : producers) t.join();
    pump.Destroy();  // documented: forwards what is still queued
    SND_CHECK_EQ(pump.Forwarded(), static_cast<std::uint64_t>(posted.load()));
    SND_CHECK_MSG(WaitZero(&done), "round %d: %u jobs never completed", round, Load(&done));
  }
  SND_CHECK(workers.WaitIdle(5000 * kMs));
}

SND_TEST(AdvJobPump, ThreadWakesPromptlyOnPost) {
  // The pump thread is woken by Post; with a 1 s safety poll, a job posted
  // while the thread sleeps must arrive long before the poll would fire.
  Recorder rec;
  JobPump pump;
  SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 16, rec));
  SND_REQUIRE(pump.StartThread(1000));
  SND_CHECK(!pump.StartThread(1000));  // already running
  snd::platform::SleepFor(20 * kMs);
  std::uint64_t worst = 0;
  for (int i = 0; i < 20; ++i) {
    const std::size_t before = rec.Items().size();
    const std::uint64_t t0 = MonotonicNanoseconds();
    SND_REQUIRE(pump.Post(snd::JobDesc{&Nop, nullptr, nullptr, nullptr, 0, 0}, snd::JobLane::Background));
    while (rec.Items().size() == before && MonotonicNanoseconds() - t0 < 3000 * kMs) std::this_thread::yield();
    const std::uint64_t dt = MonotonicNanoseconds() - t0;
    if (dt > worst) worst = dt;
    snd::platform::SleepFor(2 * kMs);
  }
  snd::test::Note("worst post->forward latency %.2f ms", static_cast<double>(worst) / 1e6);
  SND_CHECK(worst < 500 * kMs);  // far below the 1 s poll
  pump.StopThread();
  pump.StopThread();  // idempotent
  pump.Destroy();
  pump.Destroy();
}
