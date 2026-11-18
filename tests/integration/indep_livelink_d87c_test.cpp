// Independent tests of D-87 (override owners, after the stage-2 review):
//   R1 an older and a newer session set the same parameter in turn: the newer value wins,
//      an older stale setting is dropped, the older revert leaves the newer override,
//      the newer revert removes it;
//   R2 three sessions: a revert removes the overrides its session or an OLDER one set
//      last, and none a newer one set;
//   R3 the next Server starts while the previous one's revert is unapplied (real
//      Servers): in one block, the old cells go back, the new override stays;
//   R4 an override enabled by the newest session and then disabled is forgotten: a later
//      older revert does not touch a later re-enable by the newest session.
// Session numbers come from LiveRuntime::NewSession; commands are recorded directly
// (owner thread) except in R3.
#include <chrono>
#include <cstdio>
#include <thread>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;

namespace {

void Marker(Fixture& f, std::uint32_t field, std::uint32_t value = 0) {
  cmd::LiveSetValue m{};
  m.field = field;
  m.value = value;
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m)), SND_OK);
}
void Session(Fixture& f, std::uint32_t s) { Marker(f, ll::kFieldSession, s); }
void Knob(Fixture& f, float v, bool enable = true) {
  cmd::LiveOverride o{};
  o.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param);
  o.id = HashId("Knob");
  std::memcpy(&o.value, &v, 4);
  o.enable = enable ? 1u : 0u;
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveOverride, &o, sizeof(o)), SND_OK);
}
void Revert(Fixture& f) {
  Marker(f, ll::kFieldRevertBegin);
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveClearOverrides, nullptr, 0), SND_OK);
  Marker(f, ll::kFieldRevertAll);
}
double Level(Fixture& f) {
  f.Steps(24);
  ll::ApplyResult r{};
  while (f.rt->results().TryPop(r)) {
  }
  return LeftMean(f);
}
// Knob 0..1 -> 0..-20 dB on "Tone" (linear in dB).
double KnobDb(float k) { return DbRatio(-20.0 * k); }

struct KnobGame {
  Fixture f;
  double base = 0;
  KnobGame() {
    BankOpts o;
    o.withKnobCurve = true;
    f.Load(IndepBank(o), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    base = Level(f);
  }
  double L() { return Level(f) / base; }
};

}  // namespace

SND_TEST(IndepLivelinkD87c, SameParameterSetInTurnByTwoSessions) {
  KnobGame g;
  Fixture& f = g.f;
  const std::uint32_t s1 = f.rt->NewSession();
  Session(f, s1);
  Knob(f, 1.0f);
  SND_CHECK_NEAR(g.L(), KnobDb(1.0f), 1e-3);
  const std::uint32_t s2 = f.rt->NewSession();
  Session(f, s2);
  Knob(f, 0.5f);
  SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  Session(f, s1);
  Knob(f, 0.0f);  // stale: dropped
  SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 1u);
  Revert(f);  // s1's revert: s2 set Knob last -> stays
  SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);
  Session(f, s2);
  Revert(f);  // the newest session's revert
  SND_CHECK_NEAR(g.L(), 1.0, 1e-3);
}

SND_TEST(IndepLivelinkD87c, ThreeSessions) {
  {
    // s1 sets the override; s2 and s3 start; s2's (old) revert removes it (s1 <= s2).
    KnobGame g;
    Fixture& f = g.f;
    const std::uint32_t s1 = f.rt->NewSession();
    Session(f, s1);
    Knob(f, 1.0f);
    SND_CHECK_NEAR(g.L(), KnobDb(1.0f), 1e-3);
    const std::uint32_t s2 = f.rt->NewSession();
    f.rt->NewSession();  // s3
    Session(f, s2);
    Revert(f);
    SND_CHECK_NEAR(g.L(), 1.0, 1e-3);
  }
  {
    // s2 sets it last; s3 starts; s1's revert keeps it, s2's revert removes it.
    KnobGame g;
    Fixture& f = g.f;
    const std::uint32_t s1 = f.rt->NewSession();
    Session(f, s1);
    Knob(f, 1.0f);
    SND_CHECK_NEAR(g.L(), KnobDb(1.0f), 1e-3);  // applied while s1 is the newest
    const std::uint32_t s2 = f.rt->NewSession();
    Session(f, s2);
    Knob(f, 0.5f);
    SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);  // applied while s2 is the newest
    const std::uint32_t s3 = f.rt->NewSession();
    Session(f, s1);
    Revert(f);
    SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);
    Session(f, s2);
    Revert(f);
    SND_CHECK_NEAR(g.L(), 1.0, 1e-3);
    // s3 (newest) sets it; s1's and s2's late reverts both leave it.
    Session(f, s3);
    Knob(f, 0.25f);
    SND_CHECK_NEAR(g.L(), KnobDb(0.25f), 1e-3);
    Session(f, s1);
    Revert(f);
    Session(f, s2);
    Revert(f);
    SND_CHECK_NEAR(g.L(), KnobDb(0.25f), 1e-3);
  }
}

SND_TEST(IndepLivelinkD87c, DisabledThenReEnabledByTheNewestSession) {
  KnobGame g;
  Fixture& f = g.f;
  const std::uint32_t s1 = f.rt->NewSession();
  Session(f, s1);
  Knob(f, 1.0f);
  SND_CHECK_NEAR(g.L(), KnobDb(1.0f), 1e-3);
  const std::uint32_t s2 = f.rt->NewSession();
  Session(f, s2);
  Knob(f, 0.0f, false);  // s2 removes s1's override
  SND_CHECK_NEAR(g.L(), 1.0, 1e-3);
  Knob(f, 0.5f);  // and sets its own
  Session(f, s1);
  Revert(f);
  SND_CHECK_NEAR(g.L(), KnobDb(0.5f), 1e-3);
}

// R3 with real Servers: A (revertOnDisconnect) set Sfx -6 dB and Knob 1; A is destroyed,
// B started, B's tool sets Knob 0.5; A's revert and B's override are applied together.
SND_TEST(IndepLivelinkD87c, NextServerStartsDuringTheRevert) {
  KnobGame g;
  Fixture& f = g.f;
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  auto make = [&](bool revert) {
    ll::ServerDesc sd;
    sd.system = f.sys;
    sd.runtime = f.rt;
    sd.allocator = &snd::base::DefaultAllocator();
    sd.acceptor = &f.link.acceptor();
    sd.revertOnDisconnect = revert;
    ll::Server* s = nullptr;
    if (ll::Server::Create(sd, &s) != SND_OK) return static_cast<ll::Server*>(nullptr);
    if (s->Start() != SND_OK) {
      ll::Server::Destroy(s);
      return static_cast<ll::Server*>(nullptr);
    }
    return s;
  };
  f.server = make(true);
  SND_REQUIRE(f.server != nullptr);
  {
    Tool t(f);
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
    SND_CHECK_EQ(t.Ack(t.c->OverrideParam(HashId("Knob"), 1.0f, true, true)), SND_OK);
    f.Steps(24);
    SND_CHECK_NEAR(LeftMean(f) / g.base, DbRatio(-6.0) * KnobDb(1.0f), 1e-3);
  }
  ll::Server::Destroy(f.server);  // its revert is recorded, not applied
  f.server = make(false);
  SND_REQUIRE(f.server != nullptr);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
  c->Start();
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(c->State() == ll::ClientState::Ready && f.server->Connected()) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_REQUIRE(f.server->Connected());
  const std::uint32_t seq = c->OverrideParam(HashId("Knob"), 0.5f, true, true);
  while (f.server->Stats().commandsRecorded < 1u && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::int32_t r = 1;
  // Not Level(): the Server owns the results ring.
  SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  f.Steps(24);
  const double lvl = LeftMean(f) / g.base;
  SND_CHECK_MSG(std::fabs(lvl - KnobDb(0.5f)) < 1e-3, "level %.4f: expected Sfx back at 0 dB and Knob 0.5 (%.4f)", lvl,
                KnobDb(0.5f));
  ll::Client::Destroy(c);
}

// Overrides recorded without a session (a producer that never sent a session marker,
// session 0) and an older Server's revert: reported as observed (spec question).
SND_TEST(IndepLivelinkD87c, OverrideWithoutASessionAndAnOlderRevert) {
  KnobGame g;
  Fixture& f = g.f;
  Knob(f, 1.0f);  // owner thread, no session marker
  SND_CHECK_NEAR(g.L(), KnobDb(1.0f), 1e-3);
  const std::uint32_t s1 = f.rt->NewSession();
  f.rt->NewSession();
  Session(f, s1);
  Revert(f);
  const double lvl = g.L();
  std::printf("note: an override without a session %s an older session's revert (level %.4f)\n",
              std::fabs(lvl - 1.0) < 1e-3 ? "is removed by" : "survives", lvl);
}
