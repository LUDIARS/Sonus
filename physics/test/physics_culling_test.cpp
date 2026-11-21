// 9.4 culling, step by step, counted exactly: threshold (equal passes, invalid values),
// grid grouping (cell boundaries, representative, count), cooldown (boundary, recorded only
// when requested), budget (dt in whole microseconds, only the fraction carried, Break first,
// impulse order), work-area overflow, 1000 contacts a frame without allocating, a 4x burst.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "phys_rig.hpp"
#include "snd/test/rt_guard.hpp"

using namespace phystest;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// Enough emitters and playbacks for the tests that never let the system run (the held
// emitters are released only once the system is seen to have applied their plays).
snd_config RoomyConfig() {
  snd_config c = SmallConfig();
  c.max_emitters = 2048;
  c.max_playbacks = 2048;
  return c;
}

struct Fixture {
  explicit Fixture(snd_physics_config cfg, std::uint32_t materials = 2) : sys(RoomyConfig()) {
    SND_REQUIRE_EQ(sys.created, SND_OK);
    SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, nullptr, &phys), SND_OK);
    snd_physics_table_desc d = SinglesTable(materials, &single);
    SND_REQUIRE_EQ(snd_physics_set_table(phys, &d), SND_OK);
  }
  ~Fixture() { snd_phys_destroy(phys); }
  snd_result Submit(const std::vector<snd_contact>& c, float dt) {
    return snd_phys_submit_contacts(phys, c.empty() ? nullptr : c.data(), static_cast<std::uint32_t>(c.size()), dt);
  }
  Sys sys;
  snd_phys* phys = nullptr;
  std::vector<snd_id> single;
};

}  // namespace

SND_TEST(PhysicsCulling, ThresholdEqualPassesAndInvalidValuesAreDropped) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.impulse_threshold = 1.0f;
  Fixture f(cfg);
  std::vector<snd_contact> c;
  c.push_back(Impact(1, 2, 0.0f, 0.999f));  // below
  c.push_back(Impact(3, 4, 10.0f, 1.0f));   // equal: passes
  c.push_back(Impact(5, 6, 20.0f, 1.5f));
  c.push_back(Impact(7, 8, 30.0f, kNan));
  c.push_back(Impact(9, 10, 40.0f, -1.0f));
  c.push_back(Impact(11, 12, 50.0f, kInf));
  c.push_back(Contact(SND_CONTACT_BREAK, 13, 14, 0, 0, 60.0f, 0, 0, 0.0f));  // no threshold for Break
  snd_contact bad = Impact(15, 16, 70.0f, 5.0f);
  bad.kind = 7;  // unknown kind
  c.push_back(bad);
  snd_contact nanPos = Impact(17, 18, kNan, 5.0f);
  c.push_back(nanPos);
  snd_contact negSpeed = Contact(SND_CONTACT_SLIDE, 19, 20, 0, 0, 80.0f, 0, 0, 0.0f, -1.0f);
  c.push_back(negSpeed);
  SND_CHECK_EQ(f.Submit(c, 1.0f / 60.0f), SND_OK);
  const snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.contacts_received, 10u);
  SND_CHECK_EQ(s.dropped_threshold, 1u);
  SND_CHECK_EQ(s.dropped_invalid, 6u);
  SND_CHECK_EQ(s.groups, 3u);
  SND_CHECK_EQ(s.group_requests, 3u);
  SND_CHECK_EQ(s.requested_impact, 2u);
  SND_CHECK_EQ(s.requested_break, 1u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 3u);
  // Accounting identity of physics.h.
  SND_CHECK_EQ(s.contacts_received, s.dropped_invalid + s.dropped_threshold + s.dropped_work_overflow + s.merged +
                                        s.groups + s.slide_reports);

  // The table's threshold applies when the configured one is 0.
  Recorder rec2;
  Fixture g(OpenConfig(&rec2));
  snd_physics_table_desc d = SinglesTable(2, &g.single);
  d.impulse_threshold = 2.0f;
  SND_REQUIRE_EQ(snd_physics_set_table(g.phys, &d), SND_OK);
  SND_CHECK_EQ(g.Submit({Impact(1, 2, 0.0f, 1.999f), Impact(3, 4, 10.0f, 2.0f)}, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(g.phys).dropped_threshold, 1u);
  SND_CHECK_EQ(Stats(g.phys).requested_impact, 1u);
  // Slide / Roll: the relative-speed floor (equal passes).
  snd_physics_config sc = OpenConfig(nullptr);
  sc.min_slide_speed = 0.5f;
  Fixture h(sc);
  SND_CHECK_EQ(h.Submit({Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, 0, 0, 0, 0, 0.4999f),
                         Contact(SND_CONTACT_ROLL, 3, 4, 0, 0, 0, 0, 0, 0, 0.5f)},
                        0.01f),
               SND_OK);
  SND_CHECK_EQ(Stats(h.phys).dropped_threshold, 1u);
  SND_CHECK_EQ(Stats(h.phys).tracking_started, 1u);
}

SND_TEST(PhysicsCulling, GridCellsAndRepresentative) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));  // grid 0.5
  std::vector<snd_contact> c = {
      Impact(1, 2, 0.49f, 1.0f),     // cell 0
      Impact(1, 2, 0.5f, 2.0f),      // cell 1 (exactly 1 * 0.5)
      Impact(1, 2, 0.75f, 3.0f),     // cell 1
      Impact(1, 2, 0.999f, 4.0f),    // cell 1: representative (largest)
      Impact(1, 2, 1.0f, 5.0f),      // cell 2
      Impact(1, 2, -0.0001f, 6.0f),  // cell -1
      Impact(1, 2, -0.5f, 7.0f),     // cell -1
      Impact(1, 2, -0.5001f, 8.0f),  // cell -2
  };
  SND_CHECK_EQ(f.Submit(c, 0.01f), SND_OK);
  const snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.groups, 5u);
  SND_CHECK_EQ(s.merged, 3u);
  const std::vector<Rec> plays = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(plays.size(), 5u);
  // Ordered by impulse: 8, 7, 5, 4, 1.
  const float wantImpulse[5] = {8.0f, 7.0f, 5.0f, 4.0f, 1.0f};
  const float wantX[5] = {-0.5001f, -0.5f, 1.0f, 0.999f, 0.49f};
  const std::uint32_t wantCount[5] = {1, 2, 1, 3, 1};
  const snd_id countId = P(SND_PHYSICS_PARAM_COUNT_NAME);
  const snd_id impulseId = P(SND_PHYSICS_PARAM_IMPULSE_NAME);
  for (std::size_t i = 0; i < 5; ++i) {
    SND_CHECK_EQ(plays[i].r.impulse, wantImpulse[i]);
    SND_CHECK_EQ(plays[i].r.position.x, wantX[i]);
    SND_CHECK_EQ(plays[i].r.count, wantCount[i]);
    SND_CHECK_EQ(plays[i].Param(countId), static_cast<float>(wantCount[i]));
    SND_CHECK_EQ(plays[i].Param(impulseId), wantImpulse[i]);
  }
  // y and z cells, equal impulses (the first in the array represents), material order and kinds.
  rec.Clear();
  snd_physics_reset_stats(f.phys);
  std::vector<snd_contact> d = {
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 1, 0, 0.5f, 0, 2.0f),    // A
      Contact(SND_CONTACT_IMPACT, 3, 4, 1, 0, 0, 0.7f, 0, 2.0f),    // same cell, (B, A) order: merges into A
      Contact(SND_CONTACT_IMPACT, 5, 6, 0, 1, 0, 0.49f, 0, 2.0f),   // y cell 0: another group
      Contact(SND_CONTACT_IMPACT, 7, 8, 0, 1, 0, 0.5f, 0.5f, 2.0f), // z cell 1: another group
      Contact(SND_CONTACT_BREAK, 9, 10, 0, 1, 0, 0.5f, 0, 2.0f),    // Break: another group
      Contact(SND_CONTACT_IMPACT, 11, 12, 0, 0, 0, 0.5f, 0, 2.0f),  // another material pair
  };
  SND_CHECK_EQ(f.Submit(d, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(f.phys).groups, 5u);
  SND_CHECK_EQ(Stats(f.phys).merged, 1u);
  bool sawMerged = false;
  for (const Rec& r : rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    if (r.r.count == 2u) {
      sawMerged = true;
      SND_CHECK_EQ(r.r.object_a, 1u);  // the first of equal impulses
      SND_CHECK_EQ(r.r.position.y, 0.5f);
    }
  }
  SND_CHECK(sawMerged);
}

SND_TEST(PhysicsCulling, CooldownBoundaryInMicroseconds) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.cooldown_seconds = 0.05f;  // 50000 us
  Fixture f(cfg);
  std::vector<std::size_t> playedAt;
  for (int call = 1; call <= 11; ++call) {
    const std::size_t before = rec.Count(SND_PHYSICS_RECORD_PLAY);
    // Objects in either order are the same key.
    const snd_contact c = (call % 2) ? Impact(1, 2, 0.0f, 1.0f) : Impact(2, 1, 0.0f, 1.0f);
    SND_CHECK_EQ(f.Submit({c}, 0.01f), SND_OK);  // 10000 us a call
    if (rec.Count(SND_PHYSICS_RECORD_PLAY) > before) playedAt.push_back(static_cast<std::size_t>(call));
  }
  // Played at 10 ms, then at 60 ms (exactly 50 ms later), then at 110 ms.
  SND_REQUIRE_EQ(playedAt.size(), 3u);
  SND_CHECK_EQ(playedAt[0], 1u);
  SND_CHECK_EQ(playedAt[1], 6u);
  SND_CHECK_EQ(playedAt[2], 11u);
  SND_CHECK_EQ(Stats(f.phys).dropped_cooldown, 8u);
  // Another material pair or another kind is another key.
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0.0f, 1.0f, 0, 1), Contact(SND_CONTACT_BREAK, 1, 2, 0, 0, 5, 0, 0, 1.0f),
                         Impact(1, 2, 5.0f, 1.0f)},
                        0.01f),
               SND_OK);
  SND_CHECK_EQ(Stats(f.phys).dropped_cooldown, 9u);
  SND_CHECK_EQ(Stats(f.phys).group_requests, 5u);
}

SND_TEST(PhysicsCulling, CooldownIsRecordedOnlyForRequestedGroups) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.cooldown_seconds = 1.0f;
  cfg.budget_per_second = 60.0f;  // 60 * 16667 us = 1.00002: one a frame
  Fixture f(cfg);
  const std::vector<snd_contact> c = {Impact(1, 2, 0.0f, 2.0f), Impact(3, 4, 10.0f, 1.0f)};
  SND_CHECK_EQ(f.Submit(c, 1.0f / 60.0f), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.recs.back().r.object_a, 1u);
  SND_CHECK_EQ(Stats(f.phys).dropped_budget, 1u);
  SND_CHECK_EQ(f.Submit(c, 1.0f / 60.0f), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 2u);
  SND_CHECK_EQ(rec.recs.back().r.object_a, 3u);  // not on cooldown: it was not requested before
  SND_CHECK_EQ(Stats(f.phys).dropped_cooldown, 1u);
  // Two cells of the same objects in one call: one request (the cooldown key is the objects').
  snd_physics_config c2 = OpenConfig(&rec);
  c2.cooldown_seconds = 0.5f;
  Fixture g(c2);
  rec.Clear();
  SND_CHECK_EQ(g.Submit({Impact(7, 8, 0.0f, 1.0f), Impact(7, 8, 10.0f, 3.0f)}, 0.01f), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.recs.back().r.impulse, 3.0f);
  SND_CHECK_EQ(Stats(g.phys).dropped_cooldown, 1u);
}

SND_TEST(PhysicsCulling, CooldownTableEvictsTheOldest) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.cooldown_seconds = 1.0f;
  cfg.max_cooldowns = 2;
  Fixture f(cfg);
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);   // t = 10 ms
  SND_CHECK_EQ(f.Submit({Impact(3, 4, 0.0f, 1.0f)}, 0.01f), SND_OK);   // t = 20 ms
  SND_CHECK_EQ(f.Submit({Impact(5, 6, 0.0f, 1.0f)}, 0.01f), SND_OK);   // full: (1, 2) is pushed out
  SND_CHECK_EQ(Stats(f.phys).cooldown_evictions, 1u);
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0.0f, 1.0f), Impact(5, 6, 10.0f, 1.0f)}, 0.01f), SND_OK);
  // (1, 2) plays again (forgotten; this pushes out (3, 4)), (5, 6) is still on cooldown.
  SND_CHECK_EQ(Stats(f.phys).group_requests, 4u);
  SND_CHECK_EQ(Stats(f.phys).dropped_cooldown, 1u);
  SND_CHECK_EQ(Stats(f.phys).cooldown_evictions, 2u);
}

SND_TEST(PhysicsCulling, CooldownEvictionTiesGoInInsertionOrder) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.cooldown_seconds = 1.0f;
  cfg.max_cooldowns = 2;
  Fixture f(cfg);
  // Three keys requested in one call (impulses 3, 2, 1 in that order): the third pushes out the
  // first recorded, (1, 2), and takes its slot 0.
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0.0f, 3.0f), Impact(3, 4, 10.0f, 2.0f), Impact(5, 6, 20.0f, 1.0f)}, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(f.phys).cooldown_evictions, 1u);
  // (1, 2) again: the table holds (5, 6) in slot 0 and (3, 4) in slot 1, both at 10 ms; the
  // first recorded of them, (3, 4), goes (not the lower slot).
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0.0f, 3.0f)}, 0.01f), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Impact(3, 4, 10.0f, 2.0f), Impact(5, 6, 20.0f, 1.0f)}, 0.01f), SND_OK);
  const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.object_a, 3u);
  SND_CHECK_EQ(Stats(f.phys).dropped_cooldown, 1u);
}

SND_TEST(PhysicsCulling, BudgetFromDtInMicroseconds) {
  auto perFrame = [](float budget, float dt, const std::vector<std::uint32_t>& contactsPerFrame, float maxDt = 0.1f) {
    Recorder rec;
    snd_physics_config cfg = OpenConfig(&rec);
    cfg.budget_per_second = budget;
    cfg.max_dt_seconds = maxDt;
    Fixture f(cfg);
    std::vector<std::uint32_t> got;
    std::uint64_t obj = 1;
    for (std::uint32_t n : contactsPerFrame) {
      std::vector<snd_contact> c;
      for (std::uint32_t i = 0; i < n; ++i, obj += 2) c.push_back(Impact(obj, obj + 1, static_cast<float>(i), 1.0f));
      // Groups that passed the budget: played, or dropped for want of a held-emitter place
      // (the system never runs here, so the held ring fills).
      const std::size_t before = rec.Count(SND_PHYSICS_RECORD_PLAY) + rec.Count(SND_PHYSICS_RECORD_DROP);
      f.Submit(c, dt);
      got.push_back(static_cast<std::uint32_t>(rec.Count(SND_PHYSICS_RECORD_PLAY) + rec.Count(SND_PHYSICS_RECORD_DROP) - before));
    }
    return got;
  };
  // 300 / s at 1/60 s (16667 us): exactly 5 a frame for 60 frames.
  const std::vector<std::uint32_t> a = perFrame(300.0f, 1.0f / 60.0f, std::vector<std::uint32_t>(60, 20));
  std::uint32_t total = 0;
  bool allFive = true;
  for (std::uint32_t n : a) {
    total += n;
    allFive = allFive && n == 5u;
  }
  SND_CHECK(allFive);
  SND_CHECK_EQ(total, 300u);
  // 12500 us: 3.75 a frame -> 3, 4, 4, 4 (the fraction carries).
  SND_CHECK(perFrame(300.0f, 0.0125f, {20, 20, 20, 20, 20}) == (std::vector<std::uint32_t>{3, 4, 4, 4, 3}));
  // An unused whole part is not carried: 3 unused in frame 1, then 4 (3.75 + 0.75).
  SND_CHECK(perFrame(300.0f, 0.0125f, {0, 20}) == (std::vector<std::uint32_t>{0, 4}));
  // dt is clamped to max_dt (0.1 s -> 30), and dt 0 gives no budget.
  SND_CHECK(perFrame(300.0f, 1.0f, {50}) == (std::vector<std::uint32_t>{30}));
  SND_CHECK(perFrame(300.0f, 0.0f, {10, 10}) == (std::vector<std::uint32_t>{0, 0}));
  SND_CHECK(perFrame(300.0f, 1.0f, {50}, 0.05f) == (std::vector<std::uint32_t>{15}));
  // Budget 0: nothing.
  SND_CHECK(perFrame(0.0f, 0.1f, {10}) == (std::vector<std::uint32_t>{0}));
}

SND_TEST(PhysicsCulling, BudgetOrderBreakFirstThenImpulse) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.budget_per_second = 200.0f;  // 200 * 0.01 = 2 a call
  Fixture f(cfg);
  std::vector<snd_contact> c = {
      Impact(1, 2, 0.0f, 10.0f),
      Contact(SND_CONTACT_BREAK, 3, 4, 0, 0, 10.0f, 0, 0, 1.0f),
      Impact(5, 6, 20.0f, 5.0f),
      Contact(SND_CONTACT_BREAK, 7, 8, 0, 0, 30.0f, 0, 0, 3.0f),
  };
  SND_CHECK_EQ(f.Submit(c, 0.01f), SND_OK);
  const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.contact_kind, SND_CONTACT_BREAK);
  SND_CHECK_EQ(p[0].r.impulse, 3.0f);
  SND_CHECK_EQ(p[1].r.contact_kind, SND_CONTACT_BREAK);
  SND_CHECK_EQ(p[1].r.impulse, 1.0f);
  SND_CHECK_EQ(p[0].r.event, Ev(0, SND_CONTACT_BREAK));
  SND_CHECK_EQ(Stats(f.phys).dropped_budget, 2u);
  // Next call: the two Impacts, larger first; equal impulses go in array order.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Impact(9, 10, 0.0f, 5.0f), Impact(11, 12, 10.0f, 5.0f), Impact(13, 14, 20.0f, 9.0f)}, 0.01f),
               SND_OK);
  const std::vector<Rec> q = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(q.size(), 2u);
  SND_CHECK_EQ(q[0].r.object_a, 13u);
  SND_CHECK_EQ(q[1].r.object_a, 9u);
}

SND_TEST(PhysicsCulling, WorkAreaOverflowIsCounted) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.max_groups_per_call = 4;
  Fixture f(cfg);
  std::vector<snd_contact> c;
  for (int i = 0; i < 6; ++i) c.push_back(Impact(1, 2, static_cast<float>(i), 1.0f));
  c.push_back(Impact(1, 2, 0.1f, 2.0f));  // merges into cell 0 (still room for merges)
  c.push_back(Impact(1, 2, 5.1f, 2.0f));  // cell 10: its group could not be made
  SND_CHECK_EQ(f.Submit(c, 0.01f), SND_OK);
  const snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.groups, 4u);
  SND_CHECK_EQ(s.merged, 1u);
  SND_CHECK_EQ(s.dropped_work_overflow, 3u);
  SND_CHECK_EQ(s.group_requests, 4u);
}

SND_TEST(PhysicsCulling, ThousandContactsAFrameWithoutAllocating) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  CountingAllocator alloc;
  const snd_allocator at = alloc.Table();
  snd_physics_config cfg;
  snd_physics_config_init(&cfg);  // the defaults: 300 / s, 2048 groups
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, &at, &phys), SND_OK);
  SND_CHECK_EQ(alloc.allocs, 1u);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(4, &single);
  std::vector<snd_contact> c;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t o = static_cast<std::uint64_t>(i) * 2u + 1u;
    // 40 sliding pairs (fewer than the 64 tracked pairs) and 960 impacts.
    snd_contact x = Contact(i % 25 == 0 ? SND_CONTACT_SLIDE : SND_CONTACT_IMPACT, o, o + 1u,
                            static_cast<std::uint16_t>(i % 4), static_cast<std::uint16_t>((i / 4) % 4),
                            static_cast<float>(i % 10), static_cast<float>((i / 10) % 10), static_cast<float>(i / 100),
                            1.0f + static_cast<float>(i % 7), 1.0f);
    c.push_back(x);
  }
  std::uint64_t requested = 0;
  {
    snd::test::rt::ExpectViolations ev;  // the sonus C API may lock; allocations are counted below
    snd::rt::RtScope scope;
    SND_CHECK_EQ(snd_physics_set_table(phys, &d), SND_OK);
    for (int frame = 0; frame < 30; ++frame) {
      SND_CHECK_EQ(snd_phys_submit_contacts(phys, c.data(), static_cast<std::uint32_t>(c.size()), 1.0f / 60.0f), SND_OK);
      snd_surface_weight w[2] = {{0, 0, 0.7f}, {1, 0, 0.3f}};
      SND_CHECK_EQ(snd_phys_play_footstep(phys, snd_emitter_handle{0}, 0, w, 2, SND_MOTION_RUN), SND_OK);
      {
        snd::rt::SuspendScope out;  // the system's own frame work is not physics'
        sys.Advance(1);
      }
    }
    if (snd::rt::kEnabled) {
      SND_CHECK_EQ(snd::test::rt::Count(snd::rt::Violation::Alloc), 0u);
      SND_CHECK_EQ(snd::test::rt::Count(snd::rt::Violation::Free), 0u);
    }
  }
  SND_CHECK_EQ(alloc.allocs, 1u);
  const snd_physics_stats s = Stats(phys);
  SND_CHECK_EQ(s.dropped_work_overflow, 0u);
  SND_CHECK_EQ(s.contacts_received, 30000u);
  SND_CHECK_EQ(s.group_requests, 30u * 5u);  // 300 / s at 60 frames / s
  SND_CHECK_EQ(s.groups, s.dropped_no_event + s.dropped_cooldown + s.dropped_budget + s.group_requests);
  SND_CHECK_EQ(s.contacts_received,
               s.dropped_invalid + s.dropped_threshold + s.dropped_work_overflow + s.merged + s.groups + s.slide_reports);
  requested = s.requested_impact;
  SND_CHECK(requested >= 150u);
  snd_phys_destroy(phys);
  SND_CHECK_EQ(alloc.frees, 1u);
}

SND_TEST(PhysicsCulling, FourTimesTheVoicesAndEmittersInOneFrame) {
  snd_config sc = SmallConfig();
  sc.max_emitters = 32;
  sc.max_playbacks = 512;
  Sys sys(sc);
  SND_REQUIRE_EQ(sys.created, SND_OK);
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, nullptr, &phys), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(phys, &d), SND_OK);
  // 4 x max_real_voices (32) requests in one frame; only 32 emitters: the other 96 groups are
  // dropped (no snd_play_at fallback: it would sound at the listener).
  std::vector<snd_contact> c;
  for (int i = 0; i < 128; ++i) c.push_back(Impact(static_cast<std::uint64_t>(i) * 2u + 1u, 0, static_cast<float>(i), 1.0f));
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, c.data(), 128, 0.05f), SND_OK);
  snd_physics_stats s = Stats(phys);
  SND_CHECK_EQ(s.requested_impact, 32u);
  SND_CHECK_EQ(s.group_requests, 32u);
  SND_CHECK_EQ(s.dropped_no_emitter, 96u);
  SND_CHECK_EQ(s.groups, s.dropped_no_event + s.dropped_cooldown + s.dropped_budget + s.dropped_no_emitter + s.group_requests);
  SND_CHECK_EQ(s.api_failures, 0u);
  for (const Rec& r : rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    SND_CHECK(r.r.playback.value != 0u);
    SND_CHECK(r.r.emitter.value != 0u);
  }
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 32u);
  const std::vector<Rec> drops = rec.Of(SND_PHYSICS_RECORD_DROP);
  SND_REQUIRE_EQ(drops.size(), 96u);
  SND_CHECK_EQ(drops[0].r.event, Ev(0, SND_CONTACT_IMPACT));
  SND_CHECK_EQ(drops[0].r.result, SND_ERR_LIMIT_REACHED);
  // Once the system has applied the plays, the next call destroys the held emitters; the slots
  // come back with the system's updates and the next requests get emitters again.
  sys.Advance(8);
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, nullptr, 0, 0.0f), SND_OK);
  sys.Advance(8);
  rec.Clear();
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, c.data(), 16, 0.05f), SND_OK);
  s = Stats(phys);
  SND_CHECK_EQ(s.dropped_no_emitter, 96u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 16u);
  sys.Advance(4);
  snd_phys_destroy(phys);
}
