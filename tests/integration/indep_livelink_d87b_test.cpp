// Independent tests of the amended D-87 (CONTRACTS.md):
//   Q1 the session starts in Server::Start: the first block after Start returns applies no
//      leftover edit of an earlier Server;
//   Q2 an earlier session's disconnect revert (begin marker -> ClearOverrides -> revert
//      marker) is applied, whatever the producer order relative to the newer Server's
//      edits in the same block, and it restores only the cells that session wrote last;
//   Q3 an earlier session's ClearOverrides that is not part of a revert is dropped;
//   Q4 a revert begin marker without its ClearOverrides does not make a LATER session's
//      ClearOverrides count as a revert;
//   Q5 cells written alternately by an older and a newer session: the older revert leaves
//      the newer values, the newer revert restores the bank values;
//   Q6 the older revert removes only the overrides its session set last (decided after
//      the review): the newer session's parameter overrides stay.
// Most checks record the live link's commands directly (owner thread or helper threads
// registered with core) to control sessions and producer order exactly; the session
// numbers come from LiveRuntime::NewSession like a Server's.
#include <atomic>
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
void Volume(Fixture& f, const char* bus, float db) {
  cmd::LiveSetValue c{};
  c.objectId = HashId(bus);
  c.field = ll::EncodeField(bf::chunk::BUSS, bf::col::BUSS_volumeDb);
  c.value = F32Bits(db);
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &c, sizeof(c)), SND_OK);
}
void Clear(Fixture& f) { SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveClearOverrides, nullptr, 0), SND_OK); }
void Knob(Fixture& f, float v) {
  cmd::LiveOverride o{};
  o.kind = static_cast<std::uint32_t>(cmd::LiveOverrideKind::Param);
  o.id = HashId("Knob");
  std::memcpy(&o.value, &v, 4);
  o.enable = 1u;
  SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveOverride, &o, sizeof(o)), SND_OK);
}
void Revert(Fixture& f) {  // what a Server records on disconnect
  Marker(f, ll::kFieldRevertBegin);
  Clear(f);
  Marker(f, ll::kFieldRevertAll);
}
double Level(Fixture& f) {
  f.Steps(24);
  ll::ApplyResult r{};
  while (f.rt->results().TryPop(r)) {
  }
  return LeftMean(f);
}

}  // namespace

// Q1: Start returns -> the next block applies no leftover of the earlier Server.
SND_TEST(IndepLivelinkD87b, FirstBlockAfterStartAppliesNoLeftover) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  constexpr int kRounds = 12;
  for (int round = 0; round <= kRounds; ++round) {
    ll::ServerDesc sd;
    sd.system = f.sys;
    sd.runtime = f.rt;
    sd.allocator = &snd::base::DefaultAllocator();
    sd.acceptor = &f.link.acceptor();
    SND_REQUIRE_EQ(ll::Server::Create(sd, &f.server), SND_OK);
    SND_REQUIRE_EQ(f.server->Start(), SND_OK);
    f.Step();  // right after Start: the previous round's leftover must be dropped here
    if (round == kRounds) break;
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
    c->Start();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(c->State() == ll::ClientState::Ready && f.server->Connected()) && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SND_REQUIRE(f.server->Connected());
    c->SetValue(lltest::BusVolume("Sfx", -40.0f), false);
    while (f.server->Stats().commandsRecorded < 1u && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ll::Server::Destroy(f.server);
    f.server = nullptr;
    ll::Client::Destroy(c);
  }
  f.Steps(24);  // (not Level(): a Server is alive and is the only consumer of the results)
  SND_CHECK_NEAR(LeftMean(f) / base, 1.0, 1e-4);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, static_cast<std::uint32_t>(kRounds));
}

// Q2: the older session's revert and the newer session's edits in one block, recorded
// by two threads; both registration orders (so both producer orders) are tried.
SND_TEST(IndepLivelinkD87b, RevertAndNewerEditsInOneBlockEitherProducerOrder) {
  for (int order = 0; order < 2; ++order) {
    Fixture f;
    f.Load(IndepBank(), "a.sndb");
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    const double base = Level(f);
    const std::uint32_t s1 = f.rt->NewSession();
    // Thread "old" (session s1) and thread "new"; registered in the order of `order`.
    std::atomic<int> step{0};
    std::atomic<int> done{0};
    auto worker = [&](bool isOld) {
      SND_REQUIRE_EQ(f.sys->RegisterThread(), SND_OK);
      done.fetch_add(1);
      // phase 1 (old only): its edit, applied while s1 is the newest session
      while (step.load() < 1) std::this_thread::yield();
      if (isOld) {
        Session(f, s1);
        Volume(f, "Sfx", -6.0f);
        Volume(f, "Master", -6.0f);
        f.sys->Flush();
      }
      done.fetch_add(1);
      // phase 2: newer edits and the older revert, applied in one block
      while (step.load() < 2) std::this_thread::yield();
      if (isOld) {
        Revert(f);
        f.sys->Flush();
      }
      done.fetch_add(1);
      while (step.load() < 3) std::this_thread::yield();
      f.sys->UnregisterThread();
    };
    // The newer session's commands are recorded by the "new" thread in phase 2; its
    // session number is made before phase 2.
    std::uint32_t s2 = 0;
    auto newer = [&] {
      SND_REQUIRE_EQ(f.sys->RegisterThread(), SND_OK);
      done.fetch_add(1);
      while (step.load() < 1) std::this_thread::yield();
      done.fetch_add(1);
      while (step.load() < 2) std::this_thread::yield();
      Session(f, s2);
      Volume(f, "Master", -3.0f);
      f.sys->Flush();
      done.fetch_add(1);
      while (step.load() < 3) std::this_thread::yield();
      f.sys->UnregisterThread();
    };
    std::thread a, b;
    if (order == 0) {
      a = std::thread([&] { worker(true); });
      while (done.load() < 1) std::this_thread::yield();
      b = std::thread(newer);
    } else {
      b = std::thread(newer);
      while (done.load() < 1) std::this_thread::yield();
      a = std::thread([&] { worker(true); });
    }
    while (done.load() < 2) std::this_thread::yield();
    step.store(1);
    while (done.load() < 4) std::this_thread::yield();
    SND_CHECK_NEAR(Level(f) / base, DbRatio(-12.0), 1e-3);  // the old edits, while s1 is newest
    s2 = f.rt->NewSession();
    step.store(2);
    while (done.load() < 6) std::this_thread::yield();
    f.sys->Update();
    f.Step();  // one block: the newer edit and the older revert
    const double lvl = Level(f) / base;
    // Sfx (only the old session wrote it) is reverted; Master was written last by the
    // newer session: it keeps -3 dB.
    SND_CHECK_MSG(std::fabs(lvl - DbRatio(-3.0)) < 1e-3, "order %d: level %.4f (expected %.4f)", order, lvl,
                  DbRatio(-3.0));
    SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
    step.store(3);
    a.join();
    b.join();
  }
}

// Q3: an older session's ClearOverrides that is not part of a revert is dropped; the
// same command in the newest session is applied.
SND_TEST(IndepLivelinkD87b, OldNonRevertClearOverridesIsDropped) {
  Fixture f;
  BankOpts o;
  o.withKnobCurve = true;
  f.Load(IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Session(f, s1);
  Knob(f, 1.0f);
  SND_REQUIRE(std::fabs(Level(f) / base - DbRatio(-20.0)) < 1e-3);  // the override works
  const std::uint32_t s2 = f.rt->NewSession();
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  Clear(f);  // still session s1 on this producer: stale, not a revert
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-20.0), 1e-3);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 1u);
  Session(f, s2);
  Clear(f);
  SND_CHECK_NEAR(Level(f) / base, 1.0, 1e-3);
}

// Q4: session s1 records only the revert begin marker (its ClearOverrides never comes:
// the Server gave up, ServerStats::revertsLost). A later session s2 on the same producer
// (a new Server's thread taking the producer back) sets an override; when s2 is old, its
// tool's plain ClearOverrides must be dropped like any old edit.
SND_TEST(IndepLivelinkD87b, RevertBeginDoesNotLeakIntoALaterSession) {
  Fixture f;
  BankOpts o;
  o.withKnobCurve = true;
  f.Load(IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Session(f, s1);
  Marker(f, ll::kFieldRevertBegin);  // ... and nothing more of that revert
  Level(f);
  const std::uint32_t s2 = f.rt->NewSession();
  Session(f, s2);
  Knob(f, 1.0f);
  SND_REQUIRE(std::fabs(Level(f) / base - DbRatio(-20.0)) < 1e-3);
  f.rt->NewSession();  // s3: s2 is old now
  const std::uint32_t stale0 = f.rt->Drops().staleEdits;
  Clear(f);  // s2's tool's plain ClearOverrides, applied late
  const double lvl = Level(f) / base;
  SND_CHECK_MSG(std::fabs(lvl - DbRatio(-20.0)) < 1e-3,
                "the newer session's override was cleared by an old plain ClearOverrides (level %.4f)", lvl);
  SND_CHECK_EQ(f.rt->Drops().staleEdits - stale0, 1u);
}

// Q5: a cell written by the older session, then the newer one; an older stale write in
// between is dropped; the older revert keeps the newer value and restores the cell only
// the older session wrote; the newer revert restores everything.
SND_TEST(IndepLivelinkD87b, AlternatingWritersAndTwoReverts) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  const std::uint32_t s1 = f.rt->NewSession();
  Session(f, s1);
  Volume(f, "Sfx", -3.0f);
  Volume(f, "Master", -3.0f);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-6.0), 1e-3);
  const std::uint32_t s2 = f.rt->NewSession();
  Session(f, s2);
  Volume(f, "Sfx", -6.0f);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-9.0), 1e-3);
  Session(f, s1);  // the old producer again (late arena)
  Volume(f, "Sfx", -12.0f);  // stale: dropped
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-9.0), 1e-3);
  Revert(f);  // s1's disconnect revert
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-6.0), 1e-3);  // Master back, Sfx keeps s2's -6
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);
  Session(f, s2);
  Volume(f, "Sfx", -2.0f);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-2.0), 1e-3);
  Revert(f);  // s2's revert: the bank value (the original kept from s1's first write)
  SND_CHECK_NEAR(Level(f) / base, 1.0, 1e-3);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 0u);
}

// Q6 (decided after the stage-2 review): the older revert removes only the overrides its
// session (or an older one) set last: the newer session's override stays.
SND_TEST(IndepLivelinkD87b, OldRevertKeepsTheNewerSessionsOverrides) {
  Fixture f;
  BankOpts o;
  o.withKnobCurve = true;
  f.Load(IndepBank(o), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const double base = Level(f);
  const std::uint32_t s1 = f.rt->NewSession();
  const std::uint32_t s2 = f.rt->NewSession();
  Session(f, s2);
  Knob(f, 1.0f);
  SND_REQUIRE(std::fabs(Level(f) / base - DbRatio(-20.0)) < 1e-3);
  Session(f, s1);
  Revert(f);
  SND_CHECK_NEAR(Level(f) / base, DbRatio(-20.0), 1e-3);
  Session(f, s2);
  Revert(f);  // the newest session's revert clears every override
  SND_CHECK_NEAR(Level(f) / base, 1.0, 1e-3);
}
