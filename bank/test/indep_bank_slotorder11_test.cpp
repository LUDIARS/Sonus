// Independent re-check of 2743264 (decision: the outcome of dependency
// failures does not depend on slot numbers; DEPENDENCY only when no copy can
// ever register the dependency).
//
// Each scenario keeps the REQUEST order fixed (it decides duplicates, D-50) and
// runs once per permutation of the SLOT assignment: every slot is first taken
// by a filler bank, and the filler of slot perm[k] is released right before
// request k, so request k lands in slot perm[k]. The final state / result of
// every bank must be the same for all permutations, and equal to the decided one.
// Scenarios: missing dependency + duplicate copies + chains failing in later
// rounds; a cycle next to a copy waiting for an UNLOADING copy; a cancelled
// waiting copy; Shutdown after the reads; Destroy at the end (no leak).
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
namespace bf = snd::bank_format;

namespace {

struct Def {
  const char* path;
  const char* bank;
  std::vector<const char*> deps;
  std::uint64_t hash;
};

std::vector<std::uint8_t> Image(const Def& d) {
  fixture::Bank b = indep11::Named(d.bank);
  b.contentHash = d.hash;
  for (const char* dep : d.deps) b.deps.push_back(bf::Dependency{HashId(dep), 0, 0x11});
  return b.Build();
}

struct Outcome {
  snd_bank_state state;
  snd_result result;
  bool operator==(const Outcome& o) const { return state == o.state && result == o.result; }
};

// Hooks of a scenario, called with the handles in request order.
struct Scenario {
  std::vector<Def> files;
  // before: runs after the fillers, before the requests (e.g. load + start unloading an old copy)
  std::function<void(Env&, std::vector<HandleRef>&)> before;
  // middle: runs after the requests settled, before the final settle (cancel, Release, Shutdown ...)
  std::function<void(Env&, std::vector<HandleRef>&, std::vector<HandleRef>&)> middle;
  std::vector<Outcome> expected;  // per request
};

// Runs one permutation; returns the outcomes (or an empty vector when the setup failed).
std::vector<Outcome> RunOnce(const Scenario& sc, const std::vector<std::uint32_t>& perm, bool* stuck, std::size_t* leak) {
  const std::uint32_t n = static_cast<std::uint32_t>(perm.size());
  std::vector<Outcome> out;
  std::size_t live = 0;
  {
    Env e(n + 2);  // + room for an old copy and spare
    if (e.m == nullptr) return out;
    for (const Def& d : sc.files) e.fs.AddFile(d.path, Image(d));
    std::vector<HandleRef> extra;
    if (sc.before) sc.before(e, extra);
    // Fill every free slot with a filler bank.
    std::vector<HandleRef> fill(e.m->MaxBanks());
    for (std::uint32_t i = 0; i < e.m->MaxBanks(); ++i) {
      const std::string p = "filler" + std::to_string(i) + ".sndb";
      e.fs.AddFile(p, indep11::Named("Filler" + std::to_string(i)).Build());
    }
    for (std::uint32_t i = 0; i < e.m->MaxBanks(); ++i) {
      const HandleRef h = e.m->RequestLoad(("filler" + std::to_string(i) + ".sndb").c_str());
      if (!h.IsNone()) fill[h.slot] = h;
    }
    e.Settle(256);
    // Take the n lowest filler slots as the permuted target slots (the old copy keeps its own).
    std::vector<std::uint32_t> targets;
    for (std::uint32_t s = 0; s < e.m->MaxBanks() && targets.size() < n; ++s) {
      if (!fill[s].IsNone()) targets.push_back(s);
    }
    if (targets.size() < n) return out;
    std::vector<HandleRef> hs;
    for (std::uint32_t k = 0; k < n; ++k) {
      const HandleRef f = fill[targets[perm[k]]];
      e.m->BeginUnload(f, SND_UNLOAD_STOP_AND_UNLOAD, false);
      e.m->Release(f);
      fill[targets[perm[k]]] = HandleRef{0, 0};
      hs.push_back(e.m->RequestLoad(sc.files[k].path));
      if (hs.back().slot != targets[perm[k]]) return out;  // setup did not place it
    }
    e.Settle(256);
    if (sc.middle) sc.middle(e, hs, extra);
    e.Settle(256);
    for (const HandleRef& h : hs) out.push_back(Outcome{e.m->State(h), e.m->Result(h)});
    *stuck = false;
    for (std::uint32_t s = 0; s < e.m->MaxBanks(); ++s) {
      if (e.m->SlotStates()[s].state.load() == SND_BANK_STATE_LOADING) *stuck = true;
    }
    e.io.Shutdown();
    BankManager::Destroy(e.m);
    e.m = nullptr;
    e.io.Destroy();
    e.pool.Destroy();
    live = e.alloc.Live();
  }
  *leak = live;
  return out;
}

void RunAllPermutations(const char* name, const Scenario& sc) {
  std::vector<std::uint32_t> perm(sc.files.size());
  for (std::uint32_t i = 0; i < perm.size(); ++i) perm[i] = i;
  std::uint32_t runs = 0, bad = 0, stuckRuns = 0, leakRuns = 0, setup = 0;
  do {
    bool stuck = true;
    std::size_t leak = 1;
    const std::vector<Outcome> got = RunOnce(sc, perm, &stuck, &leak);
    ++runs;
    if (got.empty()) {
      ++setup;
      continue;
    }
    if (stuck) ++stuckRuns;
    if (leak != 0) ++leakRuns;
    if (!(got == sc.expected)) {
      if (bad++ == 0) {
        std::string slots, res;
        for (std::uint32_t p : perm) slots += std::to_string(p) + " ";
        for (const Outcome& o : got) res += std::to_string(o.state) + "/" + std::to_string(o.result) + " ";
        SND_CHECK_MSG(false, "%s: slot permutation [%s] gave %s", name, slots.c_str(), res.c_str());
      }
    }
  } while (std::next_permutation(perm.begin(), perm.end()));
  SND_CHECK_MSG(setup == 0, "%s: %u runs could not place the banks", name, setup);
  SND_CHECK_MSG(bad == 0, "%s: %u of %u permutations differ from the decided outcome", name, bad, runs);
  SND_CHECK_MSG(stuckRuns == 0, "%s: %u runs left a slot LOADING", name, stuckRuns);
  SND_CHECK_MSG(leakRuns == 0, "%s: %u runs leaked", name, leakRuns);
  snd::test::Note("%s: %u permutations", name, runs);
}

constexpr Outcome kLoaded{SND_BANK_STATE_LOADED, SND_OK};
constexpr Outcome kDep{SND_BANK_STATE_FAILED, SND_ERR_BANK_DEPENDENCY};
constexpr Outcome kDup{SND_BANK_STATE_FAILED, SND_ERR_BANK_ALREADY_LOADED};
constexpr Outcome kGone{SND_BANK_STATE_UNLOADED, SND_ERR_INVALID_HANDLE};

}  // namespace

// q (X, needs Nowhere) requested before p (X, plain); B -> X; Y -> {B, Nowhere2}; Z -> Y.
// Decided: q DEPENDENCY, p registers, B registers; Y DEPENDENCY (Nowhere2), Z DEPENDENCY (later round).
SND_TEST(IndepSlotOrder11, MissingDependencyCopiesAndLaterRounds) {
  Scenario sc;
  sc.files = {{"q.sndb", "X", {"Nowhere"}, 0x21},
              {"p.sndb", "X", {}, 0x11},
              {"b.sndb", "B", {"X"}, 0x11},
              {"y.sndb", "Y", {"B", "Nowhere2"}, 0x11},
              {"z.sndb", "Z", {"Y"}, 0x11}};
  sc.expected = {kDep, kLoaded, kLoaded, kDep, kDep};
  RunAllPermutations("missing+copies", sc);
}

// The plain copy first: it registers, the later copy with the missing
// dependency fails (ALREADY_LOADED or DEPENDENCY are both "failed"; the decided
// one is ALREADY_LOADED since the earlier copy registers first).
SND_TEST(IndepSlotOrder11, PlainCopyFirst) {
  Scenario sc;
  sc.files = {{"p.sndb", "X", {}, 0x11}, {"q.sndb", "X", {"Nowhere"}, 0x21}, {"b.sndb", "B", {"X"}, 0x11},
              {"c.sndb", "C", {"B", "X"}, 0x11}};
  sc.expected = {kLoaded, kDup, kLoaded, kLoaded};
  RunAllPermutations("plain-first", sc);
}

// Old A loaded and UNLOADING; A' (copy) waits for its Release; P -> {A, Q}, Q -> P (cycle), R -> A.
// Decided: P, Q DEPENDENCY; A' and R register after the Release.
SND_TEST(IndepSlotOrder11, CycleNextToAWaitingCopy) {
  Scenario sc;
  sc.files = {{"a2.sndb", "A", {}, 0x11}, {"pp.sndb", "P", {"A", "Q"}, 0x11}, {"qq.sndb", "Q", {"P"}, 0x11},
              {"rr.sndb", "R", {"A"}, 0x11}};
  sc.before = [](Env& e, std::vector<HandleRef>& extra) {
    e.fs.AddFile("a_old.sndb", indep11::Named("A").Build());
    const HandleRef old = e.m->RequestLoad("a_old.sndb");
    e.Settle();
    e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false);
    extra.push_back(old);
  };
  sc.middle = [](Env& e, std::vector<HandleRef>& hs, std::vector<HandleRef>& extra) {
    // Before the Release: the cycle has failed, the copy and R wait.
    if (e.m->State(hs[0]) != SND_BANK_STATE_LOADING || e.m->State(hs[3]) != SND_BANK_STATE_LOADING) {
      SND_CHECK_MSG(false, "copy %u / R %u should wait for the Release", e.m->State(hs[0]), e.m->State(hs[3]));
    }
    e.m->Release(extra[0]);
  };
  sc.expected = {kLoaded, kDep, kDep, kLoaded};
  RunAllPermutations("cycle+wait", sc);
}

// The waiting copy of X is cancelled: B -> X can never be registered -> DEPENDENCY;
// the earlier copy with a missing dependency fails too.
SND_TEST(IndepSlotOrder11, CancelledWaitingCopy) {
  Scenario sc;
  sc.files = {{"q.sndb", "X", {"Nowhere"}, 0x21}, {"p.sndb", "X", {}, 0x11}, {"b.sndb", "B", {"X"}, 0x11},
              {"other.sndb", "O", {}, 0x11}};
  sc.before = [](Env& e, std::vector<HandleRef>& extra) {
    e.fs.AddFile("x_old.sndb", indep11::Named("X").Build());
    const HandleRef old = e.m->RequestLoad("x_old.sndb");
    e.Settle();
    e.m->BeginUnload(old, SND_UNLOAD_STOP_AND_UNLOAD, false);  // copies wait for its Release
    extra.push_back(old);
  };
  sc.middle = [](Env& e, std::vector<HandleRef>& hs, std::vector<HandleRef>& extra) {
    e.m->BeginUnload(hs[1], SND_UNLOAD_STOP_AND_UNLOAD, false);  // cancel the copy that could come
    e.Settle(64);
    e.m->Release(extra[0]);
  };
  sc.expected = {kDep, kGone, kDep, kLoaded};
  RunAllPermutations("cancelled-copy", sc);
}

// IoScheduler::Shutdown after everything was read: the decisions are the same.
SND_TEST(IndepSlotOrder11, ShutdownAfterTheReads) {
  Scenario sc;
  sc.files = {{"q.sndb", "X", {"Nowhere"}, 0x21}, {"p.sndb", "X", {}, 0x11}, {"b.sndb", "B", {"X"}, 0x11},
              {"y.sndb", "Y", {"Nowhere"}, 0x11}};
  sc.middle = [](Env& e, std::vector<HandleRef>&, std::vector<HandleRef>&) { e.io.Shutdown(); };
  sc.expected = {kDep, kLoaded, kLoaded, kDep};
  RunAllPermutations("shutdown", sc);
}
