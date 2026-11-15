// IJobSystem contract (snd/ext/job_system.hpp, snd::RunJob) checked against
// every implementation: the deterministic mock (reference), WorkerPool,
// JobService with own workers, JobService over an external C table, and the
// C table round trip. The same test bodies run for each.
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/jobs/job_service.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {

std::uint32_t LoadCounter(std::uint32_t* c) {
  return std::atomic_ref<std::uint32_t>(*c).load(std::memory_order_acquire);
}
bool WaitCounter(std::uint32_t* c, std::uint64_t timeoutNs = 10000000000ull) {
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (LoadCounter(c) != 0u) {
    if (snd::platform::MonotonicNanoseconds() - t0 > timeoutNs) return false;
    std::this_thread::yield();
  }
  return true;
}

struct Counting {
  std::atomic<std::uint32_t> runs{0};
};
void CountFn(void* p) { static_cast<Counting*>(p)->runs.fetch_add(1, std::memory_order_relaxed); }

struct Nested {
  snd::IJobSystem* js;
  Counting* counter;
  std::uint32_t* done;
};
void NestedFn(void* p) {
  auto* n = static_cast<Nested*>(p);
  snd::JobDesc child[3];
  for (auto& c : child) c = snd::JobDesc{&CountFn, n->counter, n->done, nullptr, 0, 0};
  n->js->Submit(child, 3, snd::JobLane::Background);
}

// Each implementation under test.
struct Subject {
  const char* name;
  snd::IJobSystem* js;
  bool threadSafe;  // may be used from several submitting threads
};

void ContractEachJobOnce(const Subject& s) {
  Counting c;
  std::uint32_t done = 100;
  std::vector<snd::JobDesc> jobs(100, snd::JobDesc{&CountFn, &c, &done, nullptr, 0, 0});
  for (std::uint32_t i = 0; i < 100; ++i) jobs[i].priority = i % 7u;
  s.js->Submit(jobs.data(), 50, snd::JobLane::Prefetch);
  s.js->Submit(jobs.data() + 50, 50, snd::JobLane::Background);
  SND_CHECK_MSG(WaitCounter(&done), "%s: done counter did not reach 0", s.name);
  SND_CHECK_MSG(c.runs.load() == 100u, "%s: %u runs", s.name, c.runs.load());
}

void ContractCancelSkipsButCompletes(const Subject& s) {
  Counting c;
  std::uint32_t done = 2;
  std::uint32_t cancel = 1;
  const snd::JobDesc jobs[2] = {snd::JobDesc{&CountFn, &c, &done, &cancel, 0, 0},
                                snd::JobDesc{&CountFn, &c, &done, &cancel, 5, 0}};
  s.js->Submit(jobs, 2, snd::JobLane::Prefetch);
  SND_CHECK_MSG(WaitCounter(&done), "%s: cancelled jobs must still complete", s.name);
  SND_CHECK_MSG(c.runs.load() == 0u, "%s: cancelled job ran", s.name);
}

void ContractNullCountersAndNestedSubmit(const Subject& s) {
  Counting c;
  std::uint32_t done = 1 + 3;
  Nested n{s.js, &c, &done};
  snd::JobDesc parent{&NestedFn, &n, &done, nullptr, 0, 0};
  snd::JobDesc noCounter{&CountFn, &c, nullptr, nullptr, 0, 0};
  s.js->Submit(&parent, 1, snd::JobLane::Prefetch);
  SND_CHECK_MSG(WaitCounter(&done), "%s: nested jobs incomplete", s.name);
  s.js->Submit(&noCounter, 1, snd::JobLane::Background);
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (c.runs.load() != 4u && snd::platform::MonotonicNanoseconds() - t0 < 10000000000ull) std::this_thread::yield();
  SND_CHECK_MSG(c.runs.load() == 4u, "%s: %u runs", s.name, c.runs.load());
}

void ContractConcurrentSubmitters(const Subject& s) {
  if (!s.threadSafe) return;
  Counting c;
  constexpr std::uint32_t kThreads = 4, kPer = 2000;
  std::uint32_t done = kThreads * kPer;
  std::vector<std::thread> ts;
  for (std::uint32_t t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (std::uint32_t i = 0; i < kPer; i += 10) {
        snd::JobDesc batch[10];
        for (auto& b : batch) b = snd::JobDesc{&CountFn, &c, &done, nullptr, i % 3u, 0};
        s.js->Submit(batch, 10, (t & 1u) != 0u ? snd::JobLane::Prefetch : snd::JobLane::Background);
      }
    });
  }
  for (auto& t : ts) t.join();
  SND_CHECK_MSG(WaitCounter(&done), "%s: concurrent submit incomplete", s.name);
  SND_CHECK_MSG(c.runs.load() == kThreads * kPer, "%s: %u runs", s.name, c.runs.load());
}

void RunContract(const Subject& s) {
  ContractEachJobOnce(s);
  ContractCancelSkipsButCompletes(s);
  ContractNullCountersAndNestedSubmit(s);
  ContractConcurrentSubmitters(s);
}

}  // namespace

SND_TEST(JobContract, DeterministicMockReference) {
  snd::mock::DeterministicJobSystem mock(snd::mock::JobMode::Immediate);
  RunContract(Subject{"mock", &mock, false});
}

SND_TEST(JobContract, WorkerPool) {
  WorkerPool pool;
  WorkerPoolConfig cfg;
  cfg.workers = 3;
  cfg.initialQueueCapacity = 4;  // forces queue growth
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), cfg));
  RunContract(Subject{"WorkerPool", &pool, true});
  SND_CHECK(pool.WaitIdle(5000000000ull));
  pool.Shutdown();
}

SND_TEST(JobContract, WorkerPoolThroughCTable) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{}));
  snd_job_system table = snd::ToC(pool);
  snd::CJobSystem back(table);
  RunContract(Subject{"WorkerPool via C table", &back, true});
}

SND_TEST(JobContract, JobServiceOwnWorkers) {
  JobService svc;
  SND_REQUIRE(svc.Init(snd::base::DefaultAllocator(), nullptr, 2));
  SND_CHECK(!svc.UsesExternal());
  SND_CHECK(svc.OwnPool() != nullptr);
  SND_CHECK_EQ(svc.OwnPool()->Workers(), 2u);
  RunContract(Subject{"JobService(own)", &svc.Jobs(), true});
}

SND_TEST(JobContract, JobServiceExternalHost) {
  // The host's job system is another pool here (thread-safe); Config.jobs.external is a C table.
  WorkerPool host;
  SND_REQUIRE(host.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{}));
  const snd_job_system table = snd::ToC(host);
  JobService svc;
  SND_REQUIRE(svc.Init(snd::base::DefaultAllocator(), &table, 7));
  SND_CHECK(svc.UsesExternal());
  SND_CHECK(svc.OwnPool() == nullptr);
  RunContract(Subject{"JobService(external)", &svc.Jobs(), true});
  SND_CHECK(host.GetStats().submitted >= 100u);  // really went to the host
  svc.Shutdown();
  host.Shutdown();

  // The deterministic mock as host: runs jobs on the submitting thread.
  snd::mock::DeterministicJobSystem mock;
  const snd_job_system mockTable = snd::ToC(mock);
  JobService svc2;
  SND_REQUIRE(svc2.Init(snd::base::DefaultAllocator(), &mockTable, 0));
  RunContract(Subject{"JobService(external mock)", &svc2.Jobs(), false});
  SND_CHECK(mock.ExecutedCount() >= 100u);

  snd_job_system broken{};
  JobService svc3;
  SND_CHECK(!svc3.Init(snd::base::DefaultAllocator(), &broken, 1));
}
