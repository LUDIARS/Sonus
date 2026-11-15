// platform module (design.md 12.1): output devices, thread priority and work groups, clocks, FTZ/DAZ, SIMD abstraction.
// Dependencies (frozen, root CMakeLists.txt): base.
//
// Stage-0 stub. The owner of this module adds its headers under
// platform/include/snd/platform/ and its sources under platform/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_PLATFORM_PLATFORM_MODULE_HPP
#define SND_PLATFORM_PLATFORM_MODULE_HPP

namespace snd::platform {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::platform

#endif  // SND_PLATFORM_PLATFORM_MODULE_HPP
