// Independent re-check of the bank fixes in c65fd9a (decisions C, D, E):
//  C  duplicates are decided in RequestLoad acceptance order; a later request
//     waits while an earlier one is reading / validating / deciding. Checked
//     with every way the earlier one can end: success, open failure, read
//     failure, cancel (before / during the read, while validated), dependency
//     failure, IoScheduler::Shutdown, and the side effect on unrelated banks.
//  D  while an old copy is UNLOADING, new copies wait for its Release; AbortUnload
//     (IF_UNUSED refused) makes them fail ALREADY_LOADED instead of waiting
//     forever; a chain of waiting copies resolves to exactly one.
//  E  BeginUnload's dependency check and change happen under one lock.
// Nothing may stay LOADING forever and nothing may leak.
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
using indep11::Named;
namespace bf = snd::bank_format;

namespace {
fixture::Bank WithDep(const std::string& name, const char* dep, std::uint64_t hash = 0x11) {
  fixture::Bank b = Named(name);
  b.contentHash = hash;
  b.deps.push_back(bf::Dependency{HashId(dep), 0, 0x11});
  return b;
}
bool Final(snd_bank_state s) { return s != SND_BANK_STATE_LOADING; }
}  // namespace

// C: the earlier request fails in every way -> the later copy registers.
SND_TEST(IndepBankOrder11, LaterCopyRegistersWhenTheEarlierOneFails) {
  for (int how = 0; how < 6; ++how) {
    Env e;
    SND_REQUIRE(e.m != nullptr);
    const std::vector<std::uint8_t> img = Named("Dup").Build();
    e.fs.AddFile("first.sndb", how == 5 ? WithDep("Dup", "Missing", 0x22).Build() : img);
    e.fs.AddFile("second.sndb", img);
    if (how == 0) e.fs.FailOpen("first.sndb");
    if (how == 1) e.fs.FailReads("first.sndb", 2000, 3000);
    if (how >= 2 && how <= 4) e.fs.Hold("first.sndb");
    const HandleRef first = e.m->RequestLoad("first.sndb");
    const HandleRef second = e.m->RequestLoad("second.sndb");
    e.Settle(32);
    if (how == 2) {  // cancel the held first load (its read may be in flight)
      SND_CHECK_EQ(e.m->State(second), SND_BANK_STATE_LOADING);  // waits for the earlier request
      SND_CHECK_EQ(e.m->BeginUnload(first, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
      e.Settle(8);
      SND_CHECK_MSG(e.m->State(second) == SND_BANK_STATE_LOADED,
                    "a cancelled earlier request must not block: %u", e.m->State(second));
      e.fs.Release("first.sndb");
    } else if (how == 3) {  // Shutdown while the first is held: it ends FAILED, the second was read already
      SND_CHECK_EQ(e.m->State(second), SND_BANK_STATE_LOADING);
      e.fs.Release("first.sndb");  // Shutdown waits for reads in flight
      e.io.Shutdown();
    } else if (how == 4) {  // control case: the first one just reads late and then succeeds
      e.fs.Release("first.sndb");
      e.Settle(64);
    }
    e.Settle(64);
    SND_CHECK_MSG(Final(e.m->State(first)), "how %d: first stuck LOADING", how);
    SND_CHECK_MSG(Final(e.m->State(second)), "how %d: second stuck LOADING", how);
    if (how == 4) {
      // Both read fine: request order -> first wins.
      SND_CHECK_EQ(e.m->State(first), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->Result(second), SND_ERR_BANK_ALREADY_LOADED);
      continue;
    }
    if (how != 2) SND_CHECK_MSG(e.m->State(first) == SND_BANK_STATE_FAILED, "how %d: first %u", how, e.m->State(first));
    SND_CHECK_MSG(e.m->State(second) == SND_BANK_STATE_LOADED, "how %d: second %u (%d: %s)", how, e.m->State(second),
                  e.m->Result(second), e.m->FailureReason(second));
  }
}

// C: the earlier copy is validated but cannot decide yet (its dependency is still
// loading); the later copy (no dependency) waits; when the dependency arrives the
// earlier one wins.
SND_TEST(IndepBankOrder11, LaterCopyWaitsForAnEarlierCopyThatWaitsForADependency) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("first.sndb", WithDep("Dup", "Base", 0x22).Build());
  e.fs.AddFile("second.sndb", Named("Dup").Build());
  e.fs.AddFile("base.sndb", Named("Base").Build());
  e.fs.Hold("base.sndb");
  const HandleRef first = e.m->RequestLoad("first.sndb");
  const HandleRef second = e.m->RequestLoad("second.sndb");
  const HandleRef base = e.m->RequestLoad("base.sndb");
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(first), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(e.m->State(second), SND_BANK_STATE_LOADING);
  e.fs.Release("base.sndb");
  e.Settle(128);
  SND_CHECK_EQ(e.m->State(base), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(first), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(second), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(second), SND_ERR_BANK_ALREADY_LOADED);
}

// The documented side effect: an UNRELATED later bank waits for an earlier load
// that is still reading. It is bounded by that load: it resolves when the
// earlier read ends, or at once when the earlier load is cancelled.
SND_TEST(IndepBankOrder11, UnrelatedBankWaitsOnlyAsLongAsTheEarlierRead) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("big.sndb", Named("Big").Build());
  e.fs.AddFile("small.sndb", Named("Small").Build());
  e.fs.Hold("big.sndb");
  const HandleRef big = e.m->RequestLoad("big.sndb");
  const HandleRef small = e.m->RequestLoad("small.sndb");
  e.Settle(64);
  const snd_bank_state whileHeld = e.m->State(small);
  snd::test::Note("unrelated bank while an earlier read is held: state %u", whileHeld);
  SND_CHECK_EQ(whileHeld, SND_BANK_STATE_LOADING);  // decision C: it waits (identity of the earlier unknown)
  SND_CHECK_EQ(e.m->BeginUnload(big, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  e.Settle(4);
  SND_CHECK_EQ(e.m->State(small), SND_BANK_STATE_LOADED);  // a cancelled earlier read no longer blocks
  e.fs.Release("big.sndb");
  e.Settle(16);
  SND_CHECK_EQ(e.m->State(big), SND_BANK_STATE_UNLOADED);
  // A later request does not delay an earlier one.
  e.fs.Hold("late.sndb");
  e.fs.AddFile("late.sndb", Named("Late").Build());
  e.fs.AddFile("early.sndb", Named("Early").Build());
  const HandleRef early = e.m->RequestLoad("early.sndb");
  const HandleRef late = e.m->RequestLoad("late.sndb");
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(early), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(late), SND_BANK_STATE_LOADING);
  e.fs.Release("late.sndb");
  e.Settle(64);
  SND_CHECK_EQ(e.m->State(late), SND_BANK_STATE_LOADED);
}

// D: the core refuses the unload (AbortUnload): the waiting copy must not wait forever.
SND_TEST(IndepBankOrder11, AbortUnloadTurnsTheWaitIntoAlreadyLoaded) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::vector<std::uint8_t> img = Named("Lvl").Build();
  e.fs.AddFile("lvl.sndb", img);
  e.fs.AddFile("dlc/lvl.sndb", img);
  const HandleRef old = e.m->RequestLoad("lvl.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
  SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_IF_UNUSED, false), SND_OK);
  const HandleRef same = e.m->RequestLoad("lvl.sndb");
  const HandleRef other = e.m->RequestLoad("dlc/lvl.sndb");
  e.Settle(128);
  SND_CHECK_EQ(e.m->State(same), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(e.m->State(other), SND_BANK_STATE_LOADING);
  e.m->AbortUnload(old);  // the audio side found references
  e.Settle(16);
  SND_CHECK_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
  SND_CHECK_MSG(e.m->State(same) == SND_BANK_STATE_FAILED && e.m->Result(same) == SND_ERR_BANK_ALREADY_LOADED,
                "same path after AbortUnload: %u (%d)", e.m->State(same), e.m->Result(same));
  SND_CHECK_MSG(e.m->State(other) == SND_BANK_STATE_FAILED && e.m->Result(other) == SND_ERR_BANK_ALREADY_LOADED,
                "other path after AbortUnload: %u (%d)", e.m->State(other), e.m->Result(other));
}

// D: two waiting copies; after the Release exactly one registers (the earlier).
// A waiting copy can be cancelled without leaking.
SND_TEST(IndepBankOrder11, ChainOfWaitingCopiesResolvesToOne) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  const std::size_t base = e.alloc.Live();
  const std::vector<std::uint8_t> img = Named("C").Build();
  for (const char* p : {"c0.sndb", "c1.sndb", "c2.sndb", "c3.sndb"}) e.fs.AddFile(p, img);
  const HandleRef c0 = e.m->RequestLoad("c0.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->BeginUnload(c0, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  const HandleRef c1 = e.m->RequestLoad("c1.sndb");
  const HandleRef c2 = e.m->RequestLoad("c2.sndb");
  const HandleRef c3 = e.m->RequestLoad("c3.sndb");
  e.Settle(128);
  for (const HandleRef& h : {c1, c2, c3}) SND_CHECK_EQ(e.m->State(h), SND_BANK_STATE_LOADING);
  SND_CHECK_EQ(e.m->BeginUnload(c1, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);  // the earliest waiter goes away
  e.Settle(8);
  SND_CHECK_EQ(e.m->State(c1), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(e.m->State(c2), SND_BANK_STATE_LOADING);  // still waits for c0's Release
  SND_CHECK_EQ(e.m->Release(c0), SND_OK);
  e.Settle(32);
  SND_CHECK_EQ(e.m->State(c2), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(c3), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(e.m->Result(c3), SND_ERR_BANK_ALREADY_LOADED);
  for (const HandleRef& h : {c2, c3}) {
    if (e.m->BeginUnload(h, SND_UNLOAD_STOP_AND_UNLOAD, false) == SND_OK && e.m->State(h) == SND_BANK_STATE_UNLOADING) {
      SND_CHECK_EQ(e.m->Release(h), SND_OK);
    }
  }
  SND_CHECK_EQ(e.alloc.Live(), base);
}

// D + teardown: Destroy while copies wait for an UNLOADING slot (no Release ever).
SND_TEST(IndepBankOrder11, DestroyWithWaitingCopiesDoesNotLeak) {
  testsupport::CountingAllocator* alloc = nullptr;
  {
    Env e;
    SND_REQUIRE(e.m != nullptr);
    alloc = nullptr;
    e.fs.AddFile("w.sndb", Named("W").Build());
    e.fs.AddFile("w2.sndb", Named("W").Build());
    const HandleRef w = e.m->RequestLoad("w.sndb");
    e.Settle();
    SND_REQUIRE_EQ(e.m->BeginUnload(w, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    (void)e.m->RequestLoad("w2.sndb");
    (void)e.m->RequestLoad("w.sndb");
    e.Settle(64);
    e.io.Shutdown();
    BankManager::Destroy(e.m);
    e.m = nullptr;
    e.io.Destroy();
    e.pool.Destroy();
    SND_CHECK_EQ(e.alloc.Live(), 0u);
    SND_CHECK_EQ(e.alloc.BadFrees(), 0u);
  }
  (void)alloc;
}

// E: a bank with a registered dependant cannot be unloaded, in both modes, and the
// check follows the dependant's own state (loading dependants do not count yet).
SND_TEST(IndepBankOrder11, UnloadDependencyCheck) {
  Env e;
  SND_REQUIRE(e.m != nullptr);
  e.fs.AddFile("base.sndb", Named("Base").Build());
  e.fs.AddFile("child.sndb", WithDep("Child", "Base").Build());
  const HandleRef base = e.m->RequestLoad("base.sndb");
  const HandleRef child = e.m->RequestLoad("child.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(child), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(base), 1u);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_IF_UNUSED, false), SND_ERR_BANK_IN_USE);
  SND_CHECK_EQ(e.m->State(base), SND_BANK_STATE_LOADED);  // a refused BeginUnload changes nothing
  SND_CHECK_EQ(e.m->BeginUnload(child, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);  // child UNLOADING
  SND_CHECK_EQ(e.m->Release(child), SND_OK);
  SND_CHECK_EQ(e.m->DependentCount(base), 0u);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_IF_UNUSED, true), SND_ERR_BANK_IN_USE);  // referenced by playbacks
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, true), SND_OK);
  SND_CHECK_EQ(e.m->Release(base), SND_OK);
  SND_CHECK_EQ(e.m->BeginUnload(base, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(e.m->BeginUnload(child, 7u, false), SND_ERR_INVALID_HANDLE);  // stale before the bad mode
}
