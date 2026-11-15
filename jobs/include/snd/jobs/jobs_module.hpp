// jobs module (design.md 12.1): RT workers and chunk deadlines, normal workers and lanes, IJobSystem adapter.
// Dependencies (frozen, root CMakeLists.txt): base, platform.
//
// Stage-0 stub. The owner of this module adds its headers under
// jobs/include/snd/jobs/ and its sources under jobs/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_JOBS_JOBS_MODULE_HPP
#define SND_JOBS_JOBS_MODULE_HPP

namespace snd::jobs {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::jobs

#endif  // SND_JOBS_JOBS_MODULE_HPP
