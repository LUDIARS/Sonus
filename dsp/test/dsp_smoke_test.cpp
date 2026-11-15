// Stage-0 smoke test of the dsp module. Replace / extend with the module's
// unit tests (design.md 14). Mocks: snd/mocks/*.hpp, golden audio:
// snd/test/golden.hpp, RT checks: snd/test/rt_guard.hpp.
#include <cstring>

#include "snd/dsp/dsp_module.hpp"
#include "snd/test/test.hpp"

SND_TEST(Dsp, ModuleLinks) { SND_CHECK(std::strcmp(snd::dsp::ModuleName(), "dsp") == 0); }
