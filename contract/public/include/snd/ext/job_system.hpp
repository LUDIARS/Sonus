// IJobSystem (design.md 2.3, 12.3). FROZEN CONTRACT.
#ifndef SND_EXT_JOB_SYSTEM_HPP
#define SND_EXT_JOB_SYSTEM_HPP

#include <cstdint>

#include "snd/ext.h"
#include "snd/ext/types.hpp"

namespace snd {

// Normal-worker jobs only. RT work (render chunks) is never submitted here.
//
// Contract:
//  - Submit may be called from any thread except the audio thread and RT
//    workers (those post requests through lock-free queues to a non-RT thread;
//    CONTRACTS.md D-08).
//  - The descriptors are copied; data, done_counter and cancel_flag must stay
//    valid until the done counter has been decremented.
//  - Each job runs at most once. If cancel_flag reads non-zero before it
//    starts, fn is skipped. done_counter is decremented exactly once in both
//    cases (use snd::RunJob from snd/ext/types.hpp).
//  - Jobs of one lane may run concurrently and in any order unless the
//    implementation states otherwise; priority is a hint (higher first).
struct IJobSystem {
  virtual ~IJobSystem() = default;
  virtual void Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) = 0;
};

namespace bridge {
inline void SND_CALL JobSystemSubmit(void* u, const snd_job_desc* jobs, std::uint32_t count,
                                     snd_job_lane lane) {
  static_cast<IJobSystem*>(u)->Submit(jobs, count, static_cast<JobLane>(lane));
}
}  // namespace bridge

inline snd_job_system ToC(IJobSystem& j) noexcept {
  return snd_job_system{&j, &bridge::JobSystemSubmit};
}

class CJobSystem final : public IJobSystem {
 public:
  explicit CJobSystem(const snd_job_system& c) noexcept : c_(c) {}
  void Submit(const JobDesc* jobs, std::uint32_t count, JobLane lane) override {
    c_.submit(c_.user, jobs, count, static_cast<snd_job_lane>(lane));
  }

 private:
  snd_job_system c_;
};

}  // namespace snd

#endif  // SND_EXT_JOB_SYSTEM_HPP
