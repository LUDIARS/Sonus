// Independent tests: time and culling (9.4, decision D). Properties P11..P21 of
// INDEP_PROPERTIES.md. Expected counts are derived from the rules, not from the
// implementation. Events need not exist: physics only records commands, and the PLAY
// records of the observer say what was requested.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

// A physics object on a fresh system with `n` materials that all have single events.
struct Fx {
  explicit Fx(snd_physics_config c, std::uint32_t n = 4) {
    if (c.observer == nullptr) {
      c.observer = &Recorder::Fn;
      c.observer_user = &rec;
    }
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    single.assign(std::size_t{n} * 4u, SND_ID_NONE);
    for (std::uint32_t m = 0; m < n; ++m) {
      for (std::uint32_t k = 0; k < 4; ++k) single[m * 4u + k] = SEv(m, k);
    }
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = n;
    d.single = single.data();
    if (phys != nullptr) tableResult = snd_physics_set_table(phys, &d);
  }
  ~Fx() { snd_physics_destroy(phys); }
  snd_result Submit(const std::vector<snd_contact>& cs, float dt) {
    const snd_result r =
        snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
    if (pump) s.Advance(1);  // a game frame: publish the commands, run one device period
    return r;
  }
  bool pump = true;
  std::size_t Plays() const { return rec.Count(SND_PHYSICS_RECORD_PLAY); }
  snd_physics_stats St() const { return phystest::Stats(phys); }
  Sys s;
  Recorder rec;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
  std::vector<snd_id> single;
};

snd_contact Imp(std::uint64_t a, std::uint64_t b, float x, float impulse, std::uint16_t ma = 0, std::uint16_t mb = 0) {
  return Contact(SND_CONTACT_IMPACT, a, b, ma, mb, x, 0.0f, 0.0f, impulse);
}
snd_contact Brk(std::uint64_t a, std::uint64_t b, float x, float impulse, std::uint16_t ma = 0, std::uint16_t mb = 0) {
  return Contact(SND_CONTACT_BREAK, a, b, ma, mb, x, 0.0f, 0.0f, impulse);
}

snd_physics_config Open() {
  snd_physics_config c;
  snd_physics_config_init(&c);
  c.budget_per_second = 1.0e6f;
  c.cooldown_seconds = 0.0f;
  return c;
}

}  // namespace

// P14: Impact threshold (< drops, = passes), invalid impulses, Break exempt.
SND_TEST(IndepCull, ThresholdExactAndInvalid) {
  snd_physics_config c = Open();
  c.impulse_threshold = 1.0f;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(f.tableResult, SND_OK);
  std::vector<snd_contact> cs = {
      Imp(1, 2, 0.0f, std::nextafter(1.0f, 0.0f)),  // below: threshold
      Imp(3, 4, 1.0f, 1.0f),                        // exactly: passes
      Imp(5, 6, 2.0f, 1.5f),                        // passes
      Imp(7, 8, 3.0f, kNaN),                        // invalid
      Imp(9, 10, 4.0f, -2.0f),                      // invalid (not threshold)
      Imp(11, 12, 5.0f, kInf),                      // invalid
      Brk(13, 14, 6.0f, 0.0f),                      // Break: no threshold
      Brk(15, 16, 7.0f, 0.5f),                      // Break: no threshold
  };
  SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.contacts_received, 8u);
  SND_CHECK_EQ(st.dropped_threshold, 1u);
  SND_CHECK_EQ(st.dropped_invalid, 3u);
  SND_CHECK_EQ(st.groups, 4u);
  SND_CHECK_EQ(st.group_requests, 4u);
  SND_CHECK_EQ(st.requested_impact, 2u);
  SND_CHECK_EQ(st.requested_break, 2u);
  SND_CHECK_EQ(f.Plays(), 4u);
}

// P14: config threshold > 0 overrides the table's; 0 = the table's.
SND_TEST(IndepCull, ThresholdFromTableUnlessConfigured) {
  for (int configured = 0; configured < 2; ++configured) {
    snd_physics_config c = Open();
    c.impulse_threshold = configured ? 1.0f : 0.0f;
    Fx f(c, 1);
    SND_REQUIRE_EQ(f.created, SND_OK);
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 1;
    d.single = f.single.data();
    d.impulse_threshold = 2.0f;
    SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
    SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.5f), Imp(3, 4, 5.0f, 2.0f), Imp(5, 6, 9.0f, 0.5f)}, 0.01f), SND_OK);
    const snd_physics_stats st = f.St();
    if (configured) {
      SND_CHECK_EQ(st.dropped_threshold, 1u);  // 0.5 < 1
      SND_CHECK_EQ(st.group_requests, 2u);
    } else {
      SND_CHECK_EQ(st.dropped_threshold, 2u);  // 0.5, 1.5 < 2
      SND_CHECK_EQ(st.group_requests, 1u);
    }
  }
}

// P16: cell = floor(p / g) per axis; exactly k*g is cell k, negatives go down.
SND_TEST(IndepCull, GridBoundaries) {
  snd_physics_config c = Open();
  c.grid_size = 0.25f;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  // Same objects and materials so only the cell separates groups.
  std::vector<snd_contact> cs = {
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.0f, 0, 0, 1.0f),     // cell 0
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, -0.0f, 0, 0, 1.0f),    // cell 0 (floor(-0) = -0)
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.2499f, 0, 0, 1.0f),  // cell 0
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.25f, 0, 0, 1.0f),    // cell 1 (exactly 1 * g)
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.4999f, 0, 0, 1.0f),  // cell 1
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, -0.01f, 0, 0, 1.0f),   // cell -1
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, -0.25f, 0, 0, 1.0f),   // cell -1 (exactly -1 * g)
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, -0.2501f, 0, 0, 1.0f), // cell -2
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.0f, 0.25f, 0, 1.0f), // y cell 1
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0.0f, 0, -0.25f, 1.0f),// z cell -1
  };
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.groups, 6u);
  SND_CHECK_EQ(st.merged, 4u);
  // The cooldown key does not contain the cell: 6 groups of the same objects in one call.
  // Cooldown 0 here, so all of them play.
  SND_CHECK_EQ(st.group_requests, 6u);
  std::vector<std::uint32_t> counts;
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) counts.push_back(r.r.count);
  std::uint32_t sum = 0;
  for (std::uint32_t n : counts) sum += n;
  SND_CHECK_EQ(sum, 10u);
}

// P16: huge and non-finite coordinates (UBSan: no float->int overflow), invalid when non-finite.
SND_TEST(IndepCull, HugeAndNonFinitePositions) {
  snd_physics_config c = Open();
  c.grid_size = 1.0e-6f;  // makes p / g overflow int64 for big p
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  const float big = 3.0e38f;
  std::vector<snd_contact> cs = {
      Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, big, 0, 0, 1.0f),
      Contact(SND_CONTACT_IMPACT, 3, 4, 0, 0, -big, 0, 0, 1.0f),
      Contact(SND_CONTACT_IMPACT, 5, 6, 0, 0, 0, 1.0e30f, -1.0e30f, 1.0f),
      Contact(SND_CONTACT_IMPACT, 7, 8, 0, 0, kNaN, 0, 0, 1.0f),
      Contact(SND_CONTACT_IMPACT, 9, 10, 0, 0, 0, kInf, 0, 1.0f),
      Contact(SND_CONTACT_BREAK, 11, 12, 0, 0, 0, 0, -kInf, 1.0f),
  };
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.dropped_invalid, 3u);
  SND_CHECK_EQ(st.groups, 3u);
  SND_CHECK_EQ(st.group_requests, 3u);
}

// P16: representative = largest impulse, first on ties; Count; materials unordered;
// Impact and Break never merge; position / objects are the representative's.
SND_TEST(IndepCull, GroupRepresentativeAndCount) {
  Fx f(Open());
  SND_REQUIRE_EQ(f.created, SND_OK);
  std::vector<snd_contact> cs = {
      Contact(SND_CONTACT_IMPACT, 1, 2, 1, 2, 0.10f, 0, 0, 2.0f),
      Contact(SND_CONTACT_IMPACT, 3, 4, 2, 1, 0.20f, 0, 0, 5.0f),  // representative (first 5)
      Contact(SND_CONTACT_IMPACT, 5, 6, 1, 2, 0.30f, 0, 0, 5.0f),  // tie, later
      Contact(SND_CONTACT_BREAK, 7, 8, 1, 2, 0.15f, 0, 0, 9.0f),   // other kind: own group
      Contact(SND_CONTACT_IMPACT, 9, 9, 1, 1, 0.12f, 0, 0, 1.0f),  // other material pair
  };
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.groups, 3u);
  SND_CHECK_EQ(st.merged, 2u);
  bool found = false;
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    if (r.r.contact_kind != SND_CONTACT_IMPACT || r.r.count != 3u) continue;
    found = true;
    SND_CHECK_EQ(r.r.object_a, 3u);
    SND_CHECK_EQ(r.r.object_b, 4u);
    SND_CHECK_EQ(r.r.impulse, 5.0f);
    SND_CHECK_EQ(r.r.position.x, 0.20f);
    SND_CHECK_EQ(r.Param(P(SND_PHYSICS_PARAM_COUNT_NAME)), 3.0f);
    SND_CHECK_EQ(r.Param(P(SND_PHYSICS_PARAM_IMPULSE_NAME)), 5.0f);
  }
  SND_CHECK(found);
  // Materials 1 and 2 both have single events: 2 plays for the merged group, 2 for the
  // Break group, 1 for (1, 1) (same material once).
  SND_CHECK_EQ(f.Plays(), 5u);
}

// P17: cooldown in whole us, ">=" passes, unordered keys, kinds separate.
SND_TEST(IndepCull, CooldownBoundaryAndKey) {
  snd_physics_config c = Open();
  c.cooldown_seconds = 0.05f;  // 50000 us
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  // dt 0.01 (10000 us): plays at frames 0, 5, 10, ... (elapsed exactly 50000 passes).
  for (int i = 0; i < 11; ++i) {
    SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);
  }
  SND_CHECK_EQ(f.Plays(), 3u);
  SND_CHECK_EQ(f.St().dropped_cooldown, 8u);
  f.rec.Clear();
  // Swapped objects and materials: the same key (still in cooldown after frame 10).
  SND_CHECK_EQ(f.Submit({Imp(2, 1, 3.0f, 1.0f)}, 0.01f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 0u);
  // Other kind, other material pair, other objects: separate keys.
  SND_CHECK_EQ(f.Submit({Brk(1, 2, 0.0f, 1.0f), Imp(1, 2, 6.0f, 1.0f, 0, 1), Imp(1, 3, 9.0f, 1.0f)}, 0.01f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u + 2u + 1u);
}

// P12: time is whole microseconds: 3 x round(1/60 s) = 50001 us passes a 0.05 s
// cooldown, 3 x 16666 us = 49998 does not.
SND_TEST(IndepCull, MicrosecondClock) {
  for (int variant = 0; variant < 2; ++variant) {
    snd_physics_config c = Open();
    c.cooldown_seconds = 0.05f;
    Fx f(c);
    SND_REQUIRE_EQ(f.created, SND_OK);
    const float dt = variant == 0 ? 1.0f / 60.0f : 0.016666f;
    SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, dt), SND_OK);
    for (int i = 0; i < 3; ++i) SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, dt), SND_OK);
    SND_CHECK_EQ(f.Plays(), variant == 0 ? 2u : 1u);
  }
  // dt below half a microsecond never advances the clock, however many calls. (The last
  // call of each part uses dt = 1 us: dt = 0 would give no budget slot.)
  snd_physics_config c = Open();
  c.cooldown_seconds = 0.0001f;  // 100 us
  c.budget_per_second = 1.0e7f;  // 10 slots per us
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);
  f.pump = false;
  for (int i = 0; i < 1000; ++i) SND_CHECK_EQ(f.Submit({}, 4.0e-7f), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 1.0e-6f), SND_OK);  // elapsed 1 us (not 401)
  SND_CHECK_EQ(f.Plays(), 1u);
  // 0.6 us rounds to 1 us: 1 + 97 + 1 = 99 us -> still cooling; one more -> 100 us passes
  // (a clock in float seconds would be at 0.4 * 1000 + ... > 400 us long before).
  for (int i = 0; i < 97; ++i) SND_CHECK_EQ(f.Submit({}, 6.0e-7f), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 1.0e-6f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 1u);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 6.0e-7f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 2u);
}

// P11: a rejected dt changes nothing: no time, no budget, counted in calls_rejected.
SND_TEST(IndepCull, RejectedDtDoesNothing) {
  snd_physics_config c = Open();
  c.cooldown_seconds = 0.05f;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);
  const float bad[] = {-0.01f, -0.0f * 0.0f - 1e-30f, kNaN, kInf, -kInf};
  for (float dt : bad) {
    SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f), Imp(5, 6, 0.0f, 1.0f)}, dt), SND_ERR_INVALID_ARGUMENT);
  }
  snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.calls_rejected, 5u);
  SND_CHECK_EQ(st.contacts_received, 1u);  // the rejected contacts are not received
  SND_CHECK_EQ(f.Plays(), 1u);
  // 0.049999 s later: in cooldown unless the rejected calls added time; 1 us more passes.
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.049999f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 1u);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.000001f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 2u);
  // -0.0 is not negative: accepted.
  SND_CHECK_EQ(f.Submit({}, -0.0f), SND_OK);
  // count > 0 with NULL contacts is refused; count 0 with NULL is fine.
  SND_CHECK_EQ(snd_phys_submit_contacts(f.phys, nullptr, 3, 0.01f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_submit_contacts(f.phys, nullptr, 0, 0.01f), SND_OK);
}

// P18: budget floor(carry + bps * dt), only the fraction carried.
SND_TEST(IndepCull, BudgetFractionCarry) {
  snd_physics_config c = Open();
  c.budget_per_second = 12.0f;  // x 0.0625 s = 0.75 per frame -> 0, 1, 1, 1, 0, 1, 1, 1
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  std::vector<std::size_t> per;
  std::uint64_t obj = 100;
  for (int i = 0; i < 8; ++i) {
    std::vector<snd_contact> cs;
    for (int j = 0; j < 10; ++j, obj += 2) cs.push_back(Imp(obj, obj + 1, static_cast<float>(j), 1.0f));
    const std::size_t before = f.Plays();
    SND_CHECK_EQ(f.Submit(cs, 0.0625f), SND_OK);
    per.push_back(f.Plays() - before);
  }
  const std::size_t want[8] = {0, 1, 1, 1, 0, 1, 1, 1};
  for (int i = 0; i < 8; ++i) SND_CHECK_EQ(per[static_cast<std::size_t>(i)], want[i]);
  SND_CHECK_EQ(f.St().dropped_budget, 80u - 6u);
}

// P18: 300 / s at 1/60 s is exactly 5 every frame (600 frames); an unused whole part is
// not carried; dt = 0 adds no slot; dt is clamped to max_dt.
SND_TEST(IndepCull, BudgetPerFrameAndClamp) {
  Fx f(Cfg(nullptr));
  SND_REQUIRE_EQ(f.created, SND_OK);
  std::uint64_t obj = 1;
  auto frame = [&](int n, float dt) {
    std::vector<snd_contact> cs;
    for (int j = 0; j < n; ++j, obj += 2) cs.push_back(Imp(obj, obj + 1, static_cast<float>(j), 1.0f));
    const std::size_t before = f.Plays();
    f.Submit(cs, dt);
    return f.Plays() - before;
  };
  bool all5 = true;
  for (int i = 0; i < 600; ++i) all5 = all5 && frame(20, 1.0f / 60.0f) == 5u;
  SND_CHECK(all5);
  SND_CHECK_EQ(frame(2, 1.0f / 60.0f), 2u);
  SND_CHECK_EQ(frame(20, 1.0f / 60.0f), 5u);  // not 8: the 3 unused are not carried
  SND_CHECK_EQ(frame(20, 0.0f), 0u);
  SND_CHECK_EQ(frame(100, 1.0f), 30u);        // clamped to 0.1 s
  SND_CHECK_EQ(frame(100, 0.1f), 30u);
}

// P18: Break first, then impulse descending, array order on ties.
SND_TEST(IndepCull, BudgetOrder) {
  snd_physics_config c = Open();
  c.budget_per_second = 30.0f;  // 0.1 s -> 3 slots
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  std::vector<snd_contact> cs = {
      Imp(1, 2, 0.0f, 100.0f), Imp(3, 4, 1.0f, 7.0f), Brk(5, 6, 2.0f, 0.5f),
      Imp(7, 8, 3.0f, 7.0f),   Imp(9, 10, 4.0f, 7.0f),
  };
  SND_CHECK_EQ(f.Submit(cs, 0.1f), SND_OK);
  std::vector<std::uint64_t> played;
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) played.push_back(r.r.object_a);
  SND_REQUIRE_EQ(played.size(), 3u);
  // Selected: Break(5), Impact 100 (1), first Impact 7 (3).
  std::vector<std::uint64_t> sorted = played;
  std::sort(sorted.begin(), sorted.end());
  SND_CHECK_EQ(sorted[0], 1u);
  SND_CHECK_EQ(sorted[1], 3u);
  SND_CHECK_EQ(sorted[2], 5u);
  SND_CHECK_EQ(f.St().dropped_budget, 2u);
}

// P17: a group dropped by the budget does not start a cooldown.
SND_TEST(IndepCull, BudgetDropDoesNotRecordCooldown) {
  snd_physics_config c = Open();
  c.budget_per_second = 10.0f;  // 0.1 s -> 1 slot
  c.cooldown_seconds = 1.0f;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 9.0f), Imp(3, 4, 5.0f, 1.0f)}, 0.1f), SND_OK);
  SND_CHECK_EQ(f.Plays(), 1u);
  f.rec.Clear();
  SND_CHECK_EQ(f.Submit({Imp(3, 4, 5.0f, 1.0f)}, 0.1f), SND_OK);  // not in cooldown
  SND_CHECK_EQ(f.Plays(), 1u);
  SND_CHECK_EQ(f.St().dropped_cooldown, 0u);
}

// P17: cooldown table full -> the oldest record is evicted (that key may play again).
SND_TEST(IndepCull, CooldownTableEvictsOldest) {
  snd_physics_config c = Open();
  c.cooldown_seconds = 10.0f;
  c.max_cooldowns = 2;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);  // t1: key A
  SND_CHECK_EQ(f.Submit({Imp(3, 4, 0.0f, 1.0f)}, 0.01f), SND_OK);  // t2: key B
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f), Imp(3, 4, 5.0f, 1.0f)}, 0.01f), SND_OK);  // both cooling
  SND_CHECK_EQ(f.Plays(), 2u);
  SND_CHECK_EQ(f.Submit({Imp(5, 6, 0.0f, 1.0f)}, 0.01f), SND_OK);  // key C evicts A (oldest)
  SND_CHECK_EQ(f.St().cooldown_evictions, 1u);
  SND_CHECK_EQ(f.Submit({Imp(3, 4, 0.0f, 1.0f)}, 0.01f), SND_OK);  // B still cooling
  SND_CHECK_EQ(f.Plays(), 3u);
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 1.0f)}, 0.01f), SND_OK);  // A forgotten: plays (evicts B)
  SND_CHECK_EQ(f.Plays(), 4u);
  SND_CHECK_EQ(f.Submit({Imp(3, 4, 0.0f, 1.0f)}, 0.01f), SND_OK);  // B forgotten: plays
  SND_CHECK_EQ(f.Plays(), 5u);
}

// P19: work area overflow; 1000 contacts at the default capacity do not overflow.
SND_TEST(IndepCull, WorkAreaOverflowAndThousand) {
  {
    snd_physics_config c = Open();
    c.max_groups_per_call = 4;
    Fx f(c);
    SND_REQUIRE_EQ(f.created, SND_OK);
    std::vector<snd_contact> cs;
    for (int j = 0; j < 6; ++j) cs.push_back(Imp(1, 2, static_cast<float>(j), 1.0f));
    SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
    const snd_physics_stats st = f.St();
    SND_CHECK_EQ(st.groups, 4u);
    SND_CHECK_EQ(st.dropped_work_overflow, 2u);
    // Exactly at capacity: no overflow.
    cs.resize(4);
    SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
    SND_CHECK_EQ(f.St().dropped_work_overflow, 2u);
  }
  Fx f(Cfg(nullptr));
  SND_REQUIRE_EQ(f.created, SND_OK);
  std::vector<snd_contact> cs;
  for (int j = 0; j < 1000; ++j) {
    cs.push_back(Contact(SND_CONTACT_IMPACT, static_cast<std::uint64_t>(j), 100000u + static_cast<std::uint64_t>(j),
                         static_cast<std::uint16_t>(j % 4), 0, static_cast<float>(j), 0, 0, 1.0f + static_cast<float>(j)));
  }
  SND_CHECK_EQ(f.Submit(cs, 1.0f / 60.0f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.contacts_received, 1000u);
  SND_CHECK_EQ(st.dropped_work_overflow, 0u);
  SND_CHECK_EQ(st.groups, 1000u);
  SND_CHECK_EQ(st.group_requests, 5u);
  SND_CHECK_EQ(st.dropped_budget, 995u);
  // The 5 requested are the 5 largest impulses.
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) SND_CHECK(r.r.impulse >= 996.0f);
}

// P20: a group that resolves to nothing consumes neither budget nor cooldown.
SND_TEST(IndepCull, NoEventGroupConsumesNothing) {
  snd_physics_config c = Open();
  c.budget_per_second = 10.0f;  // 1 slot per 0.1 s
  Fx f(c, 2);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(snd_physics_set_single_event(f.phys, 1, SND_PHYSICS_ALL_KINDS, SND_ID_NONE), SND_OK);
  // Material 1 (no event) with the larger impulse, material 0 smaller: 0 must get the slot.
  SND_CHECK_EQ(f.Submit({Imp(1, 2, 0.0f, 50.0f, 1, 1), Imp(3, 4, 5.0f, 1.0f, 0, 0)}, 0.1f), SND_OK);
  const snd_physics_stats st = f.St();
  SND_CHECK_EQ(st.dropped_no_event, 1u);
  SND_CHECK_EQ(st.dropped_budget, 0u);
  SND_CHECK_EQ(st.group_requests, 1u);
}

// P21: reference model vs implementation on random contact streams (fixed seeds).
SND_TEST(IndepCull, ReferenceModelRandom) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    Rng rng(seed);
    snd_physics_config c;
    snd_physics_config_init(&c);
    c.grid_size = seed % 2 ? 0.5f : 0.25f;
    c.cooldown_seconds = seed % 3 == 0 ? 0.0f : 0.03f;
    c.budget_per_second = seed % 2 ? 300.0f : 77.0f;
    c.impulse_threshold = seed % 3 == 1 ? 0.5f : 0.0f;
    c.max_cooldowns = seed % 2 ? 8u : 256u;
    Fx f(c, 5);
    SND_REQUIRE_EQ(f.created, SND_OK);
    // Material 4 has no events.
    SND_REQUIRE_EQ(snd_physics_set_single_event(f.phys, 4, SND_PHYSICS_ALL_KINDS, SND_ID_NONE), SND_OK);
    CullModel m;
    m.threshold = c.impulse_threshold;
    m.grid = c.grid_size;
    m.cooldownUs = CullModel::Us(c.cooldown_seconds);
    m.bps = c.budget_per_second;
    m.cooldownCap = c.max_cooldowns;
    m.hasSingle = {true, true, true, true, false};
    const float dts[] = {1.0f / 60.0f, 1.0f / 30.0f, 0.0123f, 0.0f, 0.25f, 0.004f};
    bool same = true;
    for (int frame = 0; frame < 300 && same; ++frame) {
      std::vector<snd_contact> cs;
      const std::uint32_t n = rng.Below(30);
      for (std::uint32_t i = 0; i < n; ++i) {
        snd_contact x = Contact(rng.Below(8) == 0 ? SND_CONTACT_BREAK : SND_CONTACT_IMPACT, rng.Below(6), rng.Below(6),
                                static_cast<std::uint16_t>(rng.Below(5)), static_cast<std::uint16_t>(rng.Below(5)),
                                rng.Unit() * 2.0f - 1.0f, static_cast<float>(rng.Below(2)) * 0.5f, 0.0f,
                                static_cast<float>(rng.Below(8)) * 0.25f);
        if (rng.Below(40) == 0) x.impulse = kNaN;
        if (rng.Below(40) == 0) x.impulse = -1.0f;
        if (rng.Below(60) == 0) x.position[1] = kInf;
        cs.push_back(x);
      }
      const float dt = dts[rng.Below(6)];
      std::vector<ModelPlay> want;
      m.Submit(cs, dt, &want);
      f.rec.Clear();
      SND_CHECK_EQ(f.Submit(cs, dt), SND_OK);
      std::vector<Rec> got = f.rec.Of(SND_PHYSICS_RECORD_PLAY);
      if (got.size() != want.size()) {
        snd::test::Note("seed %llu frame %d: %llu plays, model %llu", static_cast<unsigned long long>(seed), frame,
                        static_cast<unsigned long long>(got.size()), static_cast<unsigned long long>(want.size()));
        same = false;
        break;
      }
      for (std::size_t i = 0; i < want.size(); ++i) {
        const snd_physics_record& r = got[i].r;
        const ModelPlay& w = want[i];
        if (r.event != w.event || r.count != w.count || r.impulse != w.impulse || r.object_a != w.objA ||
            r.object_b != w.objB || r.position.x != w.pos[0] || r.contact_kind != w.kind) {
          snd::test::Note("seed %llu frame %d play %llu: event %x count %u impulse %g obj %llu/%llu x %g; model %x %u %g "
                          "%llu/%llu %g",
                          static_cast<unsigned long long>(seed), frame, static_cast<unsigned long long>(i), r.event, r.count,
                          static_cast<double>(r.impulse), static_cast<unsigned long long>(r.object_a),
                          static_cast<unsigned long long>(r.object_b), static_cast<double>(r.position.x), w.event,
                          w.count, static_cast<double>(w.impulse), static_cast<unsigned long long>(w.objA),
                          static_cast<unsigned long long>(w.objB), static_cast<double>(w.pos[0]));
          same = false;
          break;
        }
      }
    }
    SND_CHECK(same);
    const snd_physics_stats st = f.St();
    SND_CHECK_EQ(st.contacts_received, m.st.received);
    SND_CHECK_EQ(st.dropped_invalid, m.st.invalid);
    SND_CHECK_EQ(st.dropped_threshold, m.st.threshold);
    SND_CHECK_EQ(st.merged, m.st.merged);
    SND_CHECK_EQ(st.groups, m.st.groups);
    SND_CHECK_EQ(st.dropped_no_event, m.st.noEvent);
    SND_CHECK_EQ(st.dropped_cooldown, m.st.cooldown);
    SND_CHECK_EQ(st.dropped_budget, m.st.budget);
    SND_CHECK_EQ(st.group_requests, m.st.requests);
    SND_CHECK_EQ(st.requested_impact, m.st.reqImpact);
    SND_CHECK_EQ(st.requested_break, m.st.reqBreak);
    SND_CHECK_EQ(st.cooldown_evictions, m.st.evictions);
    // P41: accounting identities.
    SND_CHECK_EQ(st.contacts_received, st.dropped_invalid + st.dropped_threshold + st.dropped_work_overflow + st.merged +
                                           st.groups + st.slide_reports);
    SND_CHECK_EQ(st.groups, st.dropped_no_event + st.dropped_cooldown + st.dropped_budget + st.group_requests);
  }
}
