// bank module (design.md 12.1): bank loading, validation, registration, release, dependencies.
// Dependencies (frozen, root CMakeLists.txt): base, io.
//
// Stage-0 stub. The owner of this module adds its headers under
// bank/include/snd/bank/ and its sources under bank/src/. Only types from the
// contract (contract/), base and platform may appear in headers here.
#ifndef SND_BANK_BANK_MODULE_HPP
#define SND_BANK_BANK_MODULE_HPP

namespace snd::bank {

// Module identification, used by the smoke test.
const char* ModuleName() noexcept;

}  // namespace snd::bank

#endif  // SND_BANK_BANK_MODULE_HPP
