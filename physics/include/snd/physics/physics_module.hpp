// physics module (design.md 12.1): contact culling, material tables, footsteps (snd/physics.h), public API only.
// Dependencies (frozen, root CMakeLists.txt): api (public API only).
//
// Stage-0 stub. The owner of this module adds its headers under
// physics/include/snd/physics/ and its sources under physics/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_PHYSICS_PHYSICS_MODULE_HPP
#define SND_PHYSICS_PHYSICS_MODULE_HPP

namespace snd::physics {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::physics

#endif  // SND_PHYSICS_PHYSICS_MODULE_HPP
