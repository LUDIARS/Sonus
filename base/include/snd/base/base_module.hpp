// base module (design.md 12.1): allocators, arenas, pools, handles, SPSC queues, atomics, hashing, lock-free logging.
// Dependencies (frozen, root CMakeLists.txt): (none).
//
// Stage-0 stub. The owner of this module adds its headers under
// base/include/snd/base/ and its sources under base/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_BASE_BASE_MODULE_HPP
#define SND_BASE_BASE_MODULE_HPP

namespace snd::base {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::base

#endif  // SND_BASE_BASE_MODULE_HPP
