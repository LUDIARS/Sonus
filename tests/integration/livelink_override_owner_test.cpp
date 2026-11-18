// D-87 (amended): an older session's disconnect revert removes only the live overrides
// that session set last; the newest session's revert / ClearOverrides clears them all.
// A revert left unfinished (begin marker without its ClearOverrides) never makes a later
// session's plain ClearOverrides count as a revert. Uses the independent tests' bank
// ("Tone" follows the Knob override: 0 -> 0 dB, 1 -> -20 dB).
#include <cmath>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;

namespace {

void Mark(Fixture& f, std::uint32_t field, std::uint32_t value = 0) {
  cmd::LiveSetValue m{};
  m.field = field;
  m.value = value;
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m)), SND_OK);
}
void Override(Fixture& f, float v, bool enable = true) {
  cmd::LiveOverride o{};
  o.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param);
  o.id = HashId("Knob");
  std::memcpy(&o.value, &v, 4);
  o.enable = enable ? 1u : 0u;
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveOverride, &o, sizeof(o)), SND_OK);
}
void ClearAll(Fixture& f) { SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveClearOverrides, nullptr, 0), SND_OK); }
void DisconnectRevert(Fixture& f) {
  Mark(f, ll::kFieldRevertBegin);
  ClearAll(f);
  Mark(f, ll::kFieldRevertAll);
}
double Settle(Fixture& f) {
  f.Steps(24);
  ll::ApplyResult r{};
  while (f.rt->results().TryPop(r)) {
  }
  return LeftMean(f);
}
Fixture* Make() {
  Fixture* f = new Fixture();
  BankOpts o;
  o.withKnobCurve = true;
  f->Load(IndepBank(o), "a.sndb");
  f->sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  return f;
}

}  // namespace

SND_TEST(LivelinkOverrideOwner, OlderRevertRemovesOnlyItsOwnOverrides) {
  Fixture& f = *Make();
  const double base = Settle(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Mark(f, ll::kFieldSession, s1);
  Override(f, 1.0f);  // s1 owns the Knob override
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  const std::uint32_t s2 = f.rt->NewSession();  // s1 is old now; s2 sets nothing yet
  Mark(f, ll::kFieldSession, s1);
  DisconnectRevert(f);  // s1's own override goes
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  // s2 sets the override; a later revert of s1 leaves it.
  Mark(f, ll::kFieldSession, s2);
  Override(f, 1.0f);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  Mark(f, ll::kFieldSession, s1);
  DisconnectRevert(f);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  // The newest session's revert clears everything.
  Mark(f, ll::kFieldSession, s2);
  DisconnectRevert(f);
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  delete &f;
}

SND_TEST(LivelinkOverrideOwner, OverrideTakenOverByANewerSessionSurvivesTheOlderRevert) {
  Fixture& f = *Make();
  const double base = Settle(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Mark(f, ll::kFieldSession, s1);
  Override(f, 0.5f);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-10.0), 1e-3);
  const std::uint32_t s2 = f.rt->NewSession();
  Mark(f, ll::kFieldSession, s2);
  Override(f, 1.0f);  // the same override, now set last by s2
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  Mark(f, ll::kFieldSession, s1);
  Override(f, 0.0f);  // a late s1 edit: dropped
  DisconnectRevert(f);  // s1's revert: the override is s2's
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 1u);
  delete &f;
}

SND_TEST(LivelinkOverrideOwner, UnfinishedRevertDoesNotLeakIntoTheNextSession) {
  Fixture& f = *Make();
  const double base = Settle(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Mark(f, ll::kFieldSession, s1);
  Mark(f, ll::kFieldRevertBegin);  // the rest of that revert was lost
  Settle(f);
  const std::uint32_t s2 = f.rt->NewSession();
  Mark(f, ll::kFieldSession, s2);
  Override(f, 1.0f);
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  f.rt->NewSession();  // s2 is old
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  ClearAll(f);  // s2's plain clear, late: dropped, not a revert
  SND_CHECK_NEAR(Settle(f) / base, DbRatio(-20.0), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 1u);
  // Within one session too: the begin marker is consumed by one ClearOverrides only.
  Mark(f, ll::kFieldRevertBegin);
  ClearAll(f);  // the revert's clear (old session: removes s2's own override)
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  Mark(f, ll::kFieldSession, s2);
  Override(f, 1.0f);  // dropped (s2 is old)
  ClearAll(f);  // plain, old: dropped
  SND_CHECK_NEAR(Settle(f) / base, 1.0, 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 3u);
  delete &f;
}
