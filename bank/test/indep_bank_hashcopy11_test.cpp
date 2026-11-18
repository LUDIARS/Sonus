// Independent re-check of 7e2155b (decision: a dependency resolves to a
// registered copy whose bankId AND content hash match, whatever its slot;
// several matches: the copy requested first; no match yet: wait; DEPENDENCY
// only once no copy of that bankId can come any more).
//  - matching and mismatching copies mixed (language copies V.ja / V.en);
//  - several matching copies: the reference (DependentCount) goes to the
//    earlier request even when it sits in the higher slot;
//  - a mismatching copy registered and the matching one still reading: wait;
//  - only a mismatching copy and nothing more can come: DEPENDENCY "built as";
//  - an UNLOADING old copy with a waiting new copy of another hash:
//    Release vs AbortUnload decide which dependants register.
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
namespace bf = snd::bank_format;

namespace {

fixture::Bank Copy(const char* name, std::uint64_t hash, std::uint32_t languageId = 0) {
  fixture::Bank b = indep11::Named(name, languageId);
  b.contentHash = hash;
  return b;
}
fixture::Bank Needs(const char* name, const char* dep, std::uint64_t buildHash) {
  fixture::Bank b = indep11::Named(name);
  b.deps.push_back(bf::Dependency{HashId(dep), 0, buildHash});
  return b;
}

// Requests `paths[k]` so that request k lands in slot slotOf[k] (slots are freed
// from fillers right before each request).
std::vector<HandleRef> RequestInSlots(Env& e, const std::vector<const char*>& paths,
                                      const std::vector<std::uint32_t>& slotOf) {
  std::vector<HandleRef> fill(e.m->MaxBanks());
  for (std::uint32_t i = 0; i < e.m->MaxBanks(); ++i) {
    const std::string p = "fill" + std::to_string(i) + ".sndb";
    e.fs.AddFile(p, indep11::Named("Fill" + std::to_string(i)).Build());
    const HandleRef h = e.m->RequestLoad(p.c_str());
    if (!h.IsNone()) fill[h.slot] = h;
  }
  e.Settle(256);
  std::vector<HandleRef> out;
  for (std::size_t k = 0; k < paths.size(); ++k) {
    const HandleRef f = fill[slotOf[k]];
    e.m->BeginUnload(f, SND_UNLOAD_STOP_AND_UNLOAD, false);
    e.m->Release(f);
    out.push_back(e.m->RequestLoad(paths[k]));
  }
  return out;
}

}  // namespace

SND_TEST(IndepHashCopy11, MatchingAndMismatchingCopiesMixedInEverySlotOrder) {
  for (int order = 0; order < 2; ++order) {
    Env e(10);
    SND_REQUIRE(e.m != nullptr);
    SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
    SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
    e.fs.AddFile("V.ja.sndb", Copy("V", 0xA1, HashId("ja")).Build());
    e.fs.AddFile("V.en.sndb", Copy("V", 0xE2, HashId("en")).Build());
    e.fs.AddFile("bj.sndb", Needs("BJ", "V", 0xA1).Build());
    e.fs.AddFile("be.sndb", Needs("BE", "V", 0xE2).Build());
    e.fs.AddFile("bx.sndb", Needs("BX", "V", 0x77).Build());  // built against a V nobody has
    const std::vector<std::uint32_t> slots = order == 0 ? std::vector<std::uint32_t>{4, 3, 2, 1, 0}
                                                        : std::vector<std::uint32_t>{0, 1, 2, 3, 4};
    const std::vector<HandleRef> h =
        RequestInSlots(e, {"V.ja.sndb", "V.en.sndb", "bj.sndb", "be.sndb", "bx.sndb"}, slots);
    e.Settle(256);
    SND_CHECK_EQ(e.m->State(h[0]), SND_BANK_STATE_LOADED);
    SND_CHECK_EQ(e.m->State(h[1]), SND_BANK_STATE_LOADED);
    SND_CHECK_MSG(e.m->State(h[2]) == SND_BANK_STATE_LOADED, "order %d: BJ %u (%s)", order, e.m->State(h[2]),
                  e.m->FailureReason(h[2]));
    SND_CHECK_MSG(e.m->State(h[3]) == SND_BANK_STATE_LOADED, "order %d: BE %u (%s)", order, e.m->State(h[3]),
                  e.m->FailureReason(h[3]));
    SND_CHECK_EQ(e.m->State(h[4]), SND_BANK_STATE_FAILED);
    SND_CHECK_EQ(e.m->Result(h[4]), SND_ERR_BANK_DEPENDENCY);
    SND_CHECK(std::strstr(e.m->FailureReason(h[4]), "built as") != nullptr);
    SND_CHECK_EQ(e.m->DependentCount(h[0]), 1u);  // BJ -> V.ja
    SND_CHECK_EQ(e.m->DependentCount(h[1]), 1u);  // BE -> V.en
  }
}

SND_TEST(IndepHashCopy11, SeveralMatchingCopiesReferenceTheEarlierRequest) {
  for (int order = 0; order < 2; ++order) {
    Env e(10);
    SND_REQUIRE(e.m != nullptr);
    SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
    SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
    e.fs.AddFile("V.ja.sndb", Copy("V", 0xC0, HashId("ja")).Build());  // same content hash
    e.fs.AddFile("V.en.sndb", Copy("V", 0xC0, HashId("en")).Build());
    e.fs.AddFile("b.sndb", Needs("B", "V", 0xC0).Build());
    // The earlier request in the HIGHER slot (order 0) or the lower one (order 1).
    const std::vector<std::uint32_t> slots =
        order == 0 ? std::vector<std::uint32_t>{5, 2, 0} : std::vector<std::uint32_t>{2, 5, 0};
    const std::vector<HandleRef> h = RequestInSlots(e, {"V.en.sndb", "V.ja.sndb", "b.sndb"}, slots);
    e.Settle(256);
    SND_REQUIRE_EQ(e.m->State(h[2]), SND_BANK_STATE_LOADED);
    SND_CHECK_MSG(e.m->DependentCount(h[0]) == 1u && e.m->DependentCount(h[1]) == 0u,
                  "order %d: references en %u / ja %u (expected the earlier request, en)", order,
                  e.m->DependentCount(h[0]), e.m->DependentCount(h[1]));
    // The referenced copy cannot be unloaded, the other one can.
    SND_CHECK_EQ(e.m->BeginUnload(h[0], SND_UNLOAD_STOP_AND_UNLOAD, false), SND_ERR_BANK_IN_USE);
    SND_CHECK_EQ(e.m->BeginUnload(h[1], SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
  }
}

SND_TEST(IndepHashCopy11, MismatchingCopyRegisteredMatchingOneStillReading) {
  Env e(10);
  SND_REQUIRE(e.m != nullptr);
  SND_REQUIRE_EQ(e.m->DeclareLanguage("ja"), SND_OK);
  SND_REQUIRE_EQ(e.m->DeclareLanguage("en"), SND_OK);
  e.fs.AddFile("V.en.sndb", Copy("V", 0xE2, HashId("en")).Build());
  e.fs.AddFile("V.ja.sndb", Copy("V", 0xA1, HashId("ja")).Build());
  e.fs.AddFile("b.sndb", Needs("B", "V", 0xA1).Build());
  const HandleRef en = e.m->RequestLoad("V.en.sndb");
  e.Settle();
  SND_REQUIRE_EQ(e.m->State(en), SND_BANK_STATE_LOADED);
  e.fs.Hold("V.ja.sndb");
  const HandleRef ja = e.m->RequestLoad("V.ja.sndb");
  const HandleRef b = e.m->RequestLoad("b.sndb");
  e.Settle(128);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADING);  // waits: the matching copy may come
  e.fs.Release("V.ja.sndb");
  e.Settle(128);
  SND_CHECK_EQ(e.m->State(ja), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(e.m->DependentCount(ja), 1u);
  SND_CHECK_EQ(e.m->DependentCount(en), 0u);
  // The matching copy's load fails instead: B fails once nothing more can come.
  Env f(10);
  SND_REQUIRE(f.m != nullptr);
  f.fs.AddFile("V.en.sndb", Copy("V", 0xE2).Build());
  f.fs.AddFile("b.sndb", Needs("B", "V", 0xA1).Build());
  const HandleRef en2 = f.m->RequestLoad("V.en.sndb");
  f.Settle();
  SND_REQUIRE_EQ(f.m->State(en2), SND_BANK_STATE_LOADED);
  f.fs.FailOpen("V.ja.sndb");
  const HandleRef ja2 = f.m->RequestLoad("V.ja.sndb");
  const HandleRef b2 = f.m->RequestLoad("b.sndb");
  f.Settle(128);
  SND_CHECK_EQ(f.m->State(ja2), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(f.m->State(b2), SND_BANK_STATE_FAILED);
  SND_CHECK_EQ(f.m->Result(b2), SND_ERR_BANK_DEPENDENCY);
  SND_CHECK(std::strstr(f.m->FailureReason(b2), "built as") != nullptr);
  for (std::uint32_t s = 0; s < f.m->MaxBanks(); ++s) {
    SND_CHECK(f.m->SlotStates()[s].state.load() != SND_BANK_STATE_LOADING);
  }
}

// Old X (hash 0x11) UNLOADING, new copy X' (hash 0x22) waits for its Release.
// B needs 0x22, C needs 0x11.  Release: X' registers, B loads, C fails (built as).
// AbortUnload: X' ALREADY_LOADED, C loads against the old X, B fails.
SND_TEST(IndepHashCopy11, UnloadingOldCopyAndNewCopyOfAnotherHash) {
  for (int abort = 0; abort < 2; ++abort) {
    Env e(10);
    SND_REQUIRE(e.m != nullptr);
    e.fs.AddFile("x.sndb", Copy("X", 0x11).Build());
    e.fs.AddFile("x2.sndb", Copy("X", 0x22).Build());
    e.fs.AddFile("b.sndb", Needs("B", "X", 0x22).Build());
    e.fs.AddFile("c.sndb", Needs("C", "X", 0x11).Build());
    const HandleRef old = e.m->RequestLoad("x.sndb");
    e.Settle();
    SND_REQUIRE_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
    SND_REQUIRE_EQ(e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false), SND_OK);
    const HandleRef x2 = e.m->RequestLoad("x2.sndb");
    const HandleRef b = e.m->RequestLoad("b.sndb");
    const HandleRef c = e.m->RequestLoad("c.sndb");
    e.Settle(256);
    SND_CHECK_MSG(e.m->State(x2) == SND_BANK_STATE_LOADING && e.m->State(b) == SND_BANK_STATE_LOADING &&
                      e.m->State(c) == SND_BANK_STATE_LOADING,
                  "abort %d: before the decision x2 %u b %u c %u", abort, e.m->State(x2), e.m->State(b),
                  e.m->State(c));
    if (abort == 0) {
      SND_CHECK_EQ(e.m->Release(old), SND_OK);
    } else {
      e.m->AbortUnload(old);
    }
    e.Settle(256);
    if (abort == 0) {
      SND_CHECK_EQ(e.m->State(x2), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_FAILED);
      SND_CHECK_EQ(e.m->Result(c), SND_ERR_BANK_DEPENDENCY);
      SND_CHECK_EQ(e.m->DependentCount(x2), 1u);
    } else {
      SND_CHECK_EQ(e.m->State(old), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->Result(x2), SND_ERR_BANK_ALREADY_LOADED);
      SND_CHECK_EQ(e.m->State(c), SND_BANK_STATE_LOADED);
      SND_CHECK_EQ(e.m->State(b), SND_BANK_STATE_FAILED);
      SND_CHECK_EQ(e.m->Result(b), SND_ERR_BANK_DEPENDENCY);
      SND_CHECK_EQ(e.m->DependentCount(old), 1u);
    }
    for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
      SND_CHECK_MSG(e.m->SlotStates()[s].state.load() != SND_BANK_STATE_LOADING, "abort %d: slot %u LOADING", abort, s);
    }
  }
}
