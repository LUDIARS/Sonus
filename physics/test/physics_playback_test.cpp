// Real playback through the C API (manual output device, banks made by buildtool): a
// positioned Impact really plays at its position (create emitter -> transform -> play ->
// destroy emitter), the emitter slots come back, volume from the curves scales the level,
// the parameters physics passes (velocity, surface parameters) reach the event's curves,
// snd_physics_play_at, and a sliding contact loops while reported, follows the speed and
// fades out after the timeout.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "mini_project.hpp"
#include "phys_rig.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/buildtool/wav.hpp"

using namespace phystest;
namespace bt = snd::buildtool;

namespace {

constexpr float kCenter = 0.70710678f;

bt::PcmData Dc(std::uint64_t frames, float v) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = 48000;
  p.frames = frames;
  p.samples.assign(static_cast<std::size_t>(frames), v);
  return p;
}

std::vector<std::uint8_t> BuildBank() {
  mini::Project proj("phys-play");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true, \"channels\": 2");
  proj.Wav("audio/hit.wav", Dc(12000, 0.5f), bt::WavSampleFormat::Float32);
  proj.Wav("audio/loop.wav", Dc(48000, 0.5f), bt::WavSampleFormat::Float32);
  const std::string f32 = "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}";
  proj.Add("assets/hit.json", "asset", "hit", "\"file\": \"audio/hit.wav\", " + f32);
  proj.Add("assets/loop.json", "asset", "loop", "\"file\": \"audio/loop.wav\", \"loop\": {\"start\": 0, \"end\": 48000}, " + f32);
  proj.Add("params/Vel.json", "parameter", "Phys/Velocity", "\"min\": 0, \"max\": 1");
  proj.Add("params/Wet.json", "parameter", "Surface/Wetness", "\"min\": 0, \"max\": 1");
  proj.Add("events/Hit.json", "event", "SE/Hit",
           "\"bank\": \"Main\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"hit\"}]}]");
  proj.Add("events/Hit2D.json", "event", "SE/Hit2D", "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"hit\"}]}]");
  proj.Add("events/Layer.json", "event", "SE/Layer",
           "\"bank\": \"Main\", \"curves\": [{\"parameter\": \"Phys/Velocity\", \"target\": \"volume\", "
           "\"points\": [[0, -40], [1, 0]]}], \"tracks\": [{\"clips\": [{\"asset\": \"hit\"}]}]");
  proj.Add("events/Wet.json", "event", "SE/Wet",
           "\"bank\": \"Main\", \"curves\": [{\"parameter\": \"Surface/Wetness\", \"target\": \"volume\", "
           "\"points\": [[0, -40], [1, 0]]}], \"tracks\": [{\"clips\": [{\"asset\": \"hit\"}]}]");
  proj.Add("events/Scrape3D.json", "event", "SE/Scrape3D",
           "\"bank\": \"Main\", \"is3D\": true, \"tracks\": [{\"clips\": [{\"asset\": \"loop\", \"loopCount\": 0}]}]");
  proj.Add("events/Scrape.json", "event", "SE/Scrape",
           "\"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"loop\", \"loopCount\": 0}]}]");
  bt::Diagnostics diag;
  const bool ok = proj.Build(&diag);
  if (!ok) snd::test::Note("build failed:\n%s", mini::Dump(diag).c_str());
  return ok ? mini::ReadBytes(proj.out() / "Main.sndb") : std::vector<std::uint8_t>{};
}

struct World {
  World() {
    bank = BuildBank();
    if (bank.empty() || sys.created != SND_OK) return;
    snd_bank_handle h{};
    if (!sys.LoadBank("Main.sndb", bank, &h)) return;
    snd_listener_desc l{};
    l.forward = snd_vec3{0, 0, 1};
    l.up = snd_vec3{0, 1, 0};
    l.enabled = 1;
    l.weight = 1.0f;
    if (snd_set_listener(sys.sys, 0, &l) != SND_OK) return;
    sys.Advance(2);
    ok = true;
  }
  explicit World(snd_config c) : sys(c) {
    bank = BuildBank();
    snd_bank_handle h{};
    ok = !bank.empty() && sys.created == SND_OK && sys.LoadBank("Main.sndb", bank, &h);
    snd_listener_desc l{};
    l.forward = snd_vec3{0, 0, 1};
    l.up = snd_vec3{0, 1, 0};
    l.enabled = 1;
    l.weight = 1.0f;
    ok = ok && snd_set_listener(sys.sys, 0, &l) == SND_OK;
  }
  double Mean(int ch, std::uint64_t from, std::uint64_t n) const {
    double s = 0.0;
    for (std::uint64_t f = from; f < from + n; ++f) s += ch == 0 ? sys.L(f) : sys.R(f);
    return s / static_cast<double>(n);
  }
  Sys sys;
  std::vector<std::uint8_t> bank;
  bool ok = false;
};

// One material whose Impact is `event`, constant volume `volume`, pitch 1.
void OneEventTable(snd_phys* p, snd_id event, float volume, snd_contact_kind kind = SND_CONTACT_IMPACT) {
  static std::vector<snd_id> single;
  single.assign(4, SND_ID_NONE);
  single[kind] = event;
  static snd_physics_curves def;
  std::memset(&def, 0, sizeof(def));
  def.impulse_to_volume = Linear({{0.0f, volume}});
  def.impulse_to_velocity = Linear({{0.0f, 0.0f}, {10.0f, 1.0f}});
  def.mass_to_pitch = Linear({{0.0f, 1.0f}});
  def.speed_to_volume = Linear({{0.0f, 0.0f}, {10.0f, 1.0f}});
  def.speed_to_pitch = Linear({{0.0f, 1.0f}});
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 1;
  d.single = single.data();
  d.default_curves = &def;
  snd_physics_set_table(p, &d);
}

}  // namespace

SND_TEST(PhysicsPlayback, PositionedImpactPlaysAtItsPositionAndEmittersComeBack) {
  snd_config sc = SmallConfig();
  sc.max_emitters = 4;
  World w(sc);
  SND_REQUIRE(w.ok);
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(w.sys.sys, &cfg, nullptr, &phys), SND_OK);
  OneEventTable(phys, snd_hash_id("SE/Hit"), 1.0f);
  // Right of the listener at 2 m: DC 0.5 x 1/2, all on the right.
  struct Case {
    float x;
    float volume;
    double wantL, wantR;
  };
  const Case cases[] = {{2.0f, 1.0f, 0.0, 0.25}, {-2.0f, 1.0f, 0.25, 0.0}, {2.0f, 0.5f, 0.0, 0.125}};
  for (const Case& c : cases) {
    OneEventTable(phys, snd_hash_id("SE/Hit"), c.volume);
    const std::uint64_t start = w.sys.Frames();
    const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, c.x, 0, 0, 5.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, &x, 1, 0.1f), SND_OK);
    w.sys.Advance(60);  // the hit is 12000 frames (47 periods)
    // Find the first audible frame, then measure 20 periods in.
    std::uint64_t on = start;
    while (on < w.sys.Frames() && std::fabs(w.sys.L(on)) + std::fabs(w.sys.R(on)) < 1e-4f) ++on;
    SND_REQUIRE(on < start + 10u * kBlock);
    SND_CHECK_NEAR(w.Mean(0, on + 512, 4096), c.wantL, 2e-3);
    SND_CHECK_NEAR(w.Mean(1, on + 512, 4096), c.wantR, 2e-3);
    // It ends: silence at the end of the window.
    SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 512, 512) + w.Mean(1, w.sys.Frames() - 512, 512), 0.0, 1e-5);
  }
  // 4 emitters only: 24 more hits over time all get one (the slots come back after Update).
  for (int i = 0; i < 24; ++i) {
    const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 2.0f, 0, 0, 5.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, &x, 1, 0.1f), SND_OK);
    w.sys.Advance(4);
  }
  const snd_physics_stats s = Stats(phys);
  SND_CHECK_EQ(s.play_at_fallbacks, 0u);
  SND_CHECK_EQ(s.api_failures, 0u);
  SND_CHECK_EQ(s.requested_impact, 27u);
  for (const Rec& r : rec.Of(SND_PHYSICS_RECORD_PLAY)) SND_CHECK(r.r.emitter.value != 0u && r.r.playback.value != 0u);
  w.sys.Advance(60);
  // snd_physics_play_at: straight ahead at 2 m (equal power on both sides).
  {
    const std::uint64_t start = w.sys.Frames();
    const snd_vec3 front{0, 0, 2};
    SND_CHECK_EQ(snd_physics_play_at(phys, snd_hash_id("SE/Hit"), &front), SND_OK);
    w.sys.Advance(30);
    std::uint64_t on = start;
    while (on < w.sys.Frames() && std::fabs(w.sys.L(on)) < 1e-4f) ++on;
    SND_REQUIRE(on < start + 10u * kBlock);
    SND_CHECK_NEAR(w.Mean(0, on + 512, 2048), 0.25 * kCenter, 2e-3);
    SND_CHECK_NEAR(w.Mean(1, on + 512, 2048), 0.25 * kCenter, 2e-3);
    SND_CHECK_EQ(Stats(phys).requested_play_at, 1u);
    const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
    SND_CHECK_EQ(p.back().r.source, SND_PHYSICS_SOURCE_PLAY_AT);
    SND_CHECK_EQ(p.back().r.event, snd_hash_id("SE/Hit"));
  }
  w.sys.Advance(60);
  // No emitter left (the game holds them all): the group falls back to snd_play_at, whose
  // implicit emitter comes from the same pool, so it plays without a position (at the
  // listener: centre, no attenuation; requests/A-004.md).
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, nullptr, 0, 0.1f), SND_OK);  // releases physics' held emitter
  w.sys.Advance(8);
  {
    std::vector<snd_emitter_handle> held;
    for (int i = 0; i < 8; ++i) {
      const snd_emitter_handle e = snd_create_emitter(w.sys.sys);
      if (e.value != 0u) held.push_back(e);
    }
    SND_REQUIRE_EQ(held.size(), 4u);
    const std::uint64_t start = w.sys.Frames();
    const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, -2.0f, 0, 0, 5.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, &x, 1, 0.1f), SND_OK);
    SND_CHECK_EQ(Stats(phys).play_at_fallbacks, 1u);
    w.sys.Advance(30);
    std::uint64_t on = start;
    while (on < w.sys.Frames() && std::fabs(w.sys.L(on)) < 1e-4f) ++on;
    SND_REQUIRE(on < start + 10u * kBlock);
    SND_CHECK_NEAR(w.Mean(0, on + 512, 2048), 0.5 * kCenter, 2e-3);
    SND_CHECK_NEAR(w.Mean(1, on + 512, 2048), 0.5 * kCenter, 2e-3);
    for (const snd_emitter_handle e : held) snd_destroy_emitter(w.sys.sys, e);
  }
  snd_phys_destroy(phys);
}

SND_TEST(PhysicsPlayback, ParametersReachTheEventCurves) {
  World w;
  SND_REQUIRE(w.ok);
  snd_physics_config cfg = OpenConfig(nullptr);
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(w.sys.sys, &cfg, nullptr, &phys), SND_OK);
  auto level = [&](const snd_contact& x) {
    const std::uint64_t start = w.sys.Frames();
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, &x, 1, 0.1f), SND_OK);
    w.sys.Advance(60);
    std::uint64_t on = start;
    while (on < w.sys.Frames() && std::fabs(w.sys.L(on)) < 1e-7f) ++on;
    if (on + 4096 > w.sys.Frames()) return 0.0;
    return w.Mean(0, on + 1024, 2048) / (0.5 * kCenter);
  };
  // Velocity: impulse 10 -> velocity 1 -> 0 dB; impulse 0 -> velocity 0 -> -40 dB.
  OneEventTable(phys, snd_hash_id("SE/Layer"), 1.0f);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 10.0f)), 1.0, 2e-3);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 0.0f)), 0.01, 2e-4);
  // Volume from the impulse curve multiplies (0.5).
  OneEventTable(phys, snd_hash_id("SE/Layer"), 0.5f);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 10.0f)), 0.5, 2e-3);
  // Surface parameter: game 1 is the same sound material, wet.
  OneEventTable(phys, snd_hash_id("SE/Wet"), 1.0f);
  snd_physics_material_entry map[2];
  std::memset(map, 0, sizeof(map));
  map[0].sound_material = 0;
  map[1].sound_material = 0;
  map[1].param_count = 1;
  map[1].params[0] = {snd_hash_id("Surface/Wetness"), 1.0f};
  SND_REQUIRE_EQ(snd_physics_set_material_map(phys, map, 2), SND_OK);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 1, 1, 0, 0, 0, 1.0f)), 1.0, 2e-3);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 1.0f)), 0.01, 2e-4);
  // It starts to rain on game material 0 too.
  SND_CHECK_EQ(snd_physics_set_surface_param(phys, 0, snd_hash_id("Surface/Wetness"), 1.0f), SND_OK);
  SND_CHECK_NEAR(level(Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 1.0f)), 1.0, 2e-3);
  snd_phys_destroy(phys);
}

SND_TEST(PhysicsPlayback, SlidingLoopsFollowsTheSpeedAndFadesOut) {
  World w;
  SND_REQUIRE(w.ok);
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.tracking_timeout_seconds = 0.05f;
  cfg.tracking_fade_seconds = 0.02f;  // 960 frames
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(w.sys.sys, &cfg, nullptr, &phys), SND_OK);
  OneEventTable(phys, snd_hash_id("SE/Scrape"), 1.0f, SND_CONTACT_SLIDE);
  const float dt = static_cast<float>(kBlock) / 48000.0f;
  auto report = [&](float speed, int frames) {
    for (int i = 0; i < frames; ++i) {
      const snd_contact c = Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, 0, 0, 0, 0.0f, speed);
      SND_CHECK_EQ(snd_phys_submit_contacts(phys, &c, 1, dt), SND_OK);
      w.sys.Advance(1);
    }
  };
  const double full = 0.5 * kCenter;  // DC 0.5, 2D at the centre
  report(10.0f, 60);  // > 1 s of 48000-frame loop: it keeps going
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), full, 1e-3);
  report(5.0f, 20);
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), 0.5 * full, 1e-3);
  report(10.0f, 220);  // past the end of the asset: the loop continues
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), full, 1e-3);
  SND_CHECK_EQ(Stats(phys).tracking_started, 1u);
  SND_CHECK_EQ(Stats(phys).tracking_restarted, 0u);
  SND_CHECK_EQ(Stats(phys).tracking_updated, 299u);
  const snd_playback_handle h = rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback;
  // Reports stop: 10 calls of 5333 us reach 50000 us; the fade then takes 960 frames.
  const std::uint64_t quietFrom = w.sys.Frames();
  int stoppedAt = -1;
  for (int i = 0; i < 40; ++i) {
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, nullptr, 0, dt), SND_OK);
    if (stoppedAt < 0 && Stats(phys).tracking_timeouts == 1u) stoppedAt = i + 1;
    w.sys.Advance(1);
  }
  SND_CHECK_EQ(stoppedAt, 10);
  const snd_playback_state endState = snd_get_playback_state(w.sys.sys, h);  // ended (or its slot returned)
  SND_CHECK(endState == SND_PLAYBACK_STATE_ENDED || endState == SND_PLAYBACK_STATE_NONE);
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), 0.0, 1e-6);
  // The fade: from the last full-level frame to the first silent one takes about 960 frames.
  std::uint64_t lastFull = quietFrom, firstSilent = 0;
  for (std::uint64_t f = quietFrom; f < w.sys.Frames(); ++f) {
    if (w.sys.L(f) > 0.999 * full) lastFull = f;
    if (firstSilent == 0 && f > lastFull && w.sys.L(f) < 1e-6) firstSilent = f;
  }
  SND_REQUIRE(firstSilent > lastFull);
  SND_CHECK_MSG(firstSilent - lastFull >= 900u && firstSilent - lastFull <= 1100u, "fade %llu frames",
                static_cast<unsigned long long>(firstSilent - lastFull));
  snd_phys_destroy(phys);
}

SND_TEST(PhysicsPlayback, SlidingContactFollowsItsPosition) {
  World w;
  SND_REQUIRE(w.ok);
  snd_physics_config cfg = OpenConfig(nullptr);
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(w.sys.sys, &cfg, nullptr, &phys), SND_OK);
  OneEventTable(phys, snd_hash_id("SE/Scrape3D"), 1.0f, SND_CONTACT_SLIDE);
  const float dt = static_cast<float>(kBlock) / 48000.0f;
  auto report = [&](float x, int frames) {
    for (int i = 0; i < frames; ++i) {
      const snd_contact c = Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, x, 0, 0, 0.0f, 10.0f);
      SND_CHECK_EQ(snd_phys_submit_contacts(phys, &c, 1, dt), SND_OK);
      w.sys.Advance(1);
    }
  };
  report(2.0f, 20);  // started at its position: right at 2 m (DC 0.5 x 1/2)
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), 0.0, 2e-3);
  SND_CHECK_NEAR(w.Mean(1, w.sys.Frames() - 1024, 1024), 0.25, 2e-3);
  report(-2.0f, 20);  // moved with the reports
  SND_CHECK_NEAR(w.Mean(0, w.sys.Frames() - 1024, 1024), 0.25, 2e-3);
  SND_CHECK_NEAR(w.Mean(1, w.sys.Frames() - 1024, 1024), 0.0, 2e-3);
  SND_CHECK_EQ(Stats(phys).tracking_restarted, 0u);
  snd_phys_destroy(phys);
}
