// livelink module (design.md 12.1): protocol, override layer, profiler stream, recording.
// Dependencies (frozen, root CMakeLists.txt): core.
//
// Stage-0 stub. The owner of this module adds its headers under
// livelink/include/snd/livelink/ and its sources under livelink/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_LIVELINK_LIVELINK_MODULE_HPP
#define SND_LIVELINK_LIVELINK_MODULE_HPP

namespace snd::livelink {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::livelink

#endif  // SND_LIVELINK_LIVELINK_MODULE_HPP
