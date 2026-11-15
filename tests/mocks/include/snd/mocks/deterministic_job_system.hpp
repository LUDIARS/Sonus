// Deterministic job system (design.md 12.2-6).
//
// Implements snd::IJobSystem with two modes:
//   Immediate - Submit() runs the jobs synchronously, in submission order,
//               before returning. Jobs submitted from inside a running job are
//               appended and run after it (breadth-first), so the global order
//               is always the submission order.
//   Manual    - Submit() only enqueues. The test advances one job at a time
//               (RunNext), per lane, or drains the queue (RunAll).
// Lanes are kept separately but ordered by one global submission counter;
// priorities are recorded but ignored (strict FIFO keeps runs reproducible).
// Jobs follow the JobDesc contract via snd::RunJob (cancel flag, done counter).
// Intended for single-threaded tests: with concurrent submitters in Immediate
// mode, jobs may run on whichever thread is currently draining the queue.
#ifndef SND_MOCKS_DETERMINISTIC_JOB_SYSTEM_HPP
#define SND_MOCKS_DETERMINISTIC_JOB_SYSTEM_HPP

#include <cstdint>
#include <deque>
#include <mutex>

#include "snd/ext/job_system.hpp"

namespace snd::mock {

enum class JobMode { Immediate, Manual };

class DeterministicJobSystem final : public IJobSystem {
 public:
  explicit DeterministicJobSystem(JobMode mode = JobMode::Immediate);
  ~DeterministicJobSystem() override;

  void Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) override;

  void SetMode(JobMode mode);  // switching to Immediate drains the queue
  JobMode Mode() const;

  std::size_t Pending() const;
  std::size_t Pending(JobLane lane) const;
  bool RunNext();              // oldest job of any lane
  bool RunNext(JobLane lane);  // oldest job of `lane`
  std::size_t RunAll();        // until empty, including jobs submitted meanwhile

  std::uint64_t SubmittedCount() const;
  std::uint64_t ExecutedCount() const;  // fn called
  std::uint64_t SkippedCount() const;   // cancelled before start

 private:
  struct Entry {
    JobDesc desc;
    JobLane lane;
    std::uint64_t order;
  };
  bool PopLocked(int lane, Entry* out);
  void Execute(const Entry& e);
  void DrainImmediate();

  mutable std::mutex mutex_;
  std::deque<Entry> queues_[static_cast<int>(JobLane::Count)];
  JobMode mode_;
  bool draining_ = false;
  std::uint64_t nextOrder_ = 0;
  std::uint64_t submitted_ = 0;
  std::uint64_t executed_ = 0;
  std::uint64_t skipped_ = 0;
};

}  // namespace snd::mock

#endif  // SND_MOCKS_DETERMINISTIC_JOB_SYSTEM_HPP
