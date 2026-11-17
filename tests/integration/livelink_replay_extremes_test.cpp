// Replay of commands the game API accepts, at their extremes (acceptance decision R2):
// command_check lets through exactly what the API entry lets through, so core must
// survive every such value (NaN / Inf / FLT_MAX in the fields the API does not
// check: transforms, listeners, Pause fades, Seek positions, When counts / values /
// offsets / reserved bytes, transports of musical When kinds, weights, huge
// volumes, fades and tempi ...). Run under ASan + UBSan: no memory error, no UB;
// the output stays finite. SND_FUZZ_ITERATIONS scales it.
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>

#include "livelink_fixture.hpp"
#include "snd/livelink/command_check.hpp"
#include "snd/livelink/record_format.hpp"

using namespace lltest;
using snd::HashId;
namespace cmd = snd::cmd;

namespace {

std::uint32_t Iterations(std::uint32_t def) {
  const char* e = std::getenv("SND_FUZZ_ITERATIONS");
  return e != nullptr ? static_cast<std::uint32_t>(std::strtoul(e, nullptr, 10)) : def;
}

struct Gen {
  std::mt19937 rng;
  explicit Gen(std::uint32_t s) : rng(s) {
    // SND_EXTREMES_ONLY=<n>[,<n>...]: generate only these command kinds (exploration).
    if (const char* e = std::getenv("SND_EXTREMES_ONLY")) {
      for (const char* q = e; *q != '\0';) {
        char* end = nullptr;
        only.push_back(static_cast<std::uint32_t>(std::strtoul(q, &end, 10)));
        q = *end == ',' ? end + 1 : end;
        if (end == q && *q != '\0') break;
      }
    }
  }
  std::vector<std::uint32_t> only;
  std::uint32_t R() { return static_cast<std::uint32_t>(rng()); }
  float F() {
    static const float kF[] = {0.0f,
                               -0.0f,
                               1.0f,
                               -1.0f,
                               0.5f,
                               1.5f,
                               5000.0f,
                               -5000.0f,
                               2.0e6f,
                               1.0e30f,
                               -1.0e30f,
                               std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::denorm_min(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()};
    return kF[R() % (sizeof(kF) / sizeof(kF[0]))];
  }
  std::uint32_t U() {
    static const std::uint32_t kU[] = {0u, 1u, 2u, 3u, 4u, 7u, 8u, 15u, 16u, 255u, 256u, 0xFFFFu, 0x7FFFFFFFu,
                                       0x80000000u, 0xFFFFFFFFu};
    return R() % 4u == 0u ? R() : kU[R() % (sizeof(kU) / sizeof(kU[0]))];
  }
  snd::HandleRef Playback() { return snd::HandleRef{R() % 8u, 1u}; }
  snd::HandleRef Emitter() { return snd::HandleRef{R() % 4u, 1u}; }
  snd::HandleRef TransportH() {
    return R() % 4u == 0u ? snd::HandleRef{SND_TRANSPORT_PLAYBACK_BIT | (R() % 8u), 1u} : snd::HandleRef{R() % 4u, 1u};
  }
  snd_when When() {
    snd_when w{};
    w.kind = static_cast<std::uint8_t>(R() % 9u);
    w.flags = static_cast<std::uint8_t>(R() % 2u);
    w.count = U();
    w.value = (static_cast<std::uint64_t>(U()) << 32) | U();
    w.offset_ticks = static_cast<std::int32_t>(U());
    w.transport.value = R() % 3u == 0u ? ((static_cast<std::uint64_t>(U()) << 32) | U()) : TransportH().Value();
    for (auto& r : w.reserved) r = static_cast<std::uint8_t>(R() % 3u == 0u ? R() : 0u);
    return w;
  }
  snd::Vec3 V() { return snd::Vec3{F(), F(), F()}; }
  snd_vec3 SV() { return snd_vec3{F(), F(), F()}; }

  template <class P>
  void Put(std::vector<std::uint8_t>& out, cmd::CommandType t, const P& p) {
    const std::uint32_t size = cmd::kCommandInfo[static_cast<std::uint16_t>(t)].wireSize;
    std::vector<std::uint8_t> c(size, 0);
    const cmd::CommandHeader h{static_cast<std::uint16_t>(t), static_cast<std::uint16_t>(size), 0u};
    std::memcpy(c.data(), &h, sizeof(h));
    std::memcpy(c.data() + sizeof(h), &p, sizeof(P));
    out.insert(out.end(), c.begin(), c.end());
  }

  // One API-shaped command with extreme values; kept only if command_check accepts it.
  void One(std::vector<std::uint8_t>& out, const snd::livelink::CommandLimits& k, std::uint32_t* kept) {
    using CT = cmd::CommandType;
    std::vector<std::uint8_t> c;
    std::uint32_t kind = R() % 22u;
    if (!only.empty()) kind = only[R() % only.size()];
    switch (kind) {
      case 0: {
        cmd::Play p{};
        p.playback = Playback();
        p.emitter = R() % 2u ? Emitter() : snd::HandleRef{U(), U()};
        p.event = R() % 2u ? HashId("Tone") : HashId("Blip");
        p.when = When();
        if (R() % 2u) {
          p.flags = cmd::kPlayOneShot | cmd::kPlayHasPosition;
          p.position = V();
        }
        Put(c, CT::Play, p);
        break;
      }
      case 1: Put(c, CT::Stop, cmd::Stop{Playback(), F(), 0u, When()}); break;
      case 2: Put(c, CT::PauseResume, cmd::PauseResume{Playback(), F(), R() % 2u}); break;
      case 3: Put(c, CT::SetScheduledStop, cmd::SetScheduledStop{Playback(), F(), 0u, When()}); break;
      case 4: Put(c, CT::CreateEmitter, cmd::CreateEmitter{Emitter()}); break;
      case 5: Put(c, CT::DestroyEmitter, cmd::DestroyEmitter{Emitter()}); break;
      case 6: Put(c, CT::SetTransform, cmd::SetTransform{Emitter(), V(), V(), V(), R() % 8u}); break;
      case 7: {
        cmd::SetListener l{};
        l.index = R() % 4u;
        l.listener = snd_listener_desc{SV(), SV(), SV(), SV(), F(), R() % 3u == 0u ? U() : R() % 2u};
        Put(c, CT::SetListener, l);
        break;
      }
      case 8: {
        cmd::SetParam p{};
        p.scope = static_cast<std::uint8_t>(R() % 3u);
        p.target = p.scope == 0u ? snd::HandleRef{} : (p.scope == 1u ? Emitter() : Playback());
        p.param = HashId("Knob");
        p.value = F();
        Put(c, CT::SetParam, p);
        break;
      }
      case 9: Put(c, CT::SetBusVolume, cmd::SetBusVolume{R() % 2u ? HashId("Sfx") : 0u, F(), std::fabs(F()), 0u}); break;
      case 10: Put(c, CT::ApplySnapshot, cmd::ApplySnapshot{HashId("Snap"), F(), std::fabs(F()), 0u}); break;
      case 11: Put(c, CT::SetCategoryVolume, cmd::SetCategoryVolume{R() % 2u ? HashId("Cat") : 0u, F(), std::fabs(F()), 0u}); break;
      case 12: {
        cmd::CreateTransport t{};
        t.transport = snd::HandleRef{R() % 4u, 1u};
        t.desc.bpm = F();
        t.desc.beats_per_bar = static_cast<std::uint16_t>(U());
        static const std::uint16_t kUnits[] = {2, 4, 8, 16};
        t.desc.beat_unit = kUnits[R() % 4u];
        t.desc.start_playing = R() % 3u == 0u ? U() : 1u;
        Put(c, CT::CreateTransport, t);
        break;
      }
      case 13: Put(c, CT::SetTransportState, cmd::SetTransportState{TransportH(), R() % 3u, 0u, When()}); break;
      case 14: Put(c, CT::SetTempo, cmd::SetTempo{TransportH(), F(), 0u, When()}); break;
      case 15: {
        cmd::SeekTransport s{};
        s.transport = TransportH();
        s.position = snd_musical_time{static_cast<std::int32_t>(U()), static_cast<std::int32_t>(U()),
                                      static_cast<std::int32_t>(U())};
        s.when = When();
        Put(c, CT::SeekTransport, s);
        break;
      }
      case 16: Put(c, CT::DestroyTransport, cmd::DestroyTransport{snd::HandleRef{R() % 4u, 1u}}); break;
      case 17: Put(c, CT::SetMusicState, cmd::SetMusicState{Playback(), U(), 0u, When()}); break;
      case 18: {
        cmd::SetSwitch s{};
        s.scope = 0u;
        s.group = U();
        s.value = U();
        Put(c, CT::SetSwitch, s);
        break;
      }
      case 19: Put(c, CT::PauseCategory, cmd::PauseCategory{U(), R() % 2u}); break;
      case 20: Put(c, CT::MuteCategory, cmd::MuteCategory{U(), R() % 2u}); break;
      default: {
        cmd::LiveOverride o{};
        o.kind = R() % 3u;
        o.id = HashId("Knob");
        const float f = F();
        std::memcpy(&o.value, &f, 4);
        o.enable = R() % 2u;
        Put(c, CT::LiveOverride, o);
        break;
      }
    }
    cmd::CommandHeader h;
    std::memcpy(&h, c.data(), sizeof(h));
    if (!snd::livelink::CheckCommand(h, c.data() + sizeof(h), k)) return;
    out.insert(out.end(), c.begin(), c.end());
    ++*kept;
  }
};

}  // namespace

SND_TEST(LivelinkReplayExtremes, ApiAcceptableExtremesNeitherCrashNorProduceNonFiniteOutput) {
  const snd_config cfg = Config();
  const snd::livelink::CommandLimits k = snd::livelink::LimitsOf(cfg);
  Gen g(0x5EEDu);
  const std::uint32_t runs = Iterations(30);
  std::uint32_t keptTotal = 0;
  for (std::uint32_t run = 0; run < runs; ++run) {
    std::vector<std::uint8_t> rec;
    const snd::livelink::RecordLod lod{static_cast<std::uint32_t>(snd::livelink::RecordKind::Lod), 0u, 100u};
    rec.insert(rec.end(), reinterpret_cast<const std::uint8_t*>(&lod), reinterpret_cast<const std::uint8_t*>(&lod) + 16);
    for (std::uint32_t a = 0; a < 24u; ++a) {
      std::vector<std::uint8_t> cmds;
      std::uint32_t kept = 0;
      for (std::uint32_t i = 0; i < 1u + g.R() % 12u; ++i) g.One(cmds, k, &kept);
      keptTotal += kept;
      const snd::livelink::RecordArena ra{static_cast<std::uint32_t>(snd::livelink::RecordKind::Arena), 0u, 100u + a * 2u,
                                          static_cast<std::uint32_t>(sizeof(snd::livelink::ArenaSections) + cmds.size()),
                                          0u};
      const snd::livelink::ArenaSections s{static_cast<std::uint32_t>(cmds.size()), 0u, 0u, 0u};
      rec.insert(rec.end(), reinterpret_cast<const std::uint8_t*>(&ra), reinterpret_cast<const std::uint8_t*>(&ra) + sizeof(ra));
      rec.insert(rec.end(), reinterpret_cast<const std::uint8_t*>(&s), reinterpret_cast<const std::uint8_t*>(&s) + sizeof(s));
      rec.insert(rec.end(), cmds.begin(), cmds.end());
    }
    snd::livelink::ReplayDesc rd;
    rd.config = &cfg;
    rd.allocator = &snd::base::DefaultAllocator();
    rd.record = rec.data();
    rd.bytes = rec.size();
    snd::livelink::Replayer* rp = nullptr;
    SND_REQUIRE_EQ(snd::livelink::Replayer::Create(rd, &rp), SND_OK);
    SND_CHECK_EQ(rp->Info().invalidCommands, 0u);  // only API-acceptable commands were written
    {
      Fixture f(Hooks::Replay, cfg, rp);
      f.Load(DevBank());
      rp->Start();
      bool finite = true;
      for (std::uint64_t b = 0; b < rp->Blocks() + 24u; ++b) {
        f.sys->RenderBlock(f.out.data());
        for (float x : f.out) finite = finite && std::isfinite(x);
      }
      SND_CHECK_MSG(finite, "run %u: non-finite output", run);
    }
    snd::livelink::Replayer::Destroy(rp);
  }
  SND_CHECK(keptTotal > runs * 50u);
}
