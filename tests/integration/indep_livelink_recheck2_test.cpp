// Independent re-check 2 of the live link (acceptance decisions R1 / R2):
//  - session markers: results matched to the Server that recorded the command;
//  - the values the API accepts at the edges, replayed one by one: core survives,
//    the output stays finite, nothing is left out;
//  - core's defensive limits seen through the game API (bpm, volume, fade).
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include "indep_livelink_common.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/livelink/record_format.hpp"

using namespace indep;
using lltest::Fixture;
using lltest::Hooks;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

ll::Server* NewServer(Fixture& f) {
  ll::ServerDesc sd;
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

void WaitRecorded(ll::Server* s, std::uint32_t n) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (s->Stats().commandsRecorded < n && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // flushed
}

bool ReadyWithoutRendering(ll::Client* c, ll::Server* s) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(c->State() == ll::ClientState::Ready && s->Connected()) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return c->State() == ll::ClientState::Ready && s->Connected();
}

ll::SetValue Unknown() {
  ll::SetValue v = lltest::BusVolume("Sfx", -6.0f);
  v.runtimeId = HashId("NoSuchBus");
  return v;
}

}  // namespace

// --- session markers -----------------------------------------------------------------------

// Several Servers in a row on one runtime (more than max_registered_threads, so producer
// slots are reused), each destroyed with edits recorded but not applied. Each new Server
// answers its own commands with their own results; the old results are dropped and counted.
SND_TEST(IndepLivelinkSession, ServersInARowAnswerOnlyTheirOwnCommands) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  SND_REQUIRE(f.link.Init(snd::base::DefaultAllocator(), 1u << 20));
  SND_REQUIRE(f.cfg_.max_registered_threads < 8u);
  for (int round = 0; round < 8; ++round) {
    f.server = NewServer(f);
    SND_REQUIRE(f.server != nullptr);
    ll::Client* c = nullptr;
    SND_REQUIRE_EQ(ll::Client::Create(lltest::MemoryClient(f.link), &c), SND_OK);
    c->Start();
    SND_REQUIRE(ReadyWithoutRendering(c, f.server));
    // The previous Server's 3 edits are still unapplied: apply them together with ours.
    const std::uint32_t bad = c->SetValue(Unknown(), true);
    const std::uint32_t good = c->SetValue(lltest::BusVolume("Sfx", -1.0f - static_cast<float>(round)), true);
    WaitRecorded(f.server, 2u);
    std::int32_t rb = 1, rg = 1;
    const bool gotBad = f.StepUntil([&] { return c->TakeAck(bad, &rb); });
    const bool gotGood = f.StepUntil([&] { return c->TakeAck(good, &rg); });
    SND_CHECK_MSG(gotBad && rb == SND_ERR_NOT_FOUND, "round %d: Ack of the bad edit %d", round, rb);
    SND_CHECK_MSG(gotGood && rg == SND_OK, "round %d: Ack of the good edit %d", round, rg);
    if (round > 0) SND_CHECK_EQ(f.server->Stats().staleResults, 3u);
    // Leave 3 edits unapplied for the next Server.
    for (int i = 0; i < 3; ++i) c->SetValue(lltest::BusVolume("Master", -0.5f), true);
    WaitRecorded(f.server, 5u);
    ll::Server::Destroy(f.server);
    f.server = nullptr;
    ll::Client::Destroy(c);
  }
}

// The marker in an arena of its own, edits in later arenas, then a new marker: each
// applied result carries the session of the latest marker of its producer. Records from
// the owner thread, results read from the runtime (no server).
SND_TEST(IndepLivelinkSession, MarkerOnlyArenaAndLaterArenas) {
  Fixture f;
  f.Load(IndepBank(), "a.sndb");
  auto marker = [&](std::uint32_t s) {
    cmd::LiveSetValue m{};
    m.field = ll::kFieldSession;
    m.value = s;
    SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m)), SND_OK);
  };
  auto edit = [&](float db) {
    cmd::LiveSetValue c{};
    c.objectId = HashId("Sfx");
    c.field = ll::EncodeField(bf::chunk::BUSS, bf::col::BUSS_volumeDb);
    c.value = F32Bits(db);
    SND_REQUIRE_EQ(f.sys->RecordLive(cmd::CommandType::LiveSetValue, &c, sizeof(c)), SND_OK);
  };
  auto results = [&] {
    std::vector<ll::ApplyResult> v;
    ll::ApplyResult r{};
    while (f.rt->results().TryPop(r)) v.push_back(r);
    return v;
  };
  marker(77u);
  f.Step();  // an arena with the marker only
  SND_CHECK(results().empty());  // the marker has no result of its own
  edit(-3.0f);
  f.Step();
  edit(-4.0f);
  f.Step();
  std::vector<ll::ApplyResult> r = results();
  SND_REQUIRE_EQ(r.size(), 2u);
  SND_CHECK_EQ(r[0].session, 77u);
  SND_CHECK_EQ(r[1].session, 77u);
  marker(78u);
  edit(-5.0f);
  f.Step();
  r = results();
  SND_REQUIRE_EQ(r.size(), 1u);
  SND_CHECK_EQ(r[0].session, 78u);
  SND_CHECK_EQ(r[0].result, SND_OK);
  SND_CHECK_EQ(f.rt->applier().Remembered(), 1u);  // the marker is not an edit
}

// A recording that contains session markers replays them as no-ops: nothing is left
// out, and the replay renders bit-identically to a replay of the same recording
// without the markers.
SND_TEST(IndepLivelinkSession, ReplayPassesMarkersThrough) {
  std::vector<std::uint8_t> rec;
  {
    Fixture f;
    f.Load(IndepBank(), "a.sndb");
    f.Steps(60);
    f.rt->RequestRecording(true);
    f.Step();
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    cmd::LiveSetValue m{};
    m.field = ll::kFieldSession;
    m.value = 5u;
    f.sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m));
    f.Steps(3);
    cmd::LiveSetValue c{};
    c.objectId = HashId("Sfx");
    c.field = ll::EncodeField(bf::chunk::BUSS, bf::col::BUSS_volumeDb);
    c.value = F32Bits(-6.0f);
    f.sys->RecordLive(cmd::CommandType::LiveSetValue, &m, sizeof(m));  // marker again, same arena
    f.sys->RecordLive(cmd::CommandType::LiveSetValue, &c, sizeof(c));
    f.Steps(6);
    f.rt->RequestRecording(false);
    f.Steps(2);
    std::uint32_t n = 0;
    while (f.rt->records().Next(&n)) {
      std::vector<std::uint8_t> msg(n);
      f.rt->records().Copy(0, msg.data(), n);
      f.rt->records().Pop();
      std::uint32_t kind = 0;
      std::memcpy(&kind, msg.data(), 4);
      if (kind != ll::kRecordEndMarker) rec.insert(rec.end(), msg.begin(), msg.end());
    }
  }
  SND_REQUIRE(!rec.empty());
  // The same recording with the markers turned into Resume (a no-op of the same size? no:
  // sizes differ) - instead strip them by rebuilding each arena without them.
  std::vector<std::uint8_t> stripped;
  {
    ll::RecordReader rr(rec.data(), rec.size());
    ll::RecordItem it;
    while (rr.Next(&it)) {
      const std::uint8_t* start = rec.data() + it.offset;
      if (it.kind != ll::RecordKind::Arena) {
        stripped.insert(stripped.end(), start, start + 16);
        continue;
      }
      std::vector<std::uint8_t> normal;
      snd::cmd::ForEachCommand(it.normal, it.normal + it.sections.normalBytes,
                               [&](const cmd::CommandHeader& h, const std::uint8_t* p) {
                                 if (h.type == static_cast<std::uint16_t>(cmd::CommandType::LiveSetValue)) {
                                   cmd::LiveSetValue v;
                                   std::memcpy(&v, p, sizeof(v));
                                   if (v.field == ll::kFieldSession) return;
                                 }
                                 normal.insert(normal.end(), p - sizeof(h), p - sizeof(h) + h.sizeBytes);
                               });
      ll::RecordArena ra = it.arena;
      ll::ArenaSections s = it.sections;
      s.normalBytes = static_cast<std::uint32_t>(normal.size());
      ra.bytes = static_cast<std::uint32_t>(sizeof(s) + s.normalBytes + s.reserveBytes + s.fallbackBytes);
      const auto* rp = reinterpret_cast<const std::uint8_t*>(&ra);
      stripped.insert(stripped.end(), rp, rp + sizeof(ra));
      const auto* sp = reinterpret_cast<const std::uint8_t*>(&s);
      stripped.insert(stripped.end(), sp, sp + sizeof(s));
      stripped.insert(stripped.end(), normal.begin(), normal.end());
      stripped.insert(stripped.end(), it.reserve, it.reserve + it.sections.reserveBytes);
      if (it.fallback != nullptr) stripped.insert(stripped.end(), it.fallback, it.fallback + it.sections.fallbackBytes);
    }
    SND_REQUIRE(rr.Error() == ll::RecordError::None);
  }
  SND_REQUIRE(stripped.size() < rec.size());
  auto replay = [](const std::vector<std::uint8_t>& r, ll::ReplayInfo* info) {
    std::vector<float> out;
    const snd_config cfg = lltest::Config();
    ll::ReplayDesc rd;
    rd.config = &cfg;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.record = r.data();
    rd.bytes = r.size();
    ll::Replayer* rp = nullptr;
    if (ll::Replayer::Create(rd, &rp) != SND_OK) return out;
    {
      Fixture f(Hooks::Replay, cfg, rp);
      f.Load(IndepBank(), "a.sndb");
      while (f.sys->LastBlock().block + 1u < rp->Info().firstBlock) f.Step();
      rp->Start();
      for (std::uint64_t i = 0; i < rp->Blocks() + 4u; ++i) {
        f.sys->RenderBlock(f.out.data());
        out.insert(out.end(), f.out.begin(), f.out.end());
      }
      *info = rp->Info();
    }
    ll::Replayer::Destroy(rp);
    return out;
  };
  ll::ReplayInfo a{}, b{};
  const std::vector<float> withMarkers = replay(rec, &a);
  const std::vector<float> without = replay(stripped, &b);
  SND_REQUIRE(!withMarkers.empty());
  SND_REQUIRE_EQ(withMarkers.size(), without.size());
  SND_CHECK(std::memcmp(withMarkers.data(), without.data(), without.size() * sizeof(float)) == 0);
  SND_CHECK_EQ(a.invalidCommands, 0u);
  SND_CHECK_EQ(a.commands, b.commands + 2u);
  // The edit is audible in both (-6 dB on the tone at the end).
  const double last = withMarkers[withMarkers.size() - 2];
  SND_CHECK(last > 0.0);
}

// --- the 10 API-accepted extremes of the boundary test, replayed one by one ------------------
namespace {
template <class P>
void PutCmd(std::vector<std::uint8_t>& b, cmd::CommandType t, const P& payload) {
  const std::uint32_t size = cmd::kCommandInfo[static_cast<std::uint16_t>(t)].wireSize;
  std::vector<std::uint8_t> c(size, 0);
  const cmd::CommandHeader h{static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(size), 0u};
  std::memcpy(c.data(), &h, sizeof(h));
  std::memcpy(c.data() + sizeof(h), &payload, sizeof(P));
  b.insert(b.end(), c.begin(), c.end());
}
void PutArena(std::vector<std::uint8_t>& rec, std::uint64_t block, const std::vector<std::uint8_t>& normal) {
  const ll::RecordArena ra{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, block,
                           static_cast<std::uint32_t>(sizeof(ll::ArenaSections) + normal.size()), 0u};
  const ll::ArenaSections s{static_cast<std::uint32_t>(normal.size()), 0u, 0u, 0u};
  const auto* a = reinterpret_cast<const std::uint8_t*>(&ra);
  rec.insert(rec.end(), a, a + sizeof(ra));
  const auto* sp = reinterpret_cast<const std::uint8_t*>(&s);
  rec.insert(rec.end(), sp, sp + sizeof(s));
  rec.insert(rec.end(), normal.begin(), normal.end());
}

// Lod, then an arena with a transport and a playing Tone (playback slot 0, gen 1) and an
// emitter, then an arena with `extreme`.
template <class P>
std::vector<std::uint8_t> WithExtreme(cmd::CommandType t, const P& extreme) {
  std::vector<std::uint8_t> rec;
  const ll::RecordLod lod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 100u};
  const auto* lp = reinterpret_cast<const std::uint8_t*>(&lod);
  rec.insert(rec.end(), lp, lp + sizeof(lod));
  std::vector<std::uint8_t> a1;
  cmd::CreateTransport ct{};
  ct.transport = snd::HandleRef{0u, 1u};
  ct.desc.bpm = 120.0f;
  ct.desc.beats_per_bar = 4;
  ct.desc.beat_unit = 4;
  ct.desc.start_playing = 1;
  PutCmd(a1, cmd::CommandType::CreateTransport, ct);
  PutCmd(a1, cmd::CommandType::CreateEmitter, cmd::CreateEmitter{snd::HandleRef{0u, 1u}});
  cmd::Play p{};
  p.playback = snd::HandleRef{0u, 1u};
  p.event = HashId("Tone");
  PutCmd(a1, cmd::CommandType::Play, p);
  PutArena(rec, 100u, a1);
  std::vector<std::uint8_t> a2;
  PutCmd(a2, t, extreme);
  PutArena(rec, 103u, a2);
  std::vector<std::uint8_t> a3;
  cmd::Play q{};
  q.playback = snd::HandleRef{1u, 1u};
  q.event = HashId("Blip");
  PutCmd(a3, cmd::CommandType::Play, q);  // the system still works afterwards
  PutArena(rec, 140u, a3);
  return rec;
}

bool ReplayFinite(const std::vector<std::uint8_t>& rec, const char* what) {
  const snd_config cfg = lltest::Config();
  ll::ReplayDesc rd;
  rd.config = &cfg;
  rd.allocator = &snd::base::DefaultAllocator();
  rd.record = rec.data();
  rd.bytes = rec.size();
  ll::Replayer* rp = nullptr;
  if (ll::Replayer::Create(rd, &rp) != SND_OK) {
    SND_CHECK_MSG(false, "%s: recording refused", what);
    return false;
  }
  bool finite = true;
  double energy = 0;
  {
    Fixture f(Hooks::Replay, cfg, rp);
    f.Load(IndepBank(), "a.sndb");
    rp->Start();
    for (std::uint64_t i = 0; i < rp->Blocks() + 200u; ++i) {
      f.Step();
      for (float x : f.out) {
        finite = finite && std::isfinite(x);
        energy += static_cast<double>(x) * x;
      }
    }
    const ll::ReplayInfo info = rp->Info();
    SND_CHECK_MSG(info.invalidCommands == 0u, "%s: %u commands left out", what, info.invalidCommands);
  }
  ll::Replayer::Destroy(rp);
  SND_CHECK_MSG(finite, "%s: non-finite output", what);
  SND_CHECK_MSG(energy > 0.0, "%s: silent", what);
  return finite;
}
}  // namespace

SND_TEST(IndepLivelinkExtremes, EachApiAcceptedExtremeReplaysSafely) {
  const snd_config cfg = lltest::Config();
  cmd::Play base{};
  base.playback = snd::HandleRef{2u, 1u};
  base.event = HashId("Tone");
  {
    cmd::Play p = base;
    p.emitter = snd::HandleRef{cfg.max_emitters, 3u};
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play emitter out of range");
  }
  {
    cmd::Play p = base;
    p.when.kind = SND_WHEN_DELAY_US;
    p.when.value = 3600ull * 1000000ull + 1u;
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play DELAY_US > 1 h");
    p.when.value = ~0ull;
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play DELAY_US max");
  }
  {
    cmd::Play p = base;
    p.when.reserved[5] = 0xFFu;
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play When reserved bytes");
  }
  {
    cmd::Play p = base;
    p.when.kind = SND_WHEN_GRID;
    p.when.count = 257u;
    p.when.transport.value = SND_HANDLE_MAKE(0u, 1u);
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play GRID 257");
    p.when.count = 0xFFFFFFFFu;
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play GRID max");
    p.when.count = 0u;
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play GRID 0");
  }
  {
    cmd::Play p = base;
    p.when.kind = SND_WHEN_NEXT_BAR;
    p.when.transport.value = SND_HANDLE_MAKE(cfg.max_transports, 1u);
    ReplayFinite(WithExtreme(cmd::CommandType::Play, p), "Play transport out of range");
  }
  {
    cmd::Stop s{snd::HandleRef{0u, 1u}, kInf, 0u, snd_when{}};
    ReplayFinite(WithExtreme(cmd::CommandType::Stop, s), "Stop fade +Inf");
  }
  {
    cmd::SetListener l{};
    l.listener.forward = snd_vec3{0, 0, 1};
    l.listener.up = snd_vec3{0, 1, 0};
    l.listener.enabled = 1u;
    l.listener.weight = -0.5f;
    ReplayFinite(WithExtreme(cmd::CommandType::SetListener, l), "Listener weight < 0");
    l.listener.weight = 0.0f;
    l.listener.enabled = 2u;
    ReplayFinite(WithExtreme(cmd::CommandType::SetListener, l), "Listener enabled 2");
    l.listener.enabled = 1u;
    l.listener.forward = snd_vec3{0, 0, 0};
    l.listener.up = snd_vec3{0, 0, 0};
    ReplayFinite(WithExtreme(cmd::CommandType::SetListener, l), "Listener zero axes");
  }
  {
    cmd::SeekTransport sk{};
    sk.transport = snd::HandleRef{0u, 1u};
    sk.position.tick = static_cast<std::int32_t>(SND_TICKS_PER_BEAT);
    ReplayFinite(WithExtreme(cmd::CommandType::SeekTransport, sk), "Seek tick 960");
    sk.position.tick = 0;
    sk.position.bar = -1;
    ReplayFinite(WithExtreme(cmd::CommandType::SeekTransport, sk), "Seek bar -1");
    sk.position.bar = std::numeric_limits<std::int32_t>::max();
    sk.position.beat = std::numeric_limits<std::int32_t>::max();
    sk.position.tick = std::numeric_limits<std::int32_t>::max();
    ReplayFinite(WithExtreme(cmd::CommandType::SeekTransport, sk), "Seek max");
    sk.position.bar = std::numeric_limits<std::int32_t>::min();
    sk.position.beat = std::numeric_limits<std::int32_t>::min();
    sk.position.tick = std::numeric_limits<std::int32_t>::min();
    ReplayFinite(WithExtreme(cmd::CommandType::SeekTransport, sk), "Seek min");
  }
}

// --- core's defensive limits, through the game API -------------------------------------------
namespace {
std::vector<float> RunApi(void (*script)(Fixture&), std::uint32_t blocks) {
  Fixture f(Hooks::None);
  f.Load(IndepBank(), "a.sndb");
  script(f);
  f.Steps(blocks);
  return f.all;
}
bool AllFinite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}
std::uint64_t TransportTicks(Fixture& f, snd_transport_handle t) {
  snd_position pos{};
  f.sys->Update();
  f.sys->GetTransportPosition(t, &pos);
  return static_cast<std::uint64_t>(pos.musical.bar) * 4u * SND_TICKS_PER_BEAT +
         static_cast<std::uint64_t>(pos.musical.beat) * SND_TICKS_PER_BEAT + static_cast<std::uint64_t>(pos.musical.tick);
}
std::uint64_t TicksAfter(float bpm, std::uint32_t blocks) {
  Fixture f(Hooks::None);
  f.Load(IndepBank(), "a.sndb");
  snd_transport_desc td{};
  td.bpm = bpm;
  td.beats_per_bar = 4;
  td.beat_unit = 4;
  td.start_playing = 1;
  const snd_transport_handle t = f.sys->CreateTransport(&td);
  f.Steps(2);
  const std::uint64_t t0 = TransportTicks(f, t);
  f.Steps(blocks);
  return TransportTicks(f, t) - t0;
}
}  // namespace

// Volume above 1000 acts as 1000 (bit-identical output), a fade above a day acts as a
// day, +Inf fades too; the output stays finite.
SND_TEST(IndepLivelinkExtremes, CoreClampsVolumesAndFadesFromTheApi) {
  const std::vector<float> v1000 = RunApi([](Fixture& f) {
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    SND_CHECK_EQ(f.sys->SetBusVolume(HashId("Sfx"), 1000.0f, 0.0f), SND_OK);
  }, 30);
  const std::vector<float> vBig = RunApi([](Fixture& f) {
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    SND_CHECK_EQ(f.sys->SetBusVolume(HashId("Sfx"), 1.0e30f, 0.0f), SND_OK);
  }, 30);
  const std::vector<float> v1 = RunApi([](Fixture& f) {
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  }, 30);
  SND_REQUIRE_EQ(v1000.size(), vBig.size());
  SND_CHECK(std::memcmp(v1000.data(), vBig.data(), v1000.size() * sizeof(float)) == 0);
  SND_CHECK(AllFinite(vBig));
  const std::size_t last = v1.size() - 2;
  SND_CHECK_NEAR(v1000[last] / v1[last], 1000.0, 1.0);
  // Fades: one day, more than a day, +Inf: identical; the fade has barely begun.
  auto fade = [](float s) {
    struct S {
      static float& Value() {
        static float v = 0;
        return v;
      }
    };
    S::Value() = s;
    return RunApi([](Fixture& f) {
      const snd_playback_handle h = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
      f.Steps(4);
      SND_CHECK_EQ(f.sys->Stop(h, S::Value(), nullptr), SND_OK);
    }, 40);
  };
  const std::vector<float> day = fade(86400.0f), more = fade(1.0e9f), inf = fade(kInf);
  SND_REQUIRE_EQ(day.size(), more.size());
  SND_CHECK(std::memcmp(day.data(), more.data(), day.size() * sizeof(float)) == 0);
  SND_CHECK(std::memcmp(day.data(), inf.data(), day.size() * sizeof(float)) == 0);
  SND_CHECK(AllFinite(inf));
  SND_CHECK(day[day.size() - 2] > 0.9 * v1[last]);
  // Volume ramps with a huge fade time stay finite too.
  const std::vector<float> slow = RunApi([](Fixture& f) {
    f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
    f.sys->SetBusVolume(HashId("Sfx"), 0.0f, 3.0e38f);
    f.sys->SetCategoryVolume(0u, 1.0e30f, 1.0e30f);
  }, 30);
  SND_CHECK(AllFinite(slow));
}

// Tempo: bpm above 1e5 runs at 1e5, below 1e-3 at 1e-3 (same musical progress).
SND_TEST(IndepLivelinkExtremes, CoreClampsTheTempoFromTheApi) {
  const std::uint64_t hi = TicksAfter(1.0e5f, 20), higher = TicksAfter(1.0e9f, 20), huge = TicksAfter(3.0e38f, 20);
  SND_CHECK(hi > 0u);
  SND_CHECK_EQ(hi, higher);
  SND_CHECK_EQ(hi, huge);
  const std::uint64_t mid = TicksAfter(120.0f, 20);
  SND_CHECK(mid > 0u && mid < hi);
  const std::uint64_t lo = TicksAfter(1.0e-3f, 2000), lower = TicksAfter(1.0e-9f, 2000);
  SND_CHECK_EQ(lo, lower);
  // SetTempo is clamped the same way.
  Fixture f(Hooks::None);
  f.Load(IndepBank(), "a.sndb");
  snd_transport_desc td{};
  td.bpm = 120.0f;
  td.beats_per_bar = 4;
  td.beat_unit = 16;
  td.start_playing = 1;
  const snd_transport_handle t = f.sys->CreateTransport(&td);
  SND_CHECK_EQ(f.sys->SetTempo(t, 3.0e38f, nullptr), SND_OK);
  f.Steps(50);
  snd_stats st{};
  f.sys->Update();
  f.sys->GetStats(&st);
  SND_CHECK(AllFinite(f.all));
}
