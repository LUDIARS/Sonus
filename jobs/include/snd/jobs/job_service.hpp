// Job system selection and the RT -> non-RT job pump (design.md 2.3, CONTRACTS.md D-08).
//
// JobService: picks the normal-worker job system once at Create:
//   - Config.jobs.external != NULL  -> the host's job system (C table, copied;
//     D-06), wrapped as snd::IJobSystem. Only normal-worker jobs go there; RT
//     work always stays on RtWorkerPool (2.3).
//   - otherwise                      -> an owned WorkerPool with
//     Config.jobs.normalWorkers threads.
//
// JobPump: the bounded lock-free queue behind SourceServices::postJob (D-08).
// The audio thread and RT workers Post() (RT-safe: no lock, no allocation; a
// semaphore post wakes the pump thread if it runs). A non-RT thread calls
// Pump() — or StartThread() runs one — which forwards the posted jobs to the
// target IJobSystem in posting order, batching consecutive jobs of the same lane.
#ifndef SND_JOBS_JOB_SERVICE_HPP
#define SND_JOBS_JOB_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "snd/base/allocator.hpp"
#include "snd/base/mpmc.hpp"
#include "snd/ext/job_system.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/platform/semaphore.hpp"
#include "snd/platform/thread.hpp"

namespace snd::jobs {

// IJobSystem over a host C table (the copy of Config.jobs.external). Submit
// forwards directly; thread rules are the host's (never the audio thread, D-08).
class ExternalJobSystem final : public IJobSystem {
 public:
  ExternalJobSystem() noexcept = default;
  explicit ExternalJobSystem(const snd_job_system& table) noexcept : table_(table) {}
  void Reset(const snd_job_system& table) noexcept { table_ = table; }
  void Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) override {
    if (table_.submit != nullptr) table_.submit(table_.user, jobs, count, static_cast<snd_job_lane>(lane));
  }
  bool Valid() const noexcept { return table_.submit != nullptr; }

 private:
  snd_job_system table_{};
};

class JobService {
 public:
  JobService() noexcept = default;
  ~JobService() { Shutdown(); }
  JobService(const JobService&) = delete;
  JobService& operator=(const JobService&) = delete;

  // Non-RT. `external` may be null (own workers). normalWorkers is ignored
  // when external is given (Config.jobs.normalWorkers comment).
  bool Init(IAllocator& allocator, const snd_job_system* external, std::uint32_t normalWorkers) noexcept;
  // Non-RT. Stops own workers (pending jobs still run). The external system is not touched.
  void Shutdown() noexcept;

  // Valid between Init and Shutdown. Same thread rules as IJobSystem.
  IJobSystem& Jobs() noexcept;
  bool UsesExternal() const noexcept { return usesExternal_; }
  WorkerPool* OwnPool() noexcept { return usesExternal_ || !initialized_ ? nullptr : &pool_; }

 private:
  WorkerPool pool_;
  ExternalJobSystem external_;
  bool usesExternal_ = false;
  bool initialized_ = false;
};

class JobPump {
 public:
  JobPump() noexcept = default;
  ~JobPump() { Destroy(); }
  JobPump(const JobPump&) = delete;
  JobPump& operator=(const JobPump&) = delete;

  // Non-RT. capacity is rounded up to a power of two.
  bool Init(IAllocator& allocator, std::uint32_t capacity, IJobSystem& target) noexcept;
  // Non-RT. Stops the pump thread, forwards what is still queued, frees memory.
  void Destroy() noexcept;

  // Any thread, RT-safe. false = queue full (the caller retries next block);
  // the job was not taken and counted in Rejected().
  bool Post(const JobDesc& job, JobLane lane) noexcept;
  // Signature of SourceServices::postJob (ctx = JobPump*).
  static bool PostThunk(void* ctx, const JobDesc& job, JobLane lane) noexcept {
    return static_cast<JobPump*>(ctx)->Post(job, lane);
  }

  // Non-RT thread: forwards up to `max` posted jobs to the target. Returns the count.
  std::uint32_t Pump(std::uint32_t max = 0xFFFFFFFFu) noexcept;
  // Non-RT. Runs Pump() on an own thread woken by Post (and every `pollMs` as a safety net).
  bool StartThread(std::uint32_t pollMs = 10) noexcept;
  void StopThread() noexcept;

  std::uint64_t Posted() const noexcept { return posted_.load(std::memory_order_relaxed); }
  std::uint64_t Rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }
  // Jobs handed to the target (counted just before each Submit).
  std::uint64_t Forwarded() const noexcept { return forwarded_.load(std::memory_order_acquire); }

 private:
  struct PostedJob {
    JobDesc desc;
    std::uint32_t lane;
    std::uint32_t reserved;
  };
  static void ThreadMain(void* self) noexcept;

  base::MpmcRing<PostedJob> queue_;
  IJobSystem* target_ = nullptr;
  platform::Semaphore wake_;
  platform::Thread thread_;
  std::atomic<bool> threadRunning_{false};
  std::atomic<bool> pumping_{false};  // Pump() is single-consumer at a time
  std::uint32_t pollMs_ = 10;
  std::atomic<std::uint64_t> posted_{0}, rejected_{0}, forwarded_{0};
};

}  // namespace snd::jobs

#endif  // SND_JOBS_JOB_SERVICE_HPP
