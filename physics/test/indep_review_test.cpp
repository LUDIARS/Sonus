// Independent tests added after reading physics/src/physics.cpp (step 3 of the independent
// test procedure): paths the first round did not reach. Each test says which rule it
// checks and whether it pins an undecided choice of the implementer.
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kLoop = snd_hash_id("I/Loop");
const snd_id kDc2D = snd_hash_id("I/Dc2D");

struct Fx {
  explicit Fx(snd_physics_config c = Cfg(nullptr), snd_config sc = phystest::SmallConfig()) : s(sc) {
    loaded = s.LoadBank("phys.sndb", TestBank(), &bank);
    SetListener(s.sys);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys == nullptr) return;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 2;
    single.assign(8, SND_ID_NONE);
    single[0 * 4 + SND_CONTACT_SLIDE] = kLoop;
    single[0 * 4 + SND_CONTACT_IMPACT] = snd_hash_id("I/Dc");
    single[1 * 4 + SND_CONTACT_IMPACT] = kDc2D;
    d.single = single.data();
    d.footstep_default = kDc2D;
    tableResult = snd_physics_set_table(phys, &d);
  }
  ~Fx() { snd_physics_destroy(phys); }
  bool Ok() const { return loaded && created == SND_OK && tableResult == SND_OK; }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 0.05f) {
    rec.Clear();
    const snd_result r =
        snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
    s.Advance(1);
    return r;
  }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Slide(std::uint64_t a, float speed = 1.0f) {
  return Contact(SND_CONTACT_SLIDE, a, a + 1000u, 0, 0, 0.0f, 0.0f, 1.0f, 0.0f, speed);
}

}  // namespace

// Decision D (9.5): an entry not reported for >= timeout is stopped with reason TIMEOUT;
// eviction is only for a full table. physics.cpp handles the new reports (TrackReports)
// before the timeouts (Timeouts), so a table full of timed-out entries evicts one
// (EVICTED, tracking_evictions) instead of timing it out first.
SND_TEST(IndepReview, Defect_TimedOutEntryIsEvictedInsteadOfTimedOut) {
  snd_physics_config c = Cfg(nullptr);
  c.max_tracked_pairs = 1;
  c.tracking_timeout_seconds = 0.1f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1)}), SND_OK);  // A at t = 50 ms
  // 100 ms later (exactly the timeout) B is reported: A has timed out in this call.
  SND_CHECK_EQ(f.Submit({Slide(2)}, 0.1f), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  const Rec stop = f.rec.Of(SND_PHYSICS_RECORD_STOP)[0];
  SND_CHECK_EQ(stop.r.object_a, 1u);
  SND_CHECK_MSG(stop.r.stop_reason == SND_PHYSICS_STOP_TIMEOUT, "stop reason %u (want TIMEOUT)", stop.r.stop_reason);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.tracking_timeouts, 1u);
  SND_CHECK_EQ(st.tracking_evictions, 0u);
  SND_CHECK_EQ(st.tracking_started, 2u);
}

// Defect (9.2 / decision D): the volume is weight / (w1 + w2). Two finite, valid weights
// whose sum overflows float give 0 instead of 0.5 (sw.weight / sum with sum = inf).
SND_TEST(IndepReview, Defect_FootstepHugeWeightsOverflow) {
  Fx f;
  SND_REQUIRE(f.Ok());
  snd_surface_weight w[2] = {{0, 0, 3.0e38f}, {1, 0, 3.0e38f}};
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 0, w, 2, SND_MOTION_RUN), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 2u);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    SND_CHECK_NEAR(r.r.volume, 0.5f, 1e-6);
    SND_CHECK_NEAR(r.Param(P(SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME)), 0.5f, 1e-6);
  }
}

// Approved (decision 6 of the review): the foot material parameter is the foot's SOUND
// material and is omitted when the foot's material is unknown.
SND_TEST(IndepReview, FootMaterialParameterIsTheSoundMaterialOrNothing) {
  Fx f;
  SND_REQUIRE(f.Ok());
  snd_physics_material_entry e[3] = {};
  e[0].sound_material = 1;
  e[1].sound_material = 0;
  e[2].sound_material = SND_PHYSICS_UNMAPPED;
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 3), SND_OK);
  const snd_surface_weight w{0, 0, 1.0f};
  const snd_id footParam = P(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME);
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 1, &w, 1, SND_MOTION_WALK), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(footParam), 0.0f);  // game 1 -> sound 0
  f.rec.Clear();
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 2, &w, 1, SND_MOTION_WALK), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK(!f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Has(footParam));  // unknown foot: not passed
}

// Decision D: "foot の Emitter で鳴らす（無効なら 2D）". A foot emitter that has become
// stale (destroyed by the game) must still give a (2D) footstep.
SND_TEST(IndepReview, StaleFootEmitterPlaysIn2D) {
  Fx f;
  SND_REQUIRE(f.Ok());
  const snd_emitter_handle foot = snd_create_emitter(f.s.sys);
  SND_REQUIRE(foot.value != 0u);
  SND_CHECK_EQ(snd_destroy_emitter(f.s.sys, foot), SND_OK);
  f.s.Advance(4);
  const snd_surface_weight w{0, 0, 1.0f};
  const snd_result r = snd_phys_play_footstep(f.phys, foot, 0, &w, 1, SND_MOTION_WALK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const Rec p = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  snd::test::Note("stale foot: result %d, playback %llu, record result %d", static_cast<int>(r),
                  static_cast<unsigned long long>(p.r.playback.value), static_cast<int>(p.r.result));
  SND_CHECK(p.r.playback.value != 0u);
  SND_CHECK_EQ(r, SND_OK);
}

// Decision 3 of the review: snd_physics_destroy on another thread does nothing (no stop, no
// free); the object stays usable and is destroyed later on its own thread.
SND_TEST(IndepReview, DestroyOnAnotherThreadDoesNothing) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  phystest::CountingAllocator a;
  const snd_allocator at = a.Table();
  Recorder rec;
  snd_physics_config c = Cfg(&rec);
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, &at, &p), SND_OK);
  std::vector<snd_id> single(4, SND_ID_NONE);
  single[SND_CONTACT_SLIDE] = kLoop;
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 1;
  d.single = single.data();
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  const snd_contact x = Slide(1);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 0.05f), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_START), 1u);
  const snd_playback_handle h = rec.Of(SND_PHYSICS_RECORD_START)[0].r.playback;
  std::thread t1([p] { snd_physics_destroy(p); });
  t1.join();
  std::thread t2([p] { snd_phys_destroy(p); });
  t2.join();
  SND_CHECK_EQ(a.frees, 0u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  s.Advance(20);
  SND_CHECK_EQ(snd_get_playback_state(s.sys, h), SND_PLAYBACK_STATE_PLAYING);
  // Still usable on its thread.
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 0.05f), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(p), 1u);
  rec.Clear();
  snd_physics_destroy(p);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(a.frees, a.allocs);
  s.Advance(60);
  const snd_playback_state st = snd_get_playback_state(s.sys, h);
  SND_CHECK(st == SND_PLAYBACK_STATE_ENDED || st == SND_PLAYBACK_STATE_NONE);
}

// Decision (release when applied): while the game is paused (dt 0, count 0 every frame) and
// the audio side runs, the held one-shot emitters come back; the game is not starved.
// (The old implementation held them for 0.1 s of physics time, so with dt 0 they were never
// returned: with max_emitters 8 and 8 hits, the game's snd_create_emitter got 0.)
SND_TEST(IndepReview, HeldEmittersReturnWhilePaused) {
  snd_config sc = phystest::SmallConfig();
  sc.max_emitters = 8;
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  c.budget_per_second = 1.0e6f;
  Fx f(c, sc);
  SND_REQUIRE(f.Ok());
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 8; ++i) {
    cs.push_back(Contact(SND_CONTACT_IMPACT, i * 2, i * 2 + 1, 0, 0, static_cast<float>(i), 0, 0, 5.0f));
  }
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 8u);
  for (int i = 0; i < 10; ++i) SND_CHECK_EQ(f.Submit({}, 0.0f), SND_OK);  // paused, audio runs
  std::vector<snd_emitter_handle> mine;
  for (int i = 0; i < 8; ++i) mine.push_back(snd_create_emitter(f.s.sys));
  std::uint32_t ok = 0;
  for (const snd_emitter_handle e : mine) ok += e.value != 0u ? 1u : 0u;
  SND_CHECK_EQ(ok, 8u);
  for (const snd_emitter_handle e : mine) snd_destroy_emitter(f.s.sys, e);
  SND_CHECK_EQ(phystest::Stats(f.phys).dropped_no_emitter, 0u);
}

// Same call: Impact groups first, then slide reports, then timeouts. A slide report and an
// impact of the same objects in one call are independent (different tables).
SND_TEST(IndepReview, ImpactAndSlideOfTheSameObjectsInOneCall) {
  Fx f;
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1), Contact(SND_CONTACT_IMPACT, 1, 1001, 0, 0, 0, 0, 1, 5.0f)}), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_START), 1u);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  // Records: the PLAY of the impact comes before the START of the slide.
  std::size_t play = 99, start = 99;
  for (std::size_t i = 0; i < f.rec.recs.size(); ++i) {
    if (f.rec.recs[i].r.kind == SND_PHYSICS_RECORD_PLAY) play = i;
    if (f.rec.recs[i].r.kind == SND_PHYSICS_RECORD_START) start = i;
  }
  SND_CHECK(play < start);
}

// Decision 4 of the review: reports to tracked keys do not use the Impact / Break work area;
// a burst of impacts that overflows it does not push a running slide out.
SND_TEST(IndepReview, ImpactBurstDoesNotCrowdOutSlideReports) {
  snd_physics_config c = Cfg(nullptr);
  c.max_groups_per_call = 4;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  SND_CHECK_EQ(f.Submit({Slide(1)}), SND_OK);
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 6; ++i) {
    cs.push_back(Contact(SND_CONTACT_IMPACT, 50 + i, 60 + i, 0, 0, static_cast<float>(i), 0, 0, 5.0f));
  }
  cs.push_back(Slide(1));
  SND_CHECK_EQ(f.Submit(cs), SND_OK);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.dropped_work_overflow, 2u);  // the 5th and 6th impact groups
  SND_CHECK_EQ(st.groups, 4u);
  SND_CHECK_EQ(st.tracking_updated, 1u);
}

// Approved (decision 6 of the review): grouping is by SOUND material; the representative's
// game materials (and their surface parameters) are used.
SND_TEST(IndepReview, GroupingBySoundMaterial) {
  Fx f;
  SND_REQUIRE(f.Ok());
  snd_physics_material_entry e[2] = {};
  e[0].sound_material = 1;
  e[1].sound_material = 1;
  e[1].param_count = 1;
  e[1].params[0] = snd_physics_param{0x77u, 1.0f};  // wet
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 2), SND_OK);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 1, 9.0f),
                         Contact(SND_CONTACT_IMPACT, 3, 4, 1, 1, 0.1f, 0, 1, 2.0f)}),
               SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const Rec r = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  SND_CHECK_EQ(r.r.count, 2u);
  SND_CHECK(!r.Has(0x77u));  // the wet contact was merged into the dry representative
}

// 14 "負荷・バースト": 4 times the real voices requested every frame for a second, with real
// events. Nothing crashes, the accounting holds, failures of the sonus API are counted
// (not lost), and every emitter comes back after destroy.
SND_TEST(IndepReview, FourTimesTheVoicesEveryFrame) {
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  c.budget_per_second = 1.0e6f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  f.rec.recs.reserve(20000);
  std::uint64_t obj = 0;
  for (int frame = 0; frame < 60; ++frame) {
    std::vector<snd_contact> cs;
    for (int i = 0; i < 4 * 32; ++i, obj += 2) {
      cs.push_back(Contact(SND_CONTACT_IMPACT, obj, obj + 1, static_cast<std::uint16_t>(i % 2), static_cast<std::uint16_t>(i % 2),
                           static_cast<float>(i), 0, 1, 5.0f));
    }
    SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
  }
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.group_requests + st.dropped_no_emitter, 60u * 128u);
  SND_CHECK_EQ(st.requested_impact, st.group_requests);
  SND_CHECK_EQ(st.contacts_received, st.groups);
  SND_CHECK_EQ(st.groups, st.dropped_no_event + st.dropped_cooldown + st.dropped_budget + st.dropped_no_emitter +
                              st.group_requests);
  snd::test::Note("4x: dropped_no_emitter %llu, group_requests %llu, api_failures %llu",
                  static_cast<unsigned long long>(st.dropped_no_emitter),
                  static_cast<unsigned long long>(st.group_requests),
                  static_cast<unsigned long long>(st.api_failures));
  snd_physics_destroy(f.phys);
  f.phys = nullptr;
  f.s.Advance(100);
  std::vector<snd_emitter_handle> mine;
  for (int i = 0; i < 128; ++i) mine.push_back(snd_create_emitter(f.s.sys));
  std::uint32_t ok = 0;
  for (const snd_emitter_handle e : mine) ok += e.value != 0u ? 1u : 0u;
  SND_CHECK_EQ(ok, 128u);
}
