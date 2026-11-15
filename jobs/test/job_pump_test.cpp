// JobPump: RT-safe postJob queue forwarded to an IJobSystem (CONTRACTS.md D-08).
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "snd/base/allocator.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/internal/source_if.hpp"
#include "snd/jobs/job_service.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/deterministic_job_system.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"

using namespace snd::jobs;

namespace {
struct Rec {
  std::vector<int>* log;
  int id;
};
void RecFn(void* p) {
  auto* r = static_cast<Rec*>(p);
  r->log->push_back(r->id);
}
}  // namespace

SND_TEST(JobPump, PostIsRtSafeAndPumpForwardsInOrder) {
  snd::mock::DeterministicJobSystem mock(snd::mock::JobMode::Manual);
  JobPump pump;
  SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 4, mock));
  std::vector<int> log;
  Rec recs[5] = {{&log, 1}, {&log, 2}, {&log, 3}, {&log, 4}, {&log, 5}};
  {
    SND_RT_SCOPE();
    SND_CHECK(pump.Post(snd::JobDesc{&RecFn, &recs[0], nullptr, nullptr, 0, 0}, snd::JobLane::Prefetch));
    SND_CHECK(pump.Post(snd::JobDesc{&RecFn, &recs[1], nullptr, nullptr, 0, 0}, snd::JobLane::Prefetch));
    SND_CHECK(pump.Post(snd::JobDesc{&RecFn, &recs[2], nullptr, nullptr, 0, 0}, snd::JobLane::Background));
    SND_CHECK(pump.Post(snd::JobDesc{&RecFn, &recs[3], nullptr, nullptr, 0, 0}, snd::JobLane::Prefetch));
    SND_CHECK(!pump.Post(snd::JobDesc{&RecFn, &recs[4], nullptr, nullptr, 0, 0}, snd::JobLane::Prefetch));  // full
  }
  SND_CHECK_EQ(pump.Rejected(), 1u);
  SND_CHECK_EQ(pump.Posted(), 4u);
  SND_CHECK_EQ(mock.Pending(), 0u);  // nothing reaches the job system before Pump
  SND_CHECK_EQ(pump.Pump(), 4u);
  SND_CHECK_EQ(mock.Pending(snd::JobLane::Prefetch), 3u);
  SND_CHECK_EQ(mock.Pending(snd::JobLane::Background), 1u);
  SND_CHECK_EQ(mock.SubmittedCount(), 4u);
  mock.RunAll();
  SND_REQUIRE_EQ(log.size(), 4u);
  for (int i = 0; i < 4; ++i) SND_CHECK_EQ(log[static_cast<std::size_t>(i)], i + 1);  // posting order
  SND_CHECK_EQ(pump.Forwarded(), 4u);
  SND_CHECK_EQ(pump.Pump(), 0u);
}

SND_TEST(JobPump, ServesAsSourceServicesPostJob) {
  snd::mock::DeterministicJobSystem mock(snd::mock::JobMode::Immediate);
  JobPump pump;
  SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 16, mock));
  snd::source::SourceServices services{};
  services.postJob = &JobPump::PostThunk;
  services.jobCtx = &pump;
  std::vector<int> log;
  Rec r{&log, 7};
  {
    SND_RT_SCOPE();
    SND_CHECK(services.postJob(services.jobCtx, snd::JobDesc{&RecFn, &r, nullptr, nullptr, 0, 0},
                               snd::JobLane::Prefetch));
  }
  pump.Pump();
  SND_REQUIRE_EQ(log.size(), 1u);
  SND_CHECK_EQ(log[0], 7);
}

SND_TEST(JobPump, ThreadForwardsFromManyRtProducersToWorkers) {
  WorkerPool pool;
  SND_REQUIRE(pool.Init(snd::base::DefaultAllocator(), WorkerPoolConfig{}));
  JobPump pump;
  SND_REQUIRE(pump.Init(snd::base::DefaultAllocator(), 256, pool));
  SND_REQUIRE(pump.StartThread(5));
  std::atomic<std::uint32_t> ran{0};
  constexpr std::uint32_t kProducers = 3, kPer = 3000;
  std::uint32_t done = kProducers * kPer;
  std::vector<std::thread> producers;
  for (std::uint32_t p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      SND_RT_SCOPE();  // audio thread / RT workers: no locks, no allocation
      const snd::JobDesc j{[](void* x) { static_cast<std::atomic<std::uint32_t>*>(x)->fetch_add(1); }, &ran, &done,
                           nullptr, p, 0};
      for (std::uint32_t i = 0; i < kPer;) {
        if (pump.Post(j, (i & 1u) != 0u ? snd::JobLane::Prefetch : snd::JobLane::Background)) {
          ++i;
        } else {
          snd::base::CpuRelax();  // full: an RT thread would retry next block
        }
      }
    });
  }
  for (auto& t : producers) t.join();
  const std::uint64_t t0 = snd::platform::MonotonicNanoseconds();
  while (std::atomic_ref<std::uint32_t>(done).load() != 0u &&
         snd::platform::MonotonicNanoseconds() - t0 < 10000000000ull) {
    std::this_thread::yield();
  }
  SND_CHECK_EQ(std::atomic_ref<std::uint32_t>(done).load(), 0u);
  SND_CHECK_EQ(ran.load(), kProducers * kPer);
  SND_CHECK_EQ(pump.Forwarded(), static_cast<std::uint64_t>(kProducers * kPer));
  pump.StopThread();
  pump.Destroy();
  pool.Shutdown();
}
