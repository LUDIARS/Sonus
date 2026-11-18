// Independent test (D-50 / 10.3, re-check of a228470): "if the dependency will
// be registered, the dependant must not fail with SND_ERR_BANK_DEPENDENCY".
//
// (Reported as a defect; fixed in 2743264.) Formerly failing: Two copies of bank X: the EARLIER request (q)
// depends on a bank that is never requested, the LATER one (p) has no
// dependency and waits for q's outcome (decision C). B depends on X. Nothing is
// loading any more, so Pump fails "one bank whose dependency is missing at a
// time" - in SLOT order. ComputeWillCome marks neither copy of X (q really
// lacks its dependency, p waits only for q, not for an UNLOADING copy), so when
// B sits in a lower slot than q, B is failed first with BANK_DEPENDENCY; then q
// fails, and p registers: X is loaded but B already failed. With B in a higher
// slot than q (other request order) B loads: the result depends on slot order.
// bank_manager.cpp Pump() victim loop + ComputeWillCome().
#include <cstring>

#include "indep_bank_io11_env.hpp"

using snd::HandleRef;
using snd::HashId;
using namespace snd::bank;
using indep11::Env;
namespace bf = snd::bank_format;

namespace {
fixture::Bank Make(const char* name, const char* dep, std::uint64_t hash) {
  fixture::Bank b = indep11::Named(name);
  b.contentHash = hash;
  if (dep != nullptr) b.deps.push_back(bf::Dependency{HashId(dep), 0, 0x22});
  return b;
}
}  // namespace

SND_TEST(BankVictimOrder, DependantFailsAlthoughItsDependencyRegisters) {
  for (int order = 0; order < 2; ++order) {
    Env e(8);
    SND_REQUIRE(e.m != nullptr);
    e.fs.AddFile("x_needs_nowhere.sndb", Make("X", "Nowhere", 0x21).Build());  // q: can never register
    e.fs.AddFile("x_plain.sndb", Make("X", nullptr, 0x22).Build());            // p: registers once q failed
    e.fs.AddFile("b.sndb", Make("B", "X", 0x33).Build());                       // built against p (0x22)
    HandleRef b, q, p;
    if (order == 0) {  // B in the lowest slot
      b = e.m->RequestLoad("b.sndb");
      q = e.m->RequestLoad("x_needs_nowhere.sndb");
      p = e.m->RequestLoad("x_plain.sndb");
    } else {  // B after the copies of X
      q = e.m->RequestLoad("x_needs_nowhere.sndb");
      p = e.m->RequestLoad("x_plain.sndb");
      b = e.m->RequestLoad("b.sndb");
    }
    e.Settle(256);
    SND_CHECK_EQ(e.m->State(q), SND_BANK_STATE_FAILED);  // really missing dependency
    SND_CHECK_EQ(e.m->State(p), SND_BANK_STATE_LOADED);
    SND_CHECK_MSG(e.m->State(b) == SND_BANK_STATE_LOADED,
                  "order %d (B in slot %u, q in slot %u): B ended %u (%d: %s) although X registered", order, b.slot,
                  q.slot, e.m->State(b), e.m->Result(b), e.m->FailureReason(b));
  }
}
