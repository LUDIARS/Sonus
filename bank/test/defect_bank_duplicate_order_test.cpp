// Independent test (contract 1.1, D-50 / snd_load_bank_async):
//   "the same bank (bankId + language) already loaded or loading from ANOTHER
//    path fails the NEW load with SND_ERR_BANK_ALREADY_LOADED (the first one stays)".
//
// Defect_*: fails on purpose. Two paths of the same bank whose validations end
// in the same Pump(): BankManager decides by SLOT INDEX (Decide() only treats
// a Validated slot as "earlier" when it has a lower slot index), so when the
// first request sits in a higher slot (slots are reused) the FIRST load fails
// with SND_ERR_BANK_ALREADY_LOADED and the later one is registered.
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using namespace snd::bank;
using indep11::Env;
using indep11::Named;

SND_TEST(BankDuplicate, Defect_FirstRequestedLoadMustWinWhenBothValidateTogether) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("filler.sndb", Named("Filler").Build());
  const std::vector<std::uint8_t> img = Named("Dup").Build();
  e.fs.AddFile("first/dup.sndb", img);
  e.fs.AddFile("second/dup.sndb", img);
  // Slot 0 is taken by a filler bank so that the first request gets slot 1 ...
  const HandleRef filler = e.m->RequestLoad("filler.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(filler), SND_BANK_STATE_LOADED);
  const HandleRef first = e.m->RequestLoad("first/dup.sndb");
  // ... and the filler goes away, so the second request gets slot 0.
  SND_REQUIRE_EQ(e.m->BeginUnload(filler, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_REQUIRE_EQ(e.m->Release(filler), SND_OK);
  const HandleRef second = e.m->RequestLoad("second/dup.sndb");
  SND_REQUIRE_MSG(second.slot < first.slot, "slots %u / %u: setup did not reuse the lower slot", second.slot,
                  first.slot);
  e.Settle();
  SND_CHECK_MSG(e.m->State(first) == SND_BANK_STATE_LOADED,
                "the first load ended %u (%d: %s) although it was requested first", e.m->State(first),
                e.m->Result(first), e.m->FailureReason(first));
  SND_CHECK_MSG(e.m->State(second) == SND_BANK_STATE_FAILED && e.m->Result(second) == SND_ERR_BANK_ALREADY_LOADED,
                "the later load ended %u (%d)", e.m->State(second), e.m->Result(second));
}
