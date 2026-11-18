// Independent tests of decision D-87 (CONTRACTS.md): edits of an earlier Server's
// session are not applied. Properties:
//   P1 an edit recorded by an earlier Server (SetValue, ReplaceChunk, OverrideParam,
//      ClearOverrides) and applied after a newer Server started never overwrites a value
//      of the newer Server, even in the same block as the newer Server's first edit,
//      however many times Servers are recreated;
//   P2 every such edit is counted once (RuntimeDrops::staleEdits);
//   P3 the ReplaceChunk slot of a dropped chunk goes back to the pool (no leak);
//   P4 the disconnect revert of an earlier Server is applied (amended D-87), the newer
//      Server's values stay;
//   P5 reconnecting to the SAME Server is not a new session: its edits apply.
#include <chrono>
#include <thread>

#include "indep_livelink_common.hpp"

using namespace indep;
using lltest::Fixture;

namespace {

ll::Server* NewServer(Fixture& f, ll::ServerDesc sd = {}) {
  sd.system = f.sys;
  sd.runtime = f.rt;
  sd.allocator = &snd::base::DefaultAllocator();
  sd.acceptor = &f.link.acceptor();
  ll::Server* s = nullptr;
  if (ll::Server::Create(sd, &s) != SND_OK) return nullptr;
  if (s->Start() != SND_OK) {
    ll::Server::Destroy(s);
    return nullptr;
  }
  return s;
}

bool ReadyNoRender(ll::Client* c, ll::Server* s) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(c->State() == ll::ClientState::Ready && s->Connected()) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return c->State() == ll::ClientState::Ready && s->Connected();
}

void WaitRecorded(ll::Server* s, std::uint32_t n) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (s->Stats().commandsRecorded < n && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // flushed
}

std::vector<std::uint8_t> BussChunk(float sfxDb, float masterDb) {
  bt::TableBuilder bu(bf::chunk::BUSS);
  std::uint32_t r = bu.AddRow();
  bu.U32(bf::col::BUSS_id, r, HashId("Sfx"));
  bu.U32(bf::col::BUSS_parent, r, 1u);
  bu.U32(bf::col::BUSS_channels, r, 2u);
  bu.F32(bf::col::BUSS_volumeDb, r, sfxDb);
  r = bu.AddRow();
  bu.U32(bf::col::BUSS_id, r, HashId("Master"));
  bu.U32(bf::col::BUSS_parent, r, bf::kNone);
  bu.U32(bf::col::BUSS_channels, r, 2u);
  bu.U32(bf::col::BUSS_flags, r, bf::kBusMaster);
  bu.F32(bf::col::BUSS_volumeDb, r, masterDb);
  return bu.Serialize();
}

double Level(Fixture& f) {
  f.Steps(24);
  return LeftMean(f);
}

}  // namespace

// P1 / P2 / P3: 6 Servers in a row. Each leaves SetValue x2, ReplaceChunk, OverrideParam
// and ClearOverrides recorded but unapplied; the next Server's first edit is applied in
// the same block. Only the newest values are audible; each old edit is counted once;
// the single ReplaceChunk slot never leaks.
SND_TEST(IndepLivelinkD87, OldServersNeverOverwriteTheNewestValues) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  std::uint32_t staleBefore = f.rt->Drops().staleEdits;
  for (int round = 0; round < 6; ++round) {
    f.server = NewServer(f);
    SND_REQUIRE(f.server != nullptr);
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
    c->Start();
    SND_REQUIRE(ReadyNoRender(c, f.server));
    // The newest values (applied in the same block as the previous Server's leftovers).
    const float newDb = -2.0f - static_cast<float>(round);
    const std::uint32_t seq = c->SetValue(lltest::BusVolume("Sfx", newDb), true);
    WaitRecorded(f.server, 1u);
    std::int32_t r = 1;
    SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
    SND_CHECK_EQ(r, SND_OK);
    const double lvl = Level(f) / base;
    SND_CHECK_MSG(std::fabs(lvl - DbRatio(newDb)) < 1e-3, "round %d: level %.4f, newest edit %.1f dB (%.4f)", round,
                  lvl, newDb, DbRatio(newDb));
    if (round > 0) {
      const std::uint32_t now = f.rt->Drops().staleEdits;
      SND_CHECK_MSG(now - staleBefore == 5u, "round %d: %u stale edits counted (5 expected)", round, now - staleBefore);
      staleBefore = now;
    }
    // Leftovers for the next Server: none of them may ever be audible.
    c->SetValue(lltest::BusVolume("Sfx", -40.0f), false);
    c->SetValue(lltest::BusVolume("Master", -40.0f), false);
    const std::vector<std::uint8_t> chunk = BussChunk(-30.0f, -30.0f);
    const std::uint32_t cs = c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true);
    c->OverrideParam(HashId("Knob"), 0.9f, true, false);
    c->ClearOverrides(false);
    WaitRecorded(f.server, 6u);
    // The chunk slot is free (the previous round's chunk was dropped and freed): the
    // ReplaceChunk is not refused at once with LIMIT_REACHED.
    std::int32_t cr = 0;
    SND_CHECK_MSG(!c->TakeAck(cs, &cr), "round %d: ReplaceChunk answered at once with %d (slot leaked?)", round, cr);
    ll::Server::Destroy(f.server);
    f.server = nullptr;
    ll::Client::Destroy(c);
  }
  // A last Server: its ReplaceChunk gets the slot and is applied.
  f.server = NewServer(f);
  SND_REQUIRE(f.server != nullptr);
  // The session starts when the new Server's thread runs (not at Start): wait for it
  // before rendering (see the report: a block rendered in between applies the leftovers).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-7.0), 1e-3);  // round 5's value; no leftover applied
  Tool t(f);
  SND_REQUIRE(t.Ready());
  const std::vector<std::uint8_t> chunk = BussChunk(-12.0f, 0.0f);
  SND_CHECK_EQ(t.Ack(t.c->ReplaceChunk(lltest::kBankId, bf::chunk::BUSS, chunk.data(), chunk.size(), true)), SND_OK);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-12.0), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - staleBefore, 5u);
}

// P4 (D-87 amended): a revertOnDisconnect Server is destroyed with an edit applied; its
// disconnect revert is still unapplied when the next Server's first edit arrives. The
// revert is applied (the earlier edit goes back), the newer edit holds.
SND_TEST(IndepLivelinkD87, EarlierServersDisconnectRevertDoesNotTouchTheNewerValues) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  f.server = NewServer(f, sd);
  SND_REQUIRE(f.server != nullptr);
  {
    Tool t(f);
    SND_REQUIRE(t.Ready());
    SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f), true)), SND_OK);
    SND_CHECK_NEAR(Level(f) / base, DbRatio(-6.0), 1e-3);
  }
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  ll::Server::Destroy(f.server);  // records the revert; no block since
  f.server = NewServer(f);
  SND_REQUIRE(f.server != nullptr);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
  c->Start();
  SND_REQUIRE(ReadyNoRender(c, f.server));
  const std::uint32_t seq = c->SetValue(lltest::BusVolume("Master", -3.0f), true);
  WaitRecorded(f.server, 1u);
  std::int32_t r = 1;
  SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  const double lvl = Level(f) / base;
  const std::uint32_t dropped = f.rt->Drops().staleEdits - stale0;
  // D-87 (amended): the earlier Server's revert is applied: its Sfx -6 dB goes back to the
  // bank value; the newer Master -3 dB stays; nothing of the revert is dropped.
  SND_CHECK_NEAR(lvl, DbRatio(-3.0), 1e-3);
  SND_CHECK_EQ(dropped, 0u);
  SND_CHECK_EQ(f.server->Stats().revertsLost, 0u);
  ll::Client::Destroy(c);
}

// P5: a tool that reconnects to the same Server keeps its session: edits recorded before
// the reconnect and applied after it are applied, nothing is counted as stale.
SND_TEST(IndepLivelinkD87, ReconnectingToTheSameServerIsNotANewSession) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  ll::ClientDesc cd = lltest::MemoryClient(f.link);
  cd.reconnectDelayMs = 5;
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(cd, &c), SND_OK);
  c->Start();
  SND_REQUIRE(ReadyNoRender(c, f.server));
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  c->SetValue(lltest::BusVolume("Sfx", -6.0f), false);
  WaitRecorded(f.server, 1u);
  c->Disconnect();
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (c->Stats().connects < 2u && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_REQUIRE(ReadyNoRender(c, f.server));
  const std::uint32_t seq = c->SetValue(lltest::BusVolume("Master", -3.0f), true);
  WaitRecorded(f.server, 2u);
  std::int32_t r = 1;
  SND_REQUIRE(f.StepUntil([&] { return c->TakeAck(seq, &r); }));
  SND_CHECK_EQ(r, SND_OK);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-9.0), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits, stale0);
  ll::Client::Destroy(c);
}
