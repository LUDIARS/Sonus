// Stage-0 smoke test of the core module. Replace / extend with the module's
// unit tests (design.md 14). Mocks: snd/mocks/*.hpp, golden audio:
// snd/test/golden.hpp, RT checks: snd/test/rt_guard.hpp.
#include <cstring>

#include "snd/core/core_module.hpp"
#include "snd/test/test.hpp"

SND_TEST(Core, ModuleLinks) { SND_CHECK(std::strcmp(snd::core::ModuleName(), "core") == 0); }
