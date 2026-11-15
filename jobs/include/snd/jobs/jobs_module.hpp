// jobs module (design.md 12.1): RT workers and chunk deadlines, normal workers and lanes, IJobSystem adapter.
// Dependencies (frozen, root CMakeLists.txt): base, platform.
//
// Headers (all under snd/jobs/):
//   rt_workers.hpp   RtWorkerPool: per-block chunk render with Pending/Taken/Done(Late) states,
//                    bounded spin deadline, generation-discarded late results (2.3). RT-safe Run().
//   worker_pool.hpp  WorkerPool: IJobSystem, 2 lanes, priorities, work stealing (normal workers).
//   job_service.hpp  JobService (own WorkerPool or Config.jobs.external), ExternalJobSystem,
//                    JobPump (RT-safe postJob queue -> IJobSystem, D-08).
#ifndef SND_JOBS_JOBS_MODULE_HPP
#define SND_JOBS_JOBS_MODULE_HPP

namespace snd::jobs {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::jobs

#endif  // SND_JOBS_JOBS_MODULE_HPP
