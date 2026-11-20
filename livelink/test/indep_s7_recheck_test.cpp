// Independent tests, protocol 1.1 (组 L, indep_s7): re-check of the decisions D-170..D-175 and
// of the fixes (indep_s7_properties.md, section M).
//   M2  revert-log places and dev reserve against a model, random operation sequences with
//       editor refusals, disconnects (revert) and reconnects;
//   M3  a removed live bus takes its live effects and sends along (no Undo for them);
//   M4  the two replay predicates over a table of hand-made recordings, and a real recording
//       that starts while a playback is alive;
//   M7  one started Server per runtime: in order and from two threads;
//   M8  the state after a ReloadBank of another bank's file;
//   M10 malformed structural edits while the log is full.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "indep_s7_common.hpp"

namespace {

using namespace s7;
using indep::BankOpts;
using indep::IndepBank;
using indep::Tool;
using lltest::Fixture;
using lltest::Hooks;
constexpr snd::Id kLive = lltest::kBankId;
const std::uint64_t kMasterArgs[2] = {HashId("Master"), 2};

// --- M2: model of the dev reserve and the revert log ------------------------------------
struct LiveBus {
  std::uint32_t id;
  std::uint32_t effects;  // live effects on it
  std::uint32_t sends;    // live sends from it
  std::uint64_t bytes;    // bytes of the bus and of what is on it
};
struct Model {
  std::uint32_t busLimit, fxLimit, logLimit;
  std::uint64_t byteLimit;
  std::vector<LiveBus> buses;
  std::uint32_t sfxEffects = 0, sfxSends = 0, bankRemoves = 0;
  std::uint64_t sfxBytes = 0;
  std::uint32_t Buses() const { return static_cast<std::uint32_t>(buses.size()); }
  std::uint32_t Effects() const {
    std::uint32_t n = sfxEffects;
    for (const LiveBus& b : buses) n += b.effects;
    return n;
  }
  std::uint64_t Bytes() const {
    std::uint64_t n = sfxBytes;
    for (const LiveBus& b : buses) n += b.bytes;
    return n;
  }
  std::uint32_t Places() const {
    std::uint32_t n = sfxEffects + sfxSends + bankRemoves;
    for (const LiveBus& b : buses) n += 1u + b.effects + b.sends;
    return n;
  }
  void Clear() {
    buses.clear();
    sfxEffects = sfxSends = bankRemoves = 0;
    sfxBytes = 0;
  }
};

}  // namespace

SND_TEST(IndepS7Recheck, StructLogAndReserveFollowTheModel) {
  for (std::uint32_t seed = 1; seed <= 4; ++seed) {
    std::mt19937 rng(seed * 7919u);
    FakeEditor ed;
    ll::RuntimeDesc rd;
    rd.structEditor = &ed;
    rd.maxStructLog = 6;
    Fixture f(Hooks::Live, ReserveConfig(3, 3, 900), nullptr, rd);
    f.Load(IndepBank(), "a.sndb");
    ll::ServerDesc sd;
    sd.revertOnDisconnect = true;
    SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
    ll::ClientDesc d = lltest::MemoryClient(f.link);
    d.reconnectDelayMs = 1;
    Tool t(f, d);
    SND_REQUIRE(t.Ready());
    Model m{3, 3, 6, 900, {}};
    std::uint32_t nextId = 0x30000u, mismatches = 0, ops = 0;
    auto bad = [&](const char* what, std::int32_t r) {
      ++mismatches;
      if (mismatches <= 8u)
        std::printf("[indep_s7] seed %u op %u (%s): result %d; model buses %u fx %u bytes %llu places %u, refuse %d\n",
                    seed, ops, what, r, m.Buses(), m.Effects(), static_cast<unsigned long long>(m.Bytes()), m.Places(),
                    ed.applyResult.load());
    };
    auto check = [&](const char* what) {
      const ll::DevReserveUsage u = f.rt->ReserveUsage();
      const bool ok = u.buses == m.Buses() && u.effects == m.Effects() && u.bytes == m.Bytes() &&
                      f.rt->StructLogPlaces() == m.Places() && f.rt->Drops().structLog == 0u;
      if (!ok) {
        ++mismatches;
        if (mismatches <= 5u)
          std::printf("[indep_s7] seed %u op %u (%s): runtime buses %u fx %u bytes %llu places %u / model %u %u %llu %u\n",
                      seed, ops, what, u.buses, u.effects, static_cast<unsigned long long>(u.bytes),
                      f.rt->StructLogPlaces(), m.Buses(), m.Effects(), static_cast<unsigned long long>(m.Bytes()),
                      m.Places());
      }
    };
    for (ops = 0; ops < 120; ++ops) {
      const std::uint32_t k = rng() % 100u;
      const bool refuse = (rng() % 8u) == 0u;
      ed.applyResult = refuse ? SND_ERR_NOT_FOUND : SND_OK;
      if (k < 25) {  // AddBus
        const std::uint32_t id = ++nextId;
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::AddBus, id, kMasterArgs, 2));
        const bool budget = m.Buses() + 1u <= m.busLimit && m.Bytes() + 80u <= m.byteLimit;
        const bool place = m.Places() + 1u <= m.logLimit;
        if (r == SND_OK) {
          if (!budget || !place || refuse) bad("AddBus", r);
          m.buses.push_back(LiveBus{id, 0, 0, 80});
        } else if (r == SND_ERR_OUT_OF_BUDGET) {
          if (budget) bad("AddBus", r);
        } else if (r == SND_ERR_LIMIT_REACHED) {
          if (place) bad("AddBus", r);
        } else if (r != SND_ERR_NOT_FOUND || !refuse || !budget || !place) {
          bad("AddBus", r);
        }
        check("AddBus");
      } else if (k < 45) {  // AddEffect on a live bus or on Sfx
        const bool onLive = !m.buses.empty() && (rng() & 1u);
        LiveBus* b = onLive ? &m.buses[rng() % m.buses.size()] : nullptr;
        const std::uint32_t target = b != nullptr ? b->id : HashId("Sfx");
        const std::uint64_t fx[2] = {HashId("snd.fx.gain"), 0xFFFFFFFFu};
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, target, fx, 2));
        const bool budget = m.Effects() + 1u <= m.fxLimit && m.Bytes() + 80u <= m.byteLimit;
        const bool place = m.Places() + 1u <= m.logLimit;
        if (r == SND_OK) {
          if (!budget || !place || refuse) bad("AddEffect", r);
          if (b != nullptr) {
            ++b->effects;
            b->bytes += 80u;
          } else {
            ++m.sfxEffects;
            m.sfxBytes += 80u;
          }
        } else if ((r == SND_ERR_OUT_OF_BUDGET && budget) || (r == SND_ERR_LIMIT_REACHED && place) ||
                   (r == SND_ERR_NOT_FOUND && !refuse) ||
                   (r != SND_ERR_OUT_OF_BUDGET && r != SND_ERR_LIMIT_REACHED && r != SND_ERR_NOT_FOUND)) {
          bad("AddEffect", r);
        }
        check("AddEffect");
      } else if (k < 60) {  // AddSend from a live bus or from Sfx
        const bool onLive = !m.buses.empty() && (rng() & 1u);
        LiveBus* b = onLive ? &m.buses[rng() % m.buses.size()] : nullptr;
        const std::uint32_t target = b != nullptr ? b->id : HashId("Sfx");
        const std::uint64_t send[2] = {HashId("Master"), F32Bits(-6.0f)};
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::AddSend, target, send, 2));
        const bool budget = m.Bytes() + 80u <= m.byteLimit;
        const bool place = m.Places() + 1u <= m.logLimit;
        if (r == SND_OK) {
          if (!budget || !place || refuse) bad("AddSend", r);
          if (b != nullptr) {
            ++b->sends;
            b->bytes += 80u;
          } else {
            ++m.sfxSends;
            m.sfxBytes += 80u;
          }
        } else if ((r == SND_ERR_OUT_OF_BUDGET && budget) || (r == SND_ERR_LIMIT_REACHED && place) ||
                   (r == SND_ERR_NOT_FOUND && !refuse) ||
                   (r != SND_ERR_OUT_OF_BUDGET && r != SND_ERR_LIMIT_REACHED && r != SND_ERR_NOT_FOUND)) {
          bad("AddSend", r);
        }
        check("AddSend");
      } else if (k < 80) {  // RemoveBus of a live bus (cancels it and what is on it)
        if (m.buses.empty()) continue;
        const std::size_t i = rng() % m.buses.size();
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, m.buses[i].id, nullptr, 0));
        // a remove needs a place at acceptance too (given back when it cancels a live add)
        const bool place = m.Places() + 1u <= m.logLimit;
        if (r == SND_OK) {
          if (refuse || !place) bad("RemoveBus live", r);
          m.buses.erase(m.buses.begin() + static_cast<std::ptrdiff_t>(i));
        } else if ((r == SND_ERR_LIMIT_REACHED && place) || (r == SND_ERR_NOT_FOUND && !refuse) ||
                   (r != SND_ERR_LIMIT_REACHED && r != SND_ERR_NOT_FOUND)) {
          bad("RemoveBus live", r);
        }
        check("RemoveBus live");
      } else if (k < 85) {  // RemoveBus of the bank's bus: stays in the log
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("Sfx"), nullptr, 0));
        const bool place = m.Places() + 1u <= m.logLimit;
        if (r == SND_OK) {
          if (refuse || !place) bad("RemoveBus bank", r);
          ++m.bankRemoves;
        } else if ((r == SND_ERR_LIMIT_REACHED && place) || (r == SND_ERR_NOT_FOUND && !refuse) ||
                   (r != SND_ERR_LIMIT_REACHED && r != SND_ERR_NOT_FOUND)) {
          bad("RemoveBus bank", r);
        }
        check("RemoveBus bank");
      } else if (k < 93) {  // malformed edit: takes nothing
        const std::uint64_t badArgs[2] = {0, 9};
        const std::int32_t r = t.Ack(t.c->StructEdit(ll::StructOp::AddBus, ++nextId, badArgs, 2));
        if (r != SND_ERR_INVALID_ARGUMENT) bad("malformed", r);
        check("malformed");
      } else {  // disconnect: the revert undoes this session's edits; then reconnect
        const std::uint32_t conns = f.server->Stats().connections;
        t.c->Disconnect();
        SND_REQUIRE(f.StepUntil([&] {
          return f.server->Stats().connections > conns && t.c->State() == ll::ClientState::Ready && f.server->Connected();
        }));
        SND_REQUIRE(f.StepUntil([&] { return f.rt->StructLogPlaces() == 0u; }, 3000));
        m.Clear();
        check("revert");
      }
    }
    SND_CHECK_MSG(mismatches == 0u, "seed %u: %u mismatches", seed, mismatches);
    // the end: a revert empties everything
    const std::uint32_t conns = f.server->Stats().connections;
    t.c->Disconnect();
    SND_REQUIRE(f.StepUntil([&] {
      return f.server->Stats().connections > conns && t.c->State() == ll::ClientState::Ready && f.server->Connected();
    }));
    SND_CHECK(f.StepUntil([&] { return f.rt->StructLogPlaces() == 0u && f.rt->ReserveUsage().bytes == 0u; }, 3000));
    SND_CHECK_EQ(f.rt->Drops().structLog, 0u);
  }
}

// M3: a removed live bus takes its live effects and sends; the revert does not Undo them.
SND_TEST(IndepS7Recheck, RemovedLiveBusTakesItsEffectsAndSends) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.autoReconnect = false;
  Tool t(f, d);
  SND_REQUIRE(t.Ready());
  const std::uint64_t fx[3] = {HashId("snd.fx.gain"), 0xFFFFFFFFu, F32Bits(0.5f)};
  const std::uint64_t send[3] = {HashId("Master"), F32Bits(-3.0f), 1};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);   // 80
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B2"), kMasterArgs, 2)), SND_OK);   // 80
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 3)), SND_OK);         // 88
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B1"), fx, 2)), SND_OK);         // 80
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("B1"), send, 3)), SND_OK);         // 88
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddEffect, HashId("B2"), fx, 2)), SND_OK);         // 80
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("Sfx"), send, 2)), SND_OK);        // 80 (from a bank bus)
  // a send TO B1 from another bus is not "from it": it stays
  const std::uint64_t toB1[2] = {HashId("B1"), F32Bits(-3.0f)};
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddSend, HashId("B2"), toB1, 2)), SND_OK);         // 80
  ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 2u);
  SND_CHECK_EQ(u.effects, 3u);
  SND_CHECK_EQ(u.bytes, 80u + 80u + 88u + 80u + 88u + 80u + 80u + 80u);
  SND_CHECK_EQ(f.rt->StructLogPlaces(), 8u);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::RemoveBus, HashId("B1"), nullptr, 0)), SND_OK);
  u = f.rt->ReserveUsage();
  SND_CHECK_EQ(u.buses, 1u);
  SND_CHECK_EQ(u.effects, 1u);
  SND_CHECK_EQ(u.bytes, 80u + 80u + 80u + 80u);  // B2, its effect, Sfx's send, B2 -> B1 send
  SND_CHECK_EQ(f.rt->StructLogPlaces(), 4u);
  const std::uint32_t before = ed.Count();
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(10);
  // the revert undoes exactly the four left, newest first; nothing of B1's
  SND_REQUIRE_EQ(ed.Count(), before + 4u);
  const std::uint32_t expectOp[4] = {static_cast<std::uint32_t>(ll::StructOp::AddSend),
                                     static_cast<std::uint32_t>(ll::StructOp::AddSend),
                                     static_cast<std::uint32_t>(ll::StructOp::AddEffect),
                                     static_cast<std::uint32_t>(ll::StructOp::AddBus)};
  const std::uint32_t expectTarget[4] = {HashId("B2"), HashId("Sfx"), HashId("B2"), HashId("B2")};
  for (std::uint32_t i = 0; i < 4; ++i) {
    const FakeEditor::Entry e = ed.At(before + i);
    SND_CHECK_MSG(e.undo == 1u && e.op == expectOp[i] && e.targetId == expectTarget[i], "undo %u: op %u target %08x",
                  i, e.op, e.targetId);
  }
  u = f.rt->ReserveUsage();
  SND_CHECK(u.buses == 0u && u.effects == 0u && u.bytes == 0u);
  SND_CHECK_EQ(f.rt->StructLogPlaces(), 0u);
}

// --- M4: the predicates over hand-made recordings -----------------------------------------
namespace {
template <class T>
void Put(std::vector<std::uint8_t>& v, const T& rec) {
  const std::size_t at = v.size();
  v.resize(at + ((sizeof(T) + 7u) & ~std::size_t{7}));
  std::memcpy(v.data() + at, &rec, sizeof(T));
}
void PutArena(std::vector<std::uint8_t>& v, std::uint64_t block) {
  Put(v, ll::RecordArena{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block, 16u, 0u});
  Put(v, ll::ArenaSections{0, 0, 0, 0});
}
}  // namespace

SND_TEST(IndepS7Recheck, PredicateTable) {
  std::uint32_t rows = 0;
  for (std::uint32_t origin = 0; origin <= 2; ++origin) {  // 0: no Begin (1.0)
    for (std::uint32_t lost = 0; lost <= 1; ++lost) {
      if (origin == 0 && lost != 0) continue;
      for (std::uint32_t lod = 0; lod <= 1; ++lod) {
        for (std::uint32_t stall = 0; stall <= 1; ++stall) {
          std::vector<std::uint8_t> v;
          Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), lod != 0 ? 2u : 0u, 10u});
          if (origin != 0)
            Put(v, ll::RecordBegin{ll::kRecordBegin, static_cast<std::uint16_t>(origin), static_cast<std::uint16_t>(lost),
                                   10u});
          PutArena(v, 10);
          if (stall != 0) Put(v, ll::RecordStall{static_cast<std::uint32_t>(ll::RecordKind::Stall), 0u, 10u});
          PutArena(v, 11);
          snd_config cfg;
          snd_config_init(&cfg);
          ll::ReplayDesc d;
          d.config = &cfg;
          d.allocator = &snd::base::DefaultAllocator();
          d.record = v.data();
          d.bytes = v.size();
          ll::Replayer* rp = nullptr;
          SND_REQUIRE_EQ(ll::Replayer::Create(d, &rp), SND_OK);
          const bool bitExact = lod == 0 && stall == 0;
          const bool reproduces = bitExact && origin != 0 && lost == 0;
          SND_CHECK_MSG(rp->BitExactExpected() == bitExact, "origin %u lost %u lod %u stall %u: BitExact %d", origin,
                        lost, lod, stall, rp->BitExactExpected() ? 1 : 0);
          SND_CHECK_MSG(rp->ReproducesRecordedRun() == reproduces, "origin %u lost %u lod %u stall %u: Reproduces %d",
                        origin, lost, lod, stall, rp->ReproducesRecordedRun() ? 1 : 0);
          SND_CHECK_EQ((rp->Info().conditions & ll::kReplayMidRun) != 0u, origin == 2u);  // information only
          ll::Replayer::Destroy(rp);
          ++rows;
        }
      }
    }
  }
  SND_CHECK_EQ(rows, 20u);
}

// M4 (real): a recording that starts while a playback is alive counts it in lostState.
SND_TEST(IndepS7Recheck, PlaybackAliveAtTheStartIsLostState) {
  for (int alive = 0; alive <= 1; ++alive) {
    std::vector<std::uint8_t> bytes;
    {
      Fixture f;
      f.Load(IndepBank(), "a.sndb");
      SND_REQUIRE_EQ(f.StartServer(), SND_OK);
      Tool t(f);
      SND_REQUIRE(t.Ready());
      if (alive != 0) {
        f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
        f.Steps(5);
      }
      SND_REQUIRE_EQ(t.Ack(t.c->StartRecording(true)), SND_OK);
      SND_REQUIRE(f.StepUntil([&] { return f.rt->Recording(); }));
      f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
      f.Steps(10);
      SND_REQUIRE_EQ(t.Ack(t.c->StopRecording(true)), SND_OK);
      SND_REQUIRE(f.StepUntil([&] { return !f.rt->Recording(); }));
      std::uint64_t last = ~0ull;
      std::uint32_t stable = 0;
      f.StepUntil([&] {
        const std::uint64_t n = t.c->RecordingBytes();
        stable = (n == last && n != 0u) ? stable + 1u : 0u;
        last = n;
        return stable >= 20u;
      });
      bytes.resize(t.c->RecordingBytes());
      t.c->CopyRecording(bytes.data(), bytes.size());
    }
    const snd_config cfg = lltest::Config();
    ll::ReplayDesc d;
    d.config = &cfg;
    d.allocator = &snd::base::DefaultAllocator();
    d.record = bytes.data();
    d.bytes = bytes.size();
    ll::Replayer* rp = nullptr;
    SND_REQUIRE_EQ(ll::Replayer::Create(d, &rp), SND_OK);
    const ll::ReplayInfo i = rp->Info();
    if (alive != 0) {
      SND_CHECK(i.lostState >= 1u);
      SND_CHECK(rp->BitExactExpected());
      SND_CHECK(!rp->ReproducesRecordedRun());
    } else {
      SND_CHECK_EQ(i.lostState, 0u);
      SND_CHECK(rp->BitExactExpected());
      SND_CHECK(rp->ReproducesRecordedRun());
    }
    ll::Replayer::Destroy(rp);
  }
}

// M5: Finished() at the boundary with leadBlocks 0 (a hand-made recording of blocks 10..12).
SND_TEST(IndepS7Recheck, FinishedAtTheLastBlockWithoutLead) {
  std::vector<std::uint8_t> v;
  Put(v, ll::RecordLod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 10u});
  Put(v, ll::RecordBegin{ll::kRecordBegin, static_cast<std::uint16_t>(ll::RecordOrigin::FromCreation), 0u, 10u});
  PutArena(v, 10);
  PutArena(v, 12);
  const snd_config cfg = lltest::Config();
  ll::ReplayDesc d;
  d.config = &cfg;
  d.allocator = &snd::base::DefaultAllocator();
  d.record = v.data();
  d.bytes = v.size();
  ll::Replayer* rp = nullptr;
  SND_REQUIRE_EQ(ll::Replayer::Create(d, &rp), SND_OK);
  {
    Fixture g(Hooks::Replay, cfg, rp);
    SND_REQUIRE_EQ(rp->Blocks(), 3u);
    SND_CHECK_EQ(rp->Info().leadBlocks, 0u);
    g.Steps(2);
    SND_CHECK(!rp->Finished());  // not armed yet
    rp->Start();
    g.Steps(2);
    SND_CHECK(!rp->Finished());
    g.Step();
    SND_CHECK(rp->Finished());
    g.Steps(3);
    SND_CHECK(rp->Finished());
  }
  ll::Replayer::Destroy(rp);
}

// --- M7: one started Server per runtime -----------------------------------------------------
namespace {
struct Srv {
  ll::MemoryLink link;
  ll::Server* s = nullptr;
  int Create(Fixture& f) {
    if (!link.Init(snd::base::DefaultAllocator(), 1u << 16)) return SND_ERR_OUT_OF_BUDGET;
    ll::ServerDesc sd;
    sd.system = f.sys;
    sd.runtime = f.rt;
    sd.allocator = &snd::base::DefaultAllocator();
    sd.acceptor = &link.acceptor();
    return ll::Server::Create(sd, &s);
  }
  ~Srv() { ll::Server::Destroy(s); }
};
}  // namespace

SND_TEST(IndepS7Recheck, OneStartedServerPerRuntime) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  Srv a, b;
  SND_REQUIRE_EQ(a.Create(f), SND_OK);
  SND_REQUIRE_EQ(b.Create(f), SND_OK);
  SND_CHECK_EQ(a.s->Start(), SND_OK);
  SND_CHECK_EQ(b.s->Start(), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(b.s->Start(), SND_ERR_LIMIT_REACHED);
  b.s->Stop();  // stopping one that never started does not free the runtime
  SND_CHECK_EQ(b.s->Start(), SND_ERR_LIMIT_REACHED);
  a.s->Stop();
  SND_CHECK_EQ(b.s->Start(), SND_OK);
  SND_CHECK_EQ(a.s->Start(), SND_ERR_LIMIT_REACHED);
  ll::Server::Destroy(b.s);  // destroying the started one frees it
  b.s = nullptr;
  SND_CHECK_EQ(a.s->Start(), SND_OK);
  // the started one works
  ll::ClientDesc d;
  d.connector = &a.link.connector();
  d.projectBuildHash = lltest::kProjectHash;
  Tool t(f, d);
  SND_REQUIRE(f.StepUntil([&] { return t.c->State() == ll::ClientState::Ready && a.s->Connected(); }));
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  t.c->Stop();
  a.s->Stop();
  a.s->Stop();  // twice
  SND_CHECK_EQ(a.s->Start(), SND_OK);  // restart
}

SND_TEST(IndepS7Recheck, ConcurrentStartsNeverOverlap) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  std::atomic<int> running{0}, maxRunning{0};
  std::atomic<std::uint32_t> starts{0}, refusals{0}, other{0};
  std::atomic<bool> stop{false};
  auto worker = [&] {
    for (int i = 0; i < 25 && !stop.load(); ++i) {
      Srv s;
      if (s.Create(f) != SND_OK) {
        other.fetch_add(1);
        continue;
      }
      const int r = s.s->Start();
      if (r == SND_OK) {
        starts.fetch_add(1);
        const int now = running.fetch_add(1) + 1;
        int m = maxRunning.load();
        while (now > m && !maxRunning.compare_exchange_weak(m, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        running.fetch_sub(1);
        s.s->Stop();
      } else if (r == SND_ERR_LIMIT_REACHED) {
        refusals.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    }
  };
  std::thread t1(worker), t2(worker), t3(worker);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while ((starts.load() + refusals.load() + other.load()) < 75u && std::chrono::steady_clock::now() < end) {
    f.Step();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  stop.store(true);
  t1.join();
  t2.join();
  t3.join();
  SND_CHECK_EQ(maxRunning.load(), 1);
  SND_CHECK(starts.load() >= 1u);
  SND_CHECK_EQ(other.load(), 0u);
  std::printf("[indep_s7] concurrent starts: %u started, %u refused\n", starts.load(), refusals.load());
  // free again afterwards
  Srv s;
  SND_REQUIRE_EQ(s.Create(f), SND_OK);
  SND_CHECK_EQ(s.s->Start(), SND_OK);
}

// --- M8: after a ReloadBank of another bank's file ------------------------------------------
SND_TEST(IndepS7Recheck, StateAfterReloadOfAnotherBanksFile) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.fs.AddFile("two.sndb", IndepBank(two));
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "two.sndb")), SND_ERR_INVALID_ARGUMENT);
  f.Steps(20);
  auto banks = [&](std::vector<snd::Id>* ids) {
    const std::uint32_t q = t.c->QueryBanks(true);
    if (t.Ack(q) != SND_OK) return false;
    ll::BankEntry b[8];
    std::uint32_t rs = 0;
    const std::uint32_t n = t.c->CopyBanks(b, 8, &rs);
    ids->clear();
    for (std::uint32_t i = 0; i < n; ++i) ids->push_back(b[i].bankId);
    return rs == q;
  };
  std::vector<snd::Id> ids;
  SND_REQUIRE(banks(&ids));
  SND_CHECK(ids.empty());
  // edits find nothing now
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -3.0f))), SND_ERR_NOT_FOUND);
  // the requested bank is not loaded: a plain reload is NOT_FOUND, loadIfAbsent loads it
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "a.sndb")), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "a.sndb", true)), SND_OK);
  SND_REQUIRE(banks(&ids));
  SND_CHECK(ids.size() == 1u && ids[0] == kLive);
  // its cells are the bank's again, and edits work
  ll::CellValue cv{};
  SND_REQUIRE(WaitCell(t, t.c->ReadCell(SfxVolumeCell()), &cv));
  SND_CHECK_EQ(cv.current, F32Bits(0.0f));
  SND_CHECK_EQ(cv.flags & ll::kCellOverridden, 0u);
  SND_CHECK_EQ(t.Ack(t.c->SetValue(lltest::BusVolume("Sfx", -6.0f))), SND_OK);
  // and the next normal reload works
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "a.sndb")), SND_OK);
  // the other bank's file loads under its own id
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(HashId("Two"), 0, "two.sndb", true)), SND_OK);
  SND_REQUIRE(banks(&ids));
  SND_CHECK_EQ(ids.size(), 2u);
  // without an Ack the failure comes as an Error with its reason (§5: bit0 absent -> Error on failure)
  t.c->ClearErrors();
  SND_REQUIRE(t.c->ReloadBank(HashId("Nope"), 0, "two.sndb", true, false) != 0u);
  SND_CHECK(f.StepUntil([&] { return HasErrorText(t.c, "another bank"); }));
}

// D-173 says the other bank read from the file is not kept. When the game itself has that other
// bank loaded, a failed ReloadBank must not take the game's bank away: the request named
// another bank, and the game's registration is not something the reload loaded.
SND_TEST(IndepS7Recheck, Defect_FailedReloadUnloadsTheGamesOwnOtherBank) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  BankOpts two;
  two.name = "Two";
  f.Load(IndepBank(two), "two.sndb");  // loaded by the game
  SND_REQUIRE_EQ(f.StartServer(), SND_OK);
  Tool t(f);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(kLive, 0, "two.sndb")), SND_ERR_INVALID_ARGUMENT);
  f.Steps(20);
  const std::uint32_t q = t.c->QueryBanks(true);
  SND_REQUIRE_EQ(t.Ack(q), SND_OK);
  ll::BankEntry b[8];
  const std::uint32_t n = t.c->CopyBanks(b, 8);
  bool hasTwo = false, hasLive = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    hasTwo = hasTwo || b[i].bankId == HashId("Two");
    hasLive = hasLive || b[i].bankId == kLive;
  }
  SND_CHECK(!hasLive);  // the requested bank stays unloaded (D-173)
  SND_CHECK(hasTwo);    // the game's own "Two" is still loaded
  // the same through a language mismatch: (Two, 7) is requested, the game's (Two, 0) must stay
  SND_CHECK_EQ(t.Ack(t.c->ReloadBank(HashId("Two"), 7, "two.sndb", true)), SND_ERR_INVALID_ARGUMENT);
  f.Steps(20);
  const std::uint32_t q2 = t.c->QueryBanks(true);
  SND_REQUIRE_EQ(t.Ack(q2), SND_OK);
  const std::uint32_t n2 = t.c->CopyBanks(b, 8);
  bool stillTwo = false;
  for (std::uint32_t i = 0; i < n2; ++i) stillTwo = stillTwo || b[i].bankId == HashId("Two");
  SND_CHECK(stillTwo);
}

// --- M10: malformed edits while the revert log is full ---------------------------------------
SND_TEST(IndepS7Recheck, MalformedEditsWithAFullLog) {
  FakeEditor ed;
  ll::RuntimeDesc rd;
  rd.structEditor = &ed;
  rd.maxStructLog = 2;
  Fixture f(Hooks::Live, ReserveConfig(4, 4, 1u << 20), nullptr, rd);
  f.Load(IndepBank(), "a.sndb");
  ll::ServerDesc sd;
  sd.revertOnDisconnect = true;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::ClientDesc d = lltest::MemoryClient(f.link);
  d.autoReconnect = false;
  Tool t(f, d);
  SND_REQUIRE(t.Ready());
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B1"), kMasterArgs, 2)), SND_OK);
  SND_CHECK_EQ(t.Ack(t.c->StructEdit(ll::StructOp::AddBus, HashId("B2"), kMasterArgs, 2)), SND_OK);
  std::mt19937 rng(0xF00Du);
  std::vector<std::uint32_t> seqs;
  for (int i = 0; i < 400; ++i) {
    std::vector<std::uint8_t> b(16 + 8u * (rng() % 6u));
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    ll::StructEdit se{};
    std::memcpy(&se, b.data(), 16);
    se.op %= 6u;
    if (rng() % 3u == 0u) se.argCount = static_cast<std::uint32_t>((b.size() - 16u) / 8u);
    std::memcpy(b.data(), &se, 16);
    const std::uint32_t q = t.c->SendRaw(static_cast<std::uint16_t>(ll::MessageType::StructEdit), ll::kFrameWantsAck,
                                         b.data(), static_cast<std::uint32_t>(b.size()));
    if (q != 0u) seqs.push_back(q);
    if ((i & 15) == 0) f.Step();
  }
  std::uint32_t ok = 0, answered = 0;
  for (std::uint32_t q : seqs) {
    const std::int32_t r = t.Ack(q);
    answered += (r != 1 && r != 2) ? 1u : 0u;
    ok += r == SND_OK ? 1u : 0u;
  }
  SND_CHECK_EQ(answered, static_cast<std::uint32_t>(seqs.size()));
  // only removes that cancel B1 / B2 could be taken; the log never exceeds its places
  SND_CHECK(f.rt->StructLogPlaces() <= 2u);
  SND_CHECK_EQ(f.rt->Drops().structLog, 0u);
  t.c->Disconnect();
  SND_REQUIRE(f.StepUntil([&] { return !f.server->Connected(); }));
  f.Steps(10);
  SND_CHECK_EQ(f.rt->StructLogPlaces(), 0u);
  const ll::DevReserveUsage u = f.rt->ReserveUsage();
  SND_CHECK(u.buses == 0u && u.effects == 0u && u.bytes == 0u);
  std::printf("[indep_s7] malformed with a full log: %zu sent, %u taken\n", seqs.size(), ok);
}
