// Independent test (re-check of 2743264; acceptance decision: "dependencies are
// matched by bankId AND buildHash, the language is not used for matching";
// outcomes must not depend on slot numbers).
//
// Defect_*: fails on purpose. Two registered copies of bank V (language banks
// ja / en, i.e. the same bankId) with different contentHash. B was built
// against the ja copy (buildHash = ja's contentHash). Decide() takes the FIRST
// registered slot of V in slot order (FindLoaded(bankId, 0)) and only then
// compares the hash: when the en copy sits in the lower slot, B fails with
// SND_ERR_BANK_DEPENDENCY although a copy with the matching hash is
// registered; with the slots the other way round B registers.
// bank_manager.cpp Decide(): FindLoaded(dep.bankId, 0) then the contentHash check.
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
namespace bf = snd::bank_format;

SND_TEST(BankDependencyHash, Defect_MatchingCopyIsFoundWhateverTheSlotOrder) {
  for (int order = 0; order < 2; ++order) {
    Env e(8);
    SND_REQUIRE(e.m != nullptr);
    const std::uint32_t ja = HashId("ja"), en = HashId("en");
    SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
    SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
    fixture::Bank vja = indep11::Named("V", ja);
    vja.contentHash = 0xA1;
    fixture::Bank ven = indep11::Named("V", en);
    ven.contentHash = 0xE2;
    fixture::Bank b = indep11::Named("B");
    b.deps.push_back(bf::Dependency{HashId("V"), 0, 0xA1});  // built against V.ja
    e.fs.AddFile("V.ja.sndb", vja.Build());
    e.fs.AddFile("V.en.sndb", ven.Build());
    e.fs.AddFile("b.sndb", b.Build());
    const HandleRef first = e.m->RequestLoad(order == 0 ? "V.en.sndb" : "V.ja.sndb");
    const HandleRef second = e.m->RequestLoad(order == 0 ? "V.ja.sndb" : "V.en.sndb");
    e.Settle();
    SND_REQUIRE_EQ(e.m->State(first), SND_BANK_STATE_LOADED);
    SND_REQUIRE_EQ(e.m->State(second), SND_BANK_STATE_LOADED);
    const HandleRef hb = e.m->RequestLoad("b.sndb");
    e.Settle();
    SND_CHECK_MSG(e.m->State(hb) == SND_BANK_STATE_LOADED,
                  "order %d (%s copy in the lower slot): B ended %u (%d: %s) although V.ja with the matching hash is "
                  "registered",
                  order, order == 0 ? "en" : "ja", e.m->State(hb), e.m->Result(hb), e.m->FailureReason(hb));
  }
}
