// Independent tests: sliding / rolling contacts (9.5, decision D). Properties P31..P35.
// The events are real (a bank built in the test): I/Loop loops, I/Short ends by itself.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kLoop = snd_hash_id("I/Loop");
const snd_id kShort = snd_hash_id("I/Short");

struct Fx {
  explicit Fx(snd_physics_config c = Cfg(nullptr), snd_id slideEvent = kLoop) {
    loaded = s.LoadBank("phys.sndb", TestBank(), &bank);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys == nullptr) return;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 2;
    single.assign(8, SND_ID_NONE);
    single[0 * 4 + SND_CONTACT_SLIDE] = slideEvent;
    single[0 * 4 + SND_CONTACT_ROLL] = slideEvent;
    d.single = single.data();
    tableResult = snd_physics_set_table(phys, &d);
  }
  ~Fx() { snd_physics_destroy(phys); }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 0.05f) {
    rec.Clear();
    const snd_result r =
        snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
    s.Advance(1);
    return r;
  }
  bool Ok() const { return loaded && created == SND_OK && tableResult == SND_OK; }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Slide(std::uint64_t a, std::uint64_t b, float speed, float x = 0.0f, std::uint16_t ma = 0,
                  std::uint16_t mb = 0, snd_contact_kind k = SND_CONTACT_SLIDE) {
  return Contact(k, a, b, ma, mb, x, 0.0f, 1.0f, 0.0f, speed);
}

// Ended (ENDED, or NONE once the slot is released).
bool Gone(const snd_system* sys, snd_playback_handle h) {
  const snd_playback_state st = snd_get_playback_state(sys, h);
  return st == SND_PLAYBACK_STATE_ENDED || st == SND_PLAYBACK_STATE_NONE;
}

}  // namespace

// P31, P32: start, update, stop exactly at the timeout, emitter returned.
SND_TEST(IndepTrack, StartUpdateTimeoutExact) {
  snd_physics_config c = Cfg(nullptr);
  c.tracking_timeout_seconds = 0.2f;  // 200000 us; dt 0.05 = 50000 us
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  const Rec start = f.rec.Of(SND_PHYSICS_RECORD_START)[0];
  SND_CHECK_EQ(start.r.event, kLoop);
  SND_CHECK(start.r.playback.value != 0u);
  SND_CHECK(start.r.emitter.value != 0u);
  for (int i = 0; i < 3; ++i) {
    SND_CHECK_EQ(f.Submit({Slide(2, 1, 2.0f + static_cast<float>(i), 0.1f * static_cast<float>(i))}), SND_OK);
    SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_UPDATE), 1u);
    const Rec u = f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0];
    SND_CHECK_EQ(u.r.playback.value, start.r.playback.value);
    SND_CHECK_EQ(u.Param(P(SND_PHYSICS_PARAM_SPEED_NAME)), 2.0f + static_cast<float>(i));
    SND_CHECK_EQ(u.r.position.x, 0.1f * static_cast<float>(i));
  }
  SND_CHECK_EQ(snd_get_playback_state(f.s.sys, snd_playback_handle{start.r.playback.value}), SND_PLAYBACK_STATE_PLAYING);
  // Not reported: 3 empty calls (150000 us) keep it, the 4th (200000 us, exactly) stops it.
  for (int i = 0; i < 3; ++i) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  }
  SND_CHECK_EQ(f.Submit({}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  const Rec stop = f.rec.Of(SND_PHYSICS_RECORD_STOP)[0];
  SND_CHECK_EQ(stop.r.stop_reason, SND_PHYSICS_STOP_TIMEOUT);
  SND_CHECK_EQ(stop.r.playback.value, start.r.playback.value);
  // (Whether the returned emitter handle is already reported stale depends on when core
  // applies the destroy; the emitter count is checked in DestroyStopsAndReturnsEmitters.)
  // The fade (0.1 s default) ends the playback.
  f.s.Advance(40);
  SND_CHECK(Gone(f.s.sys, start.r.playback));
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_started, 1u);
  SND_CHECK_EQ(st.tracking_updated, 3u);
  SND_CHECK_EQ(st.tracking_timeouts, 1u);
  SND_CHECK_EQ(st.slide_reports, 4u);
}

// P13: count == 0 with a large dt processes the timeouts (dt clamped to max_dt: 0.1).
SND_TEST(IndepTrack, TimeoutWithEmptyCallsAndClamp) {
  snd_physics_config c = Cfg(nullptr);
  c.tracking_timeout_seconds = 0.2f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_CHECK_EQ(f.Submit({}, 10.0f), SND_OK);  // clamped to 0.1 s: 100000 us < 200000
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  SND_CHECK_EQ(f.Submit({}, 10.0f), SND_OK);  // 200000: stop
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  // A rejected dt does not advance the timeouts.
  SND_CHECK_EQ(f.Submit({Slide(3, 4, 1.0f)}), SND_OK);
  SND_CHECK_EQ(f.Submit({}, kNaN), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(f.Submit({}, 0.15f), SND_OK);  // clamped to 0.1
  SND_CHECK_EQ(f.Submit({}, 0.0999f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  SND_CHECK_EQ(f.Submit({}, 0.0001f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
}

// P31: one call reporting a key several times uses the largest relative speed; the key
// is unordered in objects and materials; kinds are separate keys.
SND_TEST(IndepTrack, SameCallDuplicatesAndKey) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f, 0.0f, 0, 1), Slide(2, 1, 3.0f, 5.0f, 1, 0), Slide(1, 2, 2.0f, 9.0f, 0, 1),
                         Slide(1, 2, 2.0f, 0.0f, 0, 1, SND_CONTACT_ROLL)}),
               SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 2u);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_START)) {
    if (r.r.contact_kind == SND_CONTACT_SLIDE) {
      SND_CHECK_EQ(r.r.speed, 3.0f);
      SND_CHECK_EQ(r.r.position.x, 5.0f);
    }
  }
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.slide_reports, 2u);
  SND_CHECK_EQ(st.contacts_received, 4u);
  SND_CHECK_EQ(st.tracking_started, 2u);
}

// P33: table full -> evict the oldest last report (then the oldest start); entries
// reported in the same call are not evicted (undecided 8: the new one is dropped).
SND_TEST(IndepTrack, EvictionOrder) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 2;
  c.tracking_timeout_seconds = 10.0f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);                     // A starts t1
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f), Slide(3, 4, 1.0f)}), SND_OK);  // A, B reported t2
  SND_CHECK_EQ(f.Submit({Slide(5, 6, 1.0f)}), SND_OK);                     // C: tie on last report -> A (older start)
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.object_a, 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.stop_reason, SND_PHYSICS_STOP_EVICTED);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK_EQ(f.Submit({Slide(5, 6, 1.0f)}), SND_OK);  // C newer than B
  SND_CHECK_EQ(f.Submit({Slide(7, 8, 1.0f)}), SND_OK);  // D evicts B (oldest last report)
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.object_a, 3u);
  // Undecided 8: three keys in one call with room for two (C and D are both reported):
  // E is dropped.
  SND_CHECK_EQ(f.Submit({Slide(5, 6, 1.0f), Slide(7, 8, 1.0f), Slide(9, 10, 1.0f)}), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 0u);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_evictions, 2u);
  SND_CHECK_EQ(st.tracking_dropped, 1u);
}

// P34: a playback that ended by itself is restarted by the next report.
SND_TEST(IndepTrack, RestartAfterEnd) {
  snd_physics_config c = Cfg(nullptr);
  c.tracking_timeout_seconds = 10.0f;
  Fx f(c, kShort);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  const snd_playback_handle first = f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback;
  f.s.Advance(20);  // 256 frames of audio: long ended
  SND_REQUIRE(Gone(f.s.sys, first));
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  const Rec r = f.rec.Of(SND_PHYSICS_RECORD_START)[0];
  SND_CHECK(r.r.flags & SND_PHYSICS_RECORD_FLAG_RESTART);
  SND_CHECK_NE(r.r.playback.value, first.value);
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_restarted, 1u);
}

// P35: destroy stops the tracked playbacks (fade) and returns every emitter.
SND_TEST(IndepTrack, DestroyStopsAndReturnsEmitters) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 8;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 8; ++i) cs.push_back(Slide(i * 2, i * 2 + 1, 1.0f));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  std::vector<snd_playback_handle> pbs;
  std::vector<snd_emitter_handle> ems;  // all distinct and non-zero
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_START)) {
    pbs.push_back(r.r.playback);
    ems.push_back(r.r.emitter);
  }
  SND_REQUIRE_EQ(pbs.size(), 8u);
  for (std::size_t i = 0; i < ems.size(); ++i) {
    SND_CHECK(ems[i].value != 0u);
    for (std::size_t j = 0; j < i; ++j) SND_CHECK_NE(ems[i].value, ems[j].value);
  }
  f.rec.Clear();
  snd_physics_destroy(f.phys);
  f.phys = nullptr;
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 8u);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_STOP)) SND_CHECK_EQ(r.r.stop_reason, SND_PHYSICS_STOP_DESTROYED);
  f.s.Advance(40);
  for (const snd_playback_handle h : pbs) SND_CHECK(Gone(f.s.sys, h));
  // All emitter slots are free again (max_emitters = 128 in the rig).
  std::vector<snd_emitter_handle> mine;
  for (int i = 0; i < 128; ++i) mine.push_back(snd_create_emitter(f.s.sys));
  std::uint32_t ok = 0;
  for (const snd_emitter_handle e : mine) ok += e.value != 0u ? 1u : 0u;
  SND_CHECK_EQ(ok, 128u);
  for (const snd_emitter_handle e : mine) snd_destroy_emitter(f.s.sys, e);
}

// P15: min_slide_speed; invalid speeds; Slide / Roll ignore the budget.
SND_TEST(IndepTrack, SpeedThresholdInvalidAndNoBudget) {
  snd_physics_config c = Cfg(nullptr);
  c.min_slide_speed = 0.5f;
  c.budget_per_second = 0.0f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1, 2, std::nextafter(0.5f, 0.0f)), Slide(3, 4, 0.5f), Slide(5, 6, kNaN),
                         Slide(7, 8, -1.0f), Slide(9, 10, kInf)}),
               SND_OK);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.dropped_threshold, 1u);
  SND_CHECK_EQ(st.dropped_invalid, 3u);
  SND_CHECK_EQ(st.tracking_started, 1u);
  SND_CHECK_EQ(st.dropped_budget, 0u);
}

// P26, P27: speed curves on start / update; a replaced table keeps the running event and
// uses the new curves (undecided 8).
SND_TEST(IndepTrack, SpeedCurvesAndTableReplace) {
  Fx f;
  SND_REQUIRE(f.Ok());
  snd_physics_curves cv{};
  cv.speed_to_volume = phystest::Linear({{0.0f, 0.0f}, {4.0f, 1.0f}});
  cv.speed_to_pitch = phystest::Linear({{0.0f, 0.5f}, {4.0f, 1.5f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &cv), SND_OK);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.volume, 0.25f, 1e-6);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.pitch, 0.75f, 1e-6);
  const snd_playback_handle h = f.rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback;
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 3.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_UPDATE), 1u);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0].r.volume, 0.75f, 1e-6);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0].r.pitch, 1.25f, 1e-6);
  // Replace the table: material 0 slide now kShort, new curves.
  std::vector<snd_id> single(8, SND_ID_NONE);
  single[0 * 4 + SND_CONTACT_SLIDE] = kShort;
  snd_physics_curves cv2[2] = {};
  cv2[0].speed_to_volume = phystest::Linear({{0.0f, 0.5f}});
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 2;
  d.single = single.data();
  d.curves = cv2;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(f.Submit({Slide(1, 2, 3.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_UPDATE), 1u);
  const Rec u = f.rec.Of(SND_PHYSICS_RECORD_UPDATE)[0];
  SND_CHECK_EQ(u.r.playback.value, h.value);
  SND_CHECK_EQ(u.r.event, kLoop);
  SND_CHECK_NEAR(u.r.volume, 0.5f, 1e-6);
  SND_CHECK_EQ(snd_get_playback_state(f.s.sys, h), SND_PLAYBACK_STATE_PLAYING);
}

// P33 / P35 under load: many keys over many frames never exceed the table and every
// emitter comes back (random, fixed seed).
SND_TEST(IndepTrack, ChurnReturnsEverything) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 6;
  c.tracking_timeout_seconds = 0.1f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  Rng rng(42);
  std::uint64_t starts = 0, stops = 0;
  for (int frame = 0; frame < 200; ++frame) {
    std::vector<snd_contact> cs;
    const std::uint32_t n = rng.Below(9);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint64_t a = rng.Below(12);
      cs.push_back(Slide(a, a + 100, 0.5f + rng.Unit(), rng.Unit(), 0, 0,
                         rng.Below(2) ? SND_CONTACT_SLIDE : SND_CONTACT_ROLL));
    }
    SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
    starts += f.rec.Count(SND_PHYSICS_RECORD_START);
    stops += f.rec.Count(SND_PHYSICS_RECORD_STOP);
    SND_CHECK(starts - stops <= 6u);
  }
  f.rec.Clear();
  snd_physics_destroy(f.phys);
  f.phys = nullptr;
  stops += f.rec.Count(SND_PHYSICS_RECORD_STOP);
  SND_CHECK_EQ(starts, stops);
  f.s.Advance(40);
  std::vector<snd_emitter_handle> mine;
  for (int i = 0; i < 128; ++i) mine.push_back(snd_create_emitter(f.s.sys));
  std::uint32_t ok = 0;
  for (const snd_emitter_handle e : mine) ok += e.value != 0u ? 1u : 0u;
  SND_CHECK_EQ(ok, 128u);
  snd_stats st{};
  snd_get_stats(f.s.sys, &st);
  SND_CHECK_EQ(st.playbacks, 0u);
}
