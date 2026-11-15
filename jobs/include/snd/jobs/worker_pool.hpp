// Normal workers: IJobSystem with two lanes and work stealing (design.md 2.3).
//
// WorkerPool implements snd::IJobSystem for normal-worker jobs (decode,
// bank validation, IR conversion, ...). It is NOT used for RT work
// (rt_workers.hpp) and must not be called from the audio thread or RT
// workers (they use JobPump, D-08) — Submit takes a mutex and may allocate,
// both of which the RT checks report.
//
// Scheduling:
//  - Every worker owns one queue per lane (Prefetch, Background). A queue is
//    a priority heap: higher JobDesc::priority first, FIFO among equals
//    (per queue).
//  - Submit from a worker of this pool pushes to that worker's own queue;
//    other threads distribute round-robin.
//  - An idle worker takes from its own queue, then steals from the others
//    (work stealing), lane by lane. Workers 0..M-2 look at Prefetch first;
//    with M >= 2 the last worker looks at Background first so background work
//    is never starved by a stream of decode jobs.
//  - Jobs follow the JobDesc contract through snd::RunJob: each runs at most
//    once, a set cancel flag skips fn, the done counter is decremented exactly
//    once either way.
//  - Priority is a hint: no global ordering across queues or lanes.
//
// Submit never runs a job synchronously, except as a documented last resort:
// after Shutdown(), or if growing a queue fails (out of memory), the job is
// executed inline on the submitting thread so its done counter still drops.
#ifndef SND_JOBS_WORKER_POOL_HPP
#define SND_JOBS_WORKER_POOL_HPP

#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/atomic.hpp"
#include "snd/base/mutex.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

namespace snd::jobs {

struct WorkerPoolConfig {
  std::uint32_t workers = 2;                // Config.jobs.normalWorkers (>= 1)
  std::uint32_t initialQueueCapacity = 64;  // per worker and lane; grows on demand (non-RT)
  bool flushDenormals = true;               // FTZ/DAZ on workers (2.4)
  const char* threadName = "snd-worker";
};

class WorkerPool final : public IJobSystem {
 public:
  WorkerPool() noexcept = default;
  ~WorkerPool() override;
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Non-RT. Starts the workers. false on failure (nothing left running).
  bool Init(IAllocator& allocator, const WorkerPoolConfig& config) noexcept;
  // Non-RT. Stops the workers after the queues are empty. With
  // runPending=false, queued jobs that have not started are completed as
  // cancelled (fn skipped, done counter decremented). Jobs submitted by running
  // jobs during shutdown are handled the same way. Idempotent.
  void Shutdown(bool runPending = true) noexcept;

  // IJobSystem. Any non-RT thread (including this pool's workers).
  void Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) override;

  // Non-RT. Waits until no job is queued or running. false on timeout.
  bool WaitIdle(std::uint64_t timeoutNs = ~0ull) noexcept;

  std::uint32_t Workers() const noexcept { return workerCount_; }
  // Index of the calling thread if it is a worker of this pool, else -1.
  int CurrentWorkerIndex() const noexcept;

  struct Stats {
    std::uint64_t submitted;
    std::uint64_t executed;  // fn called
    std::uint64_t skipped;   // cancelled before start
    std::uint64_t stolen;    // taken from another worker's queue
    std::uint64_t inline_;   // run inline in Submit (after shutdown / OOM)
  };
  Stats GetStats() const noexcept;
  std::uint64_t Pending() const noexcept { return pending_.value.load(std::memory_order_acquire); }

 private:
  struct Entry {
    JobDesc desc;
    std::uint64_t seq;
  };
  struct Queue {
    base::Mutex mutex;
    Entry* heap = nullptr;
    std::uint32_t size = 0;
    std::uint32_t capacity = 0;
  };
  struct Worker {
    WorkerPool* pool = nullptr;
    std::uint32_t index = 0;
    Queue lanes[static_cast<int>(JobLane::Count)];
    platform::Thread thread;
  };

  static void WorkerMain(void* w) noexcept;
  void WorkerLoop(Worker& w) noexcept;
  bool Push(Queue& q, const JobDesc& d) noexcept;  // false on OOM
  bool Pop(Queue& q, Entry& out) noexcept;
  bool FindJob(std::uint32_t self, Entry& out) noexcept;
  void Execute(const Entry& e) noexcept;
  void RunInline(const JobDesc& d) noexcept;

  IAllocator* alloc_ = nullptr;
  WorkerPoolConfig config_{};
  Worker* workers_ = nullptr;
  std::uint32_t workerCount_ = 0;
  platform::Semaphore wake_;
  base::CachePadded<std::atomic<std::uint64_t>> pending_;  // queued, not yet popped
  base::CachePadded<std::atomic<std::int32_t>> sleepers_;
  std::atomic<std::uint32_t> running_{0};
  std::atomic<std::uint32_t> roundRobin_{0};
  std::atomic<std::uint64_t> seq_{0};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> cancelPending_{false};
  std::atomic<bool> shutDown_{false};
  std::atomic<std::uint64_t> submitted_{0}, executed_{0}, skipped_{0}, stolen_{0}, inline_{0};
};

}  // namespace snd::jobs

#endif  // SND_JOBS_WORKER_POOL_HPP
