// Independent test (contract 1.1, D-50): SND_ERR_BANK_ALREADY_LOADED is for
// the same bank loaded or loading "from ANOTHER path". Reloading the SAME path
// right after UnloadBank (the old slot is UNLOADING until core's epoch passes
// and it calls Release) is not that case.
//
// Defect_*: fails on purpose. RequestLoad skips the UNLOADING slot of the same
// path and starts a new load; Decide() then counts the UNLOADING slot as an
// earlier copy of the bank and fails the reload with
// SND_ERR_BANK_ALREADY_LOADED. A game that unloads and reloads a bank in the
// same frame (or before the epoch) gets a failure naming its own path.
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using namespace snd::bank;
using indep11::Env;
using indep11::Named;

SND_TEST(BankReload, Defect_ReloadOfTheSamePathWhileTheOldSlotIsUnloading) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("level.sndb", Named("Level").Build());
  const HandleRef old = e.m->RequestLoad("level.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);  // core unregisters ...
  const HandleRef again = e.m->RequestLoad("level.sndb");
  SND_REQUIRE(!again.IsNone());
  e.Settle(256);  // ... the reload is read and validated meanwhile
  const snd_result early = e.m->Result(again);
  SND_CHECK_EQ(e.m->Release(old), SND_OK);  // ... and the epoch passes
  e.Settle();
  SND_CHECK_MSG(early != SND_ERR_BANK_ALREADY_LOADED && e.m->State(again) == SND_BANK_STATE_LOADED,
                "reload of the same path ended %u (%d: %s)", e.m->State(again), e.m->Result(again),
                e.m->FailureReason(again));
}
