// Independent re-check of a228470 (D-50: dependants of a copy that waits for
// its old copy's Release also wait; ComputeWillCome is a least fixed point).
// Old copy A is UNLOADING and reloaded as A'; dependants are requested at once.
//  - chains of 4, diamonds, two waiting copies, language copies;
//  - a dependant with one really missing dependency must FAIL (not be marked);
//    banks depending on it fail too (never stuck LOADING);
//  - cycles mixed with waiting copies fail, the waiting copy still loads;
//  - the waiting copy cancelled / the scheduler shut down / Destroy;
//  - Release vs AbortUnload: registration, DEPENDENCY (hash mismatch), ALREADY_LOADED.
#include <cstring>
#include <initializer_list>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
namespace bf = snd::bank_format;

namespace {

fixture::Bank Make(const char* name, std::initializer_list<const char*> deps = {}, std::uint64_t hash = 0x11,
                   std::uint64_t depHash = 0x11, std::uint32_t languageId = 0) {
  fixture::Bank b = indep11::Named(name, languageId);
  b.contentHash = hash;
  for (const char* d : deps) b.deps.push_back(bf::Dependency{HashId(d), 0, depHash});
  return b;
}

// Loads `path` (bank "A"), then starts its unload: the old copy is UNLOADING.
HandleRef LoadedThenUnloading(Env& e, const char* path) {
  const HandleRef old = e.m->RequestLoad(path);
  e.Settle();
  if (e.m->State(old) != SND_BANK_STATE_LOADED) return HandleRef{0, 0};
  if (e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false) != SND_OK) return HandleRef{0, 0};
  return old;
}

bool Loading(Env& e, std::initializer_list<HandleRef> hs) {
  for (const HandleRef& h : hs) {
    if (e.m->State(h) != SND_BANK_STATE_LOADING) return false;
  }
  return true;
}

void ExpectNoneLoading(Env& e, const char* what) {
  for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
    SND_CHECK_MSG(e.m->SlotStates()[s].state.load() != SND_BANK_STATE_LOADING, "%s: slot %u stuck LOADING", what, s);
  }
}

}  // namespace

SND_TEST(IndepWillCome11, ChainOfFourAndDiamondWaitThenLoad) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A").Build());
  e.fs.AddFile("b.sndb", Make("B", {"A"}).Build());
  e.fs.AddFile("c.sndb", Make("C", {"B"}).Build());
  e.fs.AddFile("d.sndb", Make("D", {"C"}).Build());
  e.fs.AddFile("x.sndb", Make("X", {"A"}).Build());
  e.fs.AddFile("y.sndb", Make("Y", {"A"}).Build());
  e.fs.AddFile("z.sndb", Make("Z", {"X", "Y", "D"}).Build());  // diamond over the chain
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef z = e.m->RequestLoad("z.sndb");  // requested before its dependencies
  const HandleRef d = e.m->RequestLoad("d.sndb");
  const HandleRef c = e.m->RequestLoad("c.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  const HandleRef x = e.m->RequestLoad("x.sndb");
  const HandleRef y = e.m->RequestLoad("y.sndb");
  e.Settle(256);
  SND_CHECK_MSG(Loading(e, {a2, b, c, d, x, y, z}), "everything waits for A's Release: %u %u %u %u %u %u %u",
                e.m->State(a2), e.m->State(b), e.m->State(c), e.m->State(d), e.m->State(x), e.m->State(y),
                e.m->State(z));
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(64);
  for (const HandleRef& h : {a2, b, c, d, x, y, z}) {
    SND_CHECK_MSG(e.m->State(h) == SND_BANK_STATE_LOADED, "slot %u: %u (%s)", h.slot, e.m->State(h),
                  e.m->FailureReason(h));
  }
  SND_CHECK_EQ(e.m->DependentCount(a2), 3u);  // B, X, Y
}

SND_TEST(IndepWillCome11, OneReallyMissingDependencyFailsAndPropagates) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A").Build());
  e.fs.AddFile("b.sndb", Make("B", {"A", "Nowhere"}).Build());  // Nowhere is never requested
  e.fs.AddFile("c.sndb", Make("C", {"B"}).Build());
  e.fs.AddFile("ok.sndb", Make("Ok", {"A"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  const HandleRef c = e.m->RequestLoad("c.sndb");
  const HandleRef ok = e.m->RequestLoad("ok.sndb");
  e.Settle(256);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(b), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_FAILED);  // its dependency B can never come
  SND_CHECK_EQ(e.m->Result(c), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK(Loading(e, {a2, ok}));
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(ok), SND_BANK_STATE_LOADED);
  ExpectNoneLoading(e, "missing");
}

SND_TEST(IndepWillCome11, CycleNextToAWaitingCopyFailsTheCycleOnly) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A").Build());
  e.fs.AddFile("p.sndb", Make("P", {"A", "Q"}).Build());
  e.fs.AddFile("q.sndb", Make("Q", {"P"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef p = e.m->RequestLoad("p.sndb");
  const HandleRef q = e.m->RequestLoad("q.sndb");
  e.Settle(256);
  SND_CHECK_EQ(e.m->State(p), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->State(q), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(32);
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_LOADED);
  ExpectNoneLoading(e, "cycle");
}

SND_TEST(IndepWillCome11, TwoWaitingCopiesAndADependant) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  const std::vector<std::uint8_t> a = Make("A").Build();
  e.fs.AddFile("a.sndb", a);
  e.fs.AddFile("mods/a.sndb", a);
  e.fs.AddFile("b.sndb", Make("B", {"A"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("mods/a.sndb");
  const HandleRef a3 = e.m->RequestLoad("a.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  e.Settle(256);
  SND_CHECK(Loading(e, {a2, a3, b}));
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_LOADED);  // request order
  SND_CHECK_EQ(e.m->State(a3), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(a3), SND_ERR_BANK_ALREADY_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(a2), 1u);
}

SND_TEST(IndepWillCome11, LanguageCopyWaitsAndItsDependantWaits) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  const std::uint32_t ja = HashId("ja");
  SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
  e.fs.AddFile("Voice.ja.sndb", Make("Voice", {}, 0x11, 0x11, ja).Build());
  e.fs.AddFile("Talk.sndb", Make("Talk", {"Voice"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "Voice.ja.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef v2 = e.m->RequestLoad("Voice.ja.sndb");
  const HandleRef t = e.m->RequestLoad("Talk.sndb");
  e.Settle(256);
  SND_CHECK(Loading(e, {v2, t}));
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(v2), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(t), SND_BANK_STATE_LOADED);
}

SND_TEST(IndepWillCome11, CancellingTheWaitingCopyEndsTheWaitOfItsDependants) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A").Build());
  e.fs.AddFile("b.sndb", Make("B", {"A"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  e.Settle(256);
  SND_REQUIRE(Loading(e, {a2, b}));
  SND_CHECK_EQ(e.m->BeginUnload(a2, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);  // the copy goes away
  e.Settle(16);
  // A is now only UNLOADING with no copy: B's dependency cannot come (fails, no wait forever).
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(b), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  ExpectNoneLoading(e, "cancel");
}

SND_TEST(IndepWillCome11, AbortUnloadResolvesDependantsAgainstTheOldCopy) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A", {}, 0x11).Build());
  e.fs.AddFile("match.sndb", Make("Match", {"A"}, 0x11, 0x11).Build());
  e.fs.AddFile("other.sndb", Make("Other", {"A"}, 0x11, 0x99).Build());  // built against another A
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef match = e.m->RequestLoad("match.sndb");
  const HandleRef other = e.m->RequestLoad("other.sndb");
  e.Settle(256);
  SND_CHECK(Loading(e, {a2, match, other}));
  e.m->AbortUnload(old);
  e.Settle(32);
  SND_CHECK_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->Result(a2), SND_ERR_BANK_ALREADY_LOADED);
  SND_CHECK_EQ(e.m->State(match), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(old), 1u);
  SND_CHECK_EQ(e.m->State(other), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(other), SND_ERR_BANK_DEPENDENCY);
  ExpectNoneLoading(e, "abort");
}

SND_TEST(IndepWillCome11, ShutdownAndDestroyWhileWaiting) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("a.sndb", Make("A").Build());
  e.fs.AddFile("b.sndb", Make("B", {"A"}).Build());
  e.fs.AddFile("c.sndb", Make("C", {"B"}).Build());
  const HandleRef old = LoadedThenUnloading(e, "a.sndb");
  SND_REQUIRE(!old.IsNone());
  const HandleRef a2 = e.m->RequestLoad("a.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  e.Settle(256);
  e.io.Shutdown();
  const HandleRef c = e.m->RequestLoad("c.sndb");  // cannot be read any more
  e.Settle(8);
  SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_FAILED);
  SND_CHECK(Loading(e, {a2, b}));  // already read: they still wait for A's Release
  SND_CHECK_EQ(e.m->Release(old), SND_OK);
  e.Settle(16);
  SND_CHECK_EQ(e.m->State(a2), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  // Destroy while a new waiting set exists.
  SND_CHECK_EQ(e.m->BeginUnload(b, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->Release(b), SND_OK);
  SND_CHECK_EQ(e.m->BeginUnload(a2, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  BankManager::Destroy(e.m);
  e.m = nullptr;
  e.io.Destroy();
  e.pool.Destroy();
  SND_CHECK_EQ(e.alloc.Live(), 0u);
  SND_CHECK_EQ(e.alloc.BadFrees(), 0u);
}
