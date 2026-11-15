// core module (design.md 12.1): tables, commands, systems, voice management, bus graph, 3D, parameters, categories.
// Dependencies (frozen, root CMakeLists.txt): base, jobs, dsp, bank.
//
// Stage-0 stub. The owner of this module adds its headers under
// core/include/snd/core/ and its sources under core/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_CORE_CORE_MODULE_HPP
#define SND_CORE_CORE_MODULE_HPP

namespace snd::core {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::core

#endif  // SND_CORE_CORE_MODULE_HPP
