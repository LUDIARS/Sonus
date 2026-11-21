// 9.5 sliding / rolling: start on the first report, updates (largest speed of a call, speed ->
// volume / pitch, changed parameters), the timeout boundary in microseconds with the fade,
// eviction of the oldest (never one reported in the call), restart of a playback that ended
// by itself, overlaid single events, table replacement while tracking, destroy.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "phys_rig.hpp"

using namespace phystest;

namespace {

struct Fixture {
  explicit Fixture(snd_physics_config cfg, std::uint32_t materials = 2) {
    SND_REQUIRE_EQ(sys.created, SND_OK);
    SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, nullptr, &phys), SND_OK);
    snd_physics_table_desc d = SinglesTable(materials, &single);
    curves.resize(materials);
    std::memset(curves.data(), 0, sizeof(snd_physics_curves) * materials);
    curves[0].speed_to_volume = Linear({{0.0f, 0.0f}, {10.0f, 1.0f}});
    curves[0].speed_to_pitch = Linear({{0.0f, 0.5f}, {10.0f, 1.5f}});
    d.curves = curves.data();
    SND_REQUIRE_EQ(snd_physics_set_table(phys, &d), SND_OK);
  }
  ~Fixture() {
    if (phys != nullptr) snd_phys_destroy(phys);
  }
  snd_result Submit(const std::vector<snd_contact>& c, float dt = 0.05f) {
    return snd_phys_submit_contacts(phys, c.empty() ? nullptr : c.data(), static_cast<std::uint32_t>(c.size()), dt);
  }
  Sys sys;
  snd_phys* phys = nullptr;
  std::vector<snd_id> single;
  std::vector<snd_physics_curves> curves;
};

snd_contact Slide(std::uint64_t a, std::uint64_t b, float speed, std::uint16_t ma = 0, std::uint16_t mb = 0, float x = 0) {
  return Contact(SND_CONTACT_SLIDE, a, b, ma, mb, x, 0, 0, 0.0f, speed, 1.0f, 1.0f);
}

}  // namespace

SND_TEST(PhysicsTracking, StartUpdateAndTimeoutBoundary) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.tracking_timeout_seconds = 0.2f;  // 200000 us
  cfg.tracking_fade_seconds = 0.1f;
  Fixture f(cfg);
  const snd_id speedId = P(SND_PHYSICS_PARAM_SPEED_NAME);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 2.0f, 0, 0, 1.0f)}), SND_OK);  // t = 50 ms
  std::vector<Rec> s = rec.Of(SND_PHYSICS_RECORD_START);
  SND_REQUIRE_EQ(s.size(), 1u);
  SND_CHECK_EQ(s[0].r.event, Ev(0, SND_CONTACT_SLIDE));
  SND_CHECK_NEAR(s[0].r.volume, 0.2f, 1e-6f);
  SND_CHECK_NEAR(s[0].r.pitch, 0.7f, 1e-6f);
  SND_CHECK_EQ(s[0].Param(speedId), 2.0f);
  SND_CHECK(s[0].r.playback.value != 0u);
  SND_CHECK(s[0].r.emitter.value != 0u);
  SND_CHECK_EQ(s[0].r.position.x, 1.0f);
  const std::uint64_t handle = s[0].r.playback.value;
  // Two reports of the key in one call (objects in either order): the largest speed.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 3.0f, 0, 0, 2.0f), Slide(2, 1, 5.0f, 0, 0, 3.0f)}), SND_OK);  // t = 100 ms
  std::vector<Rec> u = rec.Of(SND_PHYSICS_RECORD_UPDATE);
  SND_REQUIRE_EQ(u.size(), 1u);
  SND_CHECK_EQ(u[0].r.playback.value, handle);
  SND_CHECK_EQ(u[0].r.speed, 5.0f);
  SND_CHECK_NEAR(u[0].r.volume, 0.5f, 1e-6f);
  SND_CHECK_NEAR(u[0].r.pitch, 1.0f, 1e-6f);
  SND_CHECK_EQ(u[0].r.position.x, 3.0f);
  SND_CHECK_EQ(u[0].r.result, SND_OK);
  snd_physics_stats st = Stats(f.phys);
  SND_CHECK_EQ(st.tracking_started, 1u);
  SND_CHECK_EQ(st.tracking_updated, 1u);
  SND_CHECK_EQ(st.slide_reports, 2u);
  SND_CHECK_EQ(st.merged, 1u);
  // No reports: 150, 200, 250 ms keep it; at 300 ms (exactly 200 ms after the last report) it stops.
  for (int i = 0; i < 3; ++i) {
    rec.Clear();
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  }
  rec.Clear();
  SND_CHECK_EQ(f.Submit({}), SND_OK);
  std::vector<Rec> stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  SND_REQUIRE_EQ(stop.size(), 1u);
  SND_CHECK_EQ(stop[0].r.stop_reason, SND_PHYSICS_STOP_TIMEOUT);
  SND_CHECK_EQ(stop[0].r.playback.value, handle);
  SND_CHECK_EQ(stop[0].r.result, SND_OK);
  SND_CHECK_EQ(Stats(f.phys).tracking_timeouts, 1u);
  // The stop was recorded with the fade: the handle is still current until the system ends it.
  SND_CHECK_EQ(snd_get_playback_state(f.sys.sys, snd_playback_handle{handle}) == SND_PLAYBACK_STATE_ENDED, false);
  // A new report starts it again.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK_EQ(Stats(f.phys).tracking_started, 2u);
}

SND_TEST(PhysicsTracking, AReportInTheSameCallKeepsItAlive) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.tracking_timeout_seconds = 0.0f;  // every entry not reported in a call stops
  Fixture f(cfg);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f), Slide(3, 4, 1.0f)}), SND_OK);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_CHECK_EQ(Stats(f.phys).tracking_timeouts, 1u);
  SND_CHECK_EQ(Stats(f.phys).tracking_updated, 1u);
  SND_CHECK_EQ(f.Submit({}, 0.0f), SND_OK);  // dt 0: elapsed 0 >= 0
  SND_CHECK_EQ(Stats(f.phys).tracking_timeouts, 2u);
}

SND_TEST(PhysicsTracking, EvictionOldestButNeverThisCall) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.max_tracked_pairs = 2;
  cfg.tracking_timeout_seconds = 10.0f;
  Fixture f(cfg);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);                      // A
  SND_CHECK_EQ(f.Submit({Slide(3, 4, 1.0f)}), SND_OK);                      // B
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f), Slide(5, 6, 1.0f)}), SND_OK);   // A reported, C new: B goes
  std::vector<Rec> stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  SND_REQUIRE_EQ(stop.size(), 1u);
  SND_CHECK_EQ(stop[0].r.object_a, 3u);
  SND_CHECK_EQ(stop[0].r.stop_reason, SND_PHYSICS_STOP_EVICTED);
  SND_CHECK_EQ(Stats(f.phys).tracking_evictions, 1u);
  // All entries reported in this call: the new one is dropped.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f), Slide(5, 6, 1.0f), Slide(7, 8, 1.0f)}), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_START), 0u);
  SND_CHECK_EQ(Stats(f.phys).tracking_dropped, 1u);
  // Equal last reports: the older start (A) goes.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(9, 10, 1.0f)}), SND_OK);
  stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  SND_REQUIRE_EQ(stop.size(), 1u);
  SND_CHECK_EQ(stop[0].r.object_a, 1u);
  SND_CHECK_EQ(Stats(f.phys).tracking_evictions, 2u);
  const snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.slide_reports, s.tracking_started + s.tracking_updated + s.tracking_restarted + s.tracking_dropped);
}

SND_TEST(PhysicsTracking, RestartsAPlaybackThatEndedByItself) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  // The events are not loaded: the playback ends (NOT_LOADED) at the system's next updates.
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  const std::uint64_t first = rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback.value;
  f.sys.Advance(6);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 2.0f)}), SND_OK);
  const std::vector<Rec> stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  const std::vector<Rec> start = rec.Of(SND_PHYSICS_RECORD_START);
  SND_REQUIRE_EQ(stop.size(), 1u);
  SND_REQUIRE_EQ(start.size(), 1u);
  SND_CHECK_EQ(stop[0].r.stop_reason, SND_PHYSICS_STOP_ENDED);
  SND_CHECK_EQ(stop[0].r.playback.value, first);
  SND_CHECK((start[0].r.flags & SND_PHYSICS_RECORD_FLAG_RESTART) != 0u);
  SND_CHECK(start[0].r.playback.value != 0u);
  SND_CHECK(start[0].r.playback.value != first);
  SND_CHECK_EQ(start[0].r.emitter.value, stop[0].r.emitter.value);  // same emitter
  SND_CHECK_EQ(start[0].r.speed, 2.0f);
  SND_CHECK_EQ(Stats(f.phys).tracking_restarted, 1u);
  SND_CHECK_EQ(Stats(f.phys).tracking_updated, 0u);
}

SND_TEST(PhysicsTracking, OverlaidSlidesTableReplacementAndDestroy) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.tracking_timeout_seconds = 10.0f;
  Fixture f(cfg);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_ROLL, 1, 2, 0, 1, 0, 0, 0, 0.0f, 4.0f, 1.0f, 1.0f)}), SND_OK);
  std::vector<Rec> s = rec.Of(SND_PHYSICS_RECORD_START);
  SND_REQUIRE_EQ(s.size(), 2u);
  SND_CHECK_EQ(s[0].r.event, Ev(0, SND_CONTACT_ROLL));
  SND_CHECK_EQ(s[1].r.event, Ev(1, SND_CONTACT_ROLL));
  SND_CHECK_EQ(s[0].r.emitter.value, s[1].r.emitter.value);
  SND_CHECK_NEAR(s[0].r.volume, 0.4f, 1e-6f);  // material 0's speed curve
  // Built-in speed_to_volume for material 1: 8 points 0.1 * 100^(i/7), -30 + 30 i/7 dB.
  {
    snd_physics_curve builtin{};
    builtin.count = 8;
    for (std::uint32_t i = 0; i < 8; ++i) {
      const double fr = static_cast<double>(i) / 7.0;
      builtin.points[i] = {static_cast<float>(0.1 * std::pow(100.0, fr)),
                           static_cast<float>(std::pow(10.0, (-30.0 + 30.0 * fr) / 20.0))};
    }
    SND_CHECK_NEAR(s[1].r.volume, EvalCurve(builtin, 4.0f), 1e-6f);
    SND_CHECK_EQ(s[1].r.pitch, 1.0f);
  }
  SND_CHECK_EQ(Stats(f.phys).fallback_pair_to_single, 1u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_FALLBACK), 1u);
  // Replace the table: the tracked playbacks go on with their events, with the new curves.
  std::vector<snd_id> other;
  snd_physics_table_desc d = SinglesTable(2, &other);
  std::vector<snd_id> evs(8, 0x4444u);
  d.single = evs.data();
  snd_physics_curves c[2];
  std::memset(c, 0, sizeof(c));
  c[0].speed_to_volume = Linear({{0.0f, 0.9f}});
  d.curves = c;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_ROLL, 2, 1, 1, 0, 0, 0, 0, 0.0f, 4.0f, 1.0f, 1.0f)}), SND_OK);
  const std::vector<Rec> u = rec.Of(SND_PHYSICS_RECORD_UPDATE);
  SND_REQUIRE_EQ(u.size(), 2u);
  SND_CHECK_EQ(u[0].r.event, Ev(0, SND_CONTACT_ROLL));  // orientation of the start is kept
  SND_CHECK_EQ(u[0].r.volume, 0.9f);
  SND_CHECK_EQ(u[1].r.event, Ev(1, SND_CONTACT_ROLL));
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_START), 0u);
  // Destroy stops both with the fade.
  rec.Clear();
  snd_phys_destroy(f.phys);
  f.phys = nullptr;
  const std::vector<Rec> stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  SND_REQUIRE_EQ(stop.size(), 2u);
  SND_CHECK_EQ(stop[0].r.stop_reason, SND_PHYSICS_STOP_DESTROYED);
  SND_CHECK_EQ(stop[1].r.stop_reason, SND_PHYSICS_STOP_DESTROYED);
}

SND_TEST(PhysicsTracking, SurfaceParameterChangesReachATrackedSlide) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.tracking_timeout_seconds = 10.0f;
  Fixture f(cfg);
  const snd_id wet = P("Surface/Wetness");
  snd_physics_material_entry e{};
  e.sound_material = 0;
  e.param_count = 1;
  e.params[0] = {wet, 0.0f};
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, &e, 1), SND_OK);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_START)[0].Param(wet), 0.0f);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, wet, 1.0f), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  const std::vector<Rec> u = rec.Of(SND_PHYSICS_RECORD_UPDATE);
  SND_REQUIRE_EQ(u.size(), 1u);
  SND_CHECK_EQ(u[0].Param(wet), 1.0f);
  // Unknown materials: the default Slide event; nothing at all: no entry.
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, SND_CONTACT_SLIDE, 0x7100u), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f), Slide(3, 4, 1.0f, 5, 5)}), SND_OK);
  std::vector<Rec> s = rec.Of(SND_PHYSICS_RECORD_START);
  SND_REQUIRE_EQ(s.size(), 1u);
  SND_CHECK_EQ(s[0].r.event, 0x7100u);
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, SND_CONTACT_ROLL, SND_ID_NONE), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_ROLL, 5, 6, 9, 9, 0, 0, 0, 0.0f, 1.0f)}), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_START), 0u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_FALLBACK), 1u);
  const snd_physics_stats st = Stats(f.phys);
  SND_CHECK_EQ(st.fallback_none, 1u);
  SND_CHECK_EQ(st.slide_reports,
               st.tracking_started + st.tracking_updated + st.tracking_restarted + st.tracking_dropped + st.fallback_none);
}

SND_TEST(PhysicsTracking, TimeoutComesBeforeEvictionAndImpactsDoNotPushOutReports) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.max_tracked_pairs = 1;
  cfg.tracking_timeout_seconds = 0.1f;
  cfg.max_groups_per_call = 4;
  Fixture f(cfg);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}, 0.05f), SND_OK);
  // 100 ms later, without a report from A and with a new key B: A times out (step 2) before B
  // starts (step 3); no eviction.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Slide(3, 4, 1.0f)}, 0.1f), SND_OK);
  const std::vector<Rec> stop = rec.Of(SND_PHYSICS_RECORD_STOP);
  SND_REQUIRE_EQ(stop.size(), 1u);
  SND_CHECK_EQ(stop[0].r.stop_reason, SND_PHYSICS_STOP_TIMEOUT);
  SND_CHECK_EQ(Stats(f.phys).tracking_timeouts, 1u);
  SND_CHECK_EQ(Stats(f.phys).tracking_evictions, 0u);
  SND_CHECK_EQ(Stats(f.phys).tracking_started, 2u);
  // A burst of Impacts that fills the Impact / Break work area does not push out the report
  // of the tracked slide (it does not use that area).
  std::vector<snd_contact> burst;
  for (int i = 0; i < 20; ++i) burst.push_back(Impact(static_cast<std::uint64_t>(i) * 2u + 100u, 0, static_cast<float>(i), 1.0f));
  burst.push_back(Slide(3, 4, 2.0f));
  rec.Clear();
  SND_CHECK_EQ(f.Submit(burst, 0.05f), SND_OK);
  SND_CHECK_EQ(Stats(f.phys).dropped_work_overflow, 16u);
  SND_CHECK_EQ(Stats(f.phys).tracking_updated, 1u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_UPDATE), 1u);
}
