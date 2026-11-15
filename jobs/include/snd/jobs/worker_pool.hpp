// Normal workers: IJobSystem with two lanes and work stealing (design.md 2.3).
//
// WorkerPool implements snd::IJobSystem for normal-worker jobs (decode,
// bank validation, IR conversion, ...). It is NOT used for RT work
// (rt_workers.hpp) and must not be called from the audio thread or RT
// workers (they use JobPump, D-08) — Submit takes a mutex and may allocate,
// both of which the RT checks report.
//
// Scheduling (ext.h: "within a lane: higher runs first, FIFO among equals"):
//  - Every worker owns one queue per lane (Prefetch, Background); a queue is a
//    priority heap ordered by (priority desc, submission order asc). Submit
//    from a worker of this pool pushes to that worker's own queue; other
//    threads distribute round-robin.
//  - Lane-wide priority: an idle worker looks at the heads of ALL queues of
//    the lane and takes the best one (its own or another worker's: work
//    stealing). So a worker never starts a lower-priority job of its own queue
//    while a higher-priority job of the same lane waits in another queue.
//    Two workers racing for the same head can produce a momentary inversion
//    (one of them falls back to the next best head); nothing else does.
//    Submission order is global, so FIFO among equal priorities holds across
//    queues as well.
//  - Lanes: Prefetch (decode, latency sensitive) is served first. To keep the
//    Background lane from starving, every worker that has run
//    `prefetchBurst` (default 8) Prefetch jobs in a row runs one waiting
//    Background job next, whatever the number of workers.
//  - Jobs follow the JobDesc contract through snd::RunJob: each runs at most
//    once, a set cancel flag skips fn, the done counter is decremented exactly
//    once either way.
//
// Threads: Submit may be called from any non-RT thread, concurrently with
// everything including Shutdown(). A Submit that overlaps Shutdown either
// queues the jobs before the pool stops (they run, or are completed as
// cancelled with Shutdown(false)) or runs them inline; in every case each
// job's done counter drops exactly once. Init/Shutdown themselves are called
// by the owner thread only (never concurrently with each other).
//
// Submit never runs a job synchronously, except as a documented last resort:
// before Init / during and after Shutdown (from a thread that is not one of
// this pool's workers), or if growing a queue fails (out of memory), the job
// is executed inline on the submitting thread so its done counter still drops.
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
  // After this many Prefetch jobs in a row, a worker runs one waiting
  // Background job (0 = treated as 1).
  std::uint32_t prefetchBurst = 8;
};

class WorkerPool final : public IJobSystem {
 public:
  WorkerPool() noexcept = default;
  ~WorkerPool() override;
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Non-RT, owner thread. Starts the workers. false on failure (nothing left running).
  bool Init(IAllocator& allocator, const WorkerPoolConfig& config) noexcept;
  // Non-RT, owner thread. Stops the workers after the queues are empty. With
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
    std::uint64_t inline_;   // run inline in Submit (before Init / during or after Shutdown / OOM)
  };
  Stats GetStats() const noexcept;
  // Jobs queued and not yet taken by a worker (approximate while busy).
  std::uint64_t Pending() const noexcept {
    const std::int64_t p = pending_.value.load(std::memory_order_acquire);
    return p > 0 ? static_cast<std::uint64_t>(p) : 0u;
  }

 private:
  static constexpr int kLanes = static_cast<int>(JobLane::Count);
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
    Queue lanes[kLanes];
    platform::Thread thread;
  };

  static void WorkerMain(void* w) noexcept;
  void WorkerLoop(Worker& w) noexcept;
  bool Push(Queue& q, const JobDesc& d) noexcept;  // false on OOM
  bool Pop(Queue& q, int lane, Entry& out) noexcept;
  bool Peek(Queue& q, Entry& out) noexcept;
  bool TakeBest(std::uint32_t self, int lane, Entry& out) noexcept;
  bool FindJob(std::uint32_t self, std::uint32_t& prefetchStreak, Entry& out) noexcept;
  void Execute(const Entry& e) noexcept;
  void RunInline(const JobDesc& d) noexcept;

  IAllocator* alloc_ = nullptr;
  WorkerPoolConfig config_{};
  Worker* workers_ = nullptr;
  std::uint32_t workerCount_ = 0;
  platform::Semaphore wake_;
  base::CachePadded<std::atomic<std::int64_t>> pending_;  // queued, not yet popped
  std::atomic<std::int64_t> lanePending_[kLanes] = {};
  base::CachePadded<std::atomic<std::int32_t>> sleepers_;
  base::CachePadded<std::atomic<std::uint32_t>> submitsInFlight_;  // Submit <-> Shutdown handshake
  std::atomic<std::uint32_t> running_{0};
  std::atomic<std::uint32_t> roundRobin_{0};
  std::atomic<std::uint64_t> seq_{0};
  std::atomic<bool> stopping_{true};  // true while not initialised
  std::atomic<bool> cancelPending_{false};
  std::atomic<std::uint64_t> submitted_{0}, executed_{0}, skipped_{0}, stolen_{0}, inline_{0};
};

}  // namespace snd::jobs

#endif  // SND_JOBS_WORKER_POOL_HPP
