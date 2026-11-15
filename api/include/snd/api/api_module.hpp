// api module (design.md 12.1): C API implementation (snd/sonus.h) and composition root; the C++ API is header-only in the contract.
// Dependencies (frozen, root CMakeLists.txt): core, music.
//
// Stage-0 stub. The owner of this module adds its headers under
// api/include/snd/api/ and its sources under api/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_API_API_MODULE_HPP
#define SND_API_API_MODULE_HPP

namespace snd::api {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::api

#endif  // SND_API_API_MODULE_HPP
