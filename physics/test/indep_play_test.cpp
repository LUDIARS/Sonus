// Independent tests: real playback through the C API (SND_OUTPUT_MANUAL), positioned
// one-shots, the emitter hold of the implementer (undecided 2), emitter exhaustion, and
// the core behaviour behind request A-004 reproduced with the public API only.
// Properties P35..P38.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kTable = snd_hash_id("I/Table");
const snd_id kDc = snd_hash_id("I/Dc");

// Mean L / R of the frames captured during the last `periods` device periods.
struct Lr {
  double l, r;
};
Lr Measure(Sys& s, std::uint32_t settle, std::uint32_t periods) {
  s.Advance(settle);
  const std::uint64_t from = s.Frames();
  s.Advance(periods);
  const std::uint64_t n = s.Frames() - from;
  double l = 0, r = 0;
  for (std::uint64_t f = from; f < from + n; ++f) {
    l += s.L(f);
    r += s.R(f);
  }
  return Lr{l / static_cast<double>(n), r / static_cast<double>(n)};
}

struct Fx {
  explicit Fx(snd_physics_config c = Cfg(nullptr), snd_config sc = phystest::SmallConfig()) : s(sc) {
    loaded = s.LoadBank("phys.sndb", TestBank(), &bank);
    SetListener(s.sys);
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys != nullptr && loaded) tableResult = snd_physics_load_bank_table(phys, bank, kTable);
  }
  ~Fx() { snd_physics_destroy(phys); }
  bool Ok() const { return loaded && created == SND_OK && tableResult == SND_OK; }
  snd_result Submit(std::vector<snd_contact> cs, float dt = 1.0f / 60.0f) {
    rec.Clear();
    return snd_phys_submit_contacts(phys, cs.empty() ? nullptr : cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
  }
  Sys s;
  Recorder rec;
  snd_bank_handle bank{};
  bool loaded = false;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
};

snd_contact ImpactAt(std::uint64_t obj, float x, float impulse = 100.0f, std::uint16_t m = 0) {
  return Contact(SND_CONTACT_IMPACT, obj, obj + 1000u, m, m, x, 0.0f, 0.0f, impulse);
}

}  // namespace

// P38 (core, outside physics): record of the core behaviour of request A-004, public API
// only (spec/handoff/parallel/requests/A-004.md). sonus.h: "Playbacks on the emitter keep the
// last transform". An emitter created, moved, played on and destroyed before one snd_update:
//   (a) correct:      L ~ 0, R ~ 0.25 (2 m to the right, like play_at / no destroy)
//   (b) known defect: L ~ R ~ 0.3536 (at the listener, no attenuation)
// Anything else fails. While core shows (b), physics must hold one-shot emitters until the
// plays are applied; once core shows (a), that hold can be simplified (destroy at once).
// That physics' own hits sound at their position is checked by other tests
// (ImpactIsPositioned, Defect_HoldInPhysicsTimeSubstepsLosePosition, ...).
SND_TEST(IndepPlay, CoreA004_EmitterCreatedAndDestroyedInOneUpdate) {
  Lr res[4] = {};
  for (int variant = 0; variant < 4; ++variant) {
    Sys s;
    snd_bank_handle bank{};
    SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
    SetListener(s.sys);
    s.Advance(2);
    const snd_vec3 right{2.0f, 0.0f, 0.0f};
    if (variant == 3) {
      SND_CHECK_EQ(snd_play_at(s.sys, kDc, &right, nullptr), SND_OK);
    } else {
      const snd_emitter_handle e = snd_create_emitter(s.sys);
      SND_REQUIRE(e.value != 0u);
      SND_CHECK_EQ(snd_set_transform(s.sys, e, &right, nullptr, nullptr), SND_OK);
      if (variant == 2) s.Advance(1);  // the emitter lives through one block first
      const snd_playback_handle h = snd_play(s.sys, kDc, e, nullptr);
      SND_CHECK(h.value != 0u);
      if (variant >= 1) SND_CHECK_EQ(snd_destroy_emitter(s.sys, e), SND_OK);
    }
    res[variant] = Measure(s, 6, 8);
  }
  // The reference cases place the sound on the right.
  SND_CHECK(res[0].r > 0.2 && res[0].r < 0.3 && res[0].l < 0.02);
  SND_CHECK(res[2].r > 0.2 && res[2].r < 0.3 && res[2].l < 0.02);
  SND_CHECK(res[3].r > 0.2 && res[3].r < 0.3 && res[3].l < 0.02);
  const bool correct = res[1].l < 0.02 && std::fabs(res[1].r - res[0].r) < 0.02;
  const bool knownDefect = std::fabs(res[1].l - 0.3536) < 0.02 && std::fabs(res[1].r - 0.3536) < 0.02;
  snd::test::Note("core A-004: %s (L %.4f R %.4f)",
                  correct ? "(a) correct: the position is kept" : knownDefect ? "(b) known defect: at the listener" : "neither",
                  res[1].l, res[1].r);
  SND_CHECK(correct || knownDefect);
}

// P36: an Impact is heard at its position (right / left), through physics.
SND_TEST(IndepPlay, ImpactIsPositioned) {
  for (int side = 0; side < 2; ++side) {
    Fx f;
    SND_REQUIRE(f.Ok());
    f.s.Advance(2);
    SND_CHECK_EQ(f.Submit({ImpactAt(1, side == 0 ? 2.0f : -2.0f)}), SND_OK);
    SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
    SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.flags & SND_PHYSICS_RECORD_FLAG_POSITION_ONLY, 0u);
    const Lr m = Measure(f.s, 6, 8);
    snd::test::Note("side %d: L %.4f R %.4f", side, m.l, m.r);
    const double nearCh = side == 0 ? m.r : m.l, farCh = side == 0 ? m.l : m.r;
    SND_CHECK(nearCh > 0.05);
    SND_CHECK(farCh < 0.2 * nearCh);
  }
}

// Defect 2 of the first round (fixed by the decision "release when the plays are seen
// applied"; assertions unchanged): it was measured in physics time, not in snd_update calls. A fixed-step game that
// catches up after a hitch runs several sub-steps before one snd_update (here 7 x 1/60 s);
// the hold (0.1 s) passes inside the same command batch, the emitter is destroyed in the
// batch that plays on it -> the A-004 situation: the hit 2 m to the right is centred.
SND_TEST(IndepPlay, Defect_HoldInPhysicsTimeSubstepsLosePosition) {
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  SND_CHECK_EQ(f.Submit({ImpactAt(1, 2.0f)}), SND_OK);
  for (int i = 0; i < 6; ++i) SND_CHECK_EQ(f.Submit({}), SND_OK);  // catch-up sub-steps, no snd_update
  const Lr m = Measure(f.s, 6, 8);
  snd::test::Note("sub-stepped: L %.4f R %.4f", m.l, m.r);
  SND_CHECK_MSG(m.l < 0.02, "L %.4f: the impact 2 m to the right is heard on the left", m.l);
}

// P37 (decision: no position-less fallback): no emitter -> the group is dropped
// (dropped_no_emitter, a DROP record with result LIMIT_REACHED and the group's first event),
// its budget place is used, no cooldown is recorded, not counted as a request. The emitters
// come back once the plays are applied, and the dropped keys can play again at once.
SND_TEST(IndepPlay, EmitterExhaustionDropsTheGroup) {
  snd_config sc = phystest::SmallConfig();
  sc.max_emitters = 6;
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 10.0f;
  c.budget_per_second = 100.0f;  // 0.1 s -> 10 places
  Fx f(c, sc);
  SND_REQUIRE(f.Ok());
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 12; ++i) cs.push_back(ImpactAt(i * 2, static_cast<float>(i), 100.0f - static_cast<float>(i)));
  SND_CHECK_EQ(f.Submit(cs, 0.1f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 6u);
  const std::vector<Rec> drops = f.rec.Of(SND_PHYSICS_RECORD_DROP);
  SND_REQUIRE_EQ(drops.size(), 4u);
  for (const Rec& r : drops) {
    SND_CHECK_EQ(r.r.result, SND_ERR_LIMIT_REACHED);
    SND_CHECK_EQ(r.r.event, kDc);
    SND_CHECK_EQ(r.r.playback.value, 0u);
    SND_CHECK(r.r.object_a >= 12u);  // the 7th..10th largest impulses (objects 12..18)
  }
  snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.dropped_no_emitter, 4u);
  SND_CHECK_EQ(st.group_requests, 6u);
  SND_CHECK_EQ(st.requested_impact, 6u);
  SND_CHECK_EQ(st.dropped_budget, 2u);  // the places of the dropped groups were used
  SND_CHECK_EQ(st.groups, st.dropped_no_event + st.dropped_cooldown + st.dropped_budget + st.dropped_no_emitter +
                              st.group_requests);
  // Once applied, the emitters come back; a dropped key is not in cooldown.
  for (int i = 0; i < 4; ++i) {
    f.s.Advance(1);
    SND_CHECK_EQ(f.Submit({}, 0.0f), SND_OK);
  }
  f.s.Advance(1);
  SND_CHECK_EQ(f.Submit({ImpactAt(12, 6.0f)}, 0.1f), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_DROP), 0u);
  SND_CHECK_EQ(f.Submit({ImpactAt(0, 0.0f)}, 0.1f), SND_OK);  // played before: in cooldown
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 0u);
}

// The hold ring (decision): full of this call's emitters -> the new group is dropped; full
// with emitters of an earlier call that are not applied yet -> the oldest is destroyed early
// (held_emitters_released_early); applied ones are released normally (not early).
SND_TEST(IndepPlay, HoldRingFull) {
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  c.budget_per_second = 1.0e6f;
  c.max_held_emitters = 4;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 6; ++i) cs.push_back(ImpactAt(i * 2, static_cast<float>(i)));
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  SND_CHECK_EQ(phystest::Stats(f.phys).dropped_no_emitter, 2u);
  // No snd_update in between: the 4 held are not applied -> 2 released early.
  SND_CHECK_EQ(f.Submit({ImpactAt(100, 1.0f), ImpactAt(102, 3.0f)}, 0.01f), SND_OK);
  snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.held_emitters_released_early, 2u);
  SND_CHECK_EQ(st.dropped_no_emitter, 2u);
  // Applied now: the next call releases them normally.
  f.s.Advance(4);
  SND_CHECK_EQ(f.Submit({ImpactAt(104, 1.0f), ImpactAt(106, 3.0f), ImpactAt(108, 5.0f), ImpactAt(110, 7.0f)}, 0.01f), SND_OK);
  st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.held_emitters_released_early, 2u);
  SND_CHECK_EQ(st.dropped_no_emitter, 2u);
}

// P37 / P35: destroy releases held emitters at once (all slots usable afterwards).
SND_TEST(IndepPlay, DestroyReleasesHeldEmitters) {
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  c.budget_per_second = 1.0e6f;
  Fx f(c);
  SND_REQUIRE(f.Ok());
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 40; ++i) cs.push_back(ImpactAt(i * 2, static_cast<float>(i)));
  SND_CHECK_EQ(f.Submit(cs, 0.01f), SND_OK);
  snd_physics_destroy(f.phys);
  f.phys = nullptr;
  f.s.Advance(2);
  std::vector<snd_emitter_handle> mine;
  for (int i = 0; i < 128; ++i) mine.push_back(snd_create_emitter(f.s.sys));
  std::uint32_t ok = 0;
  for (const snd_emitter_handle e : mine) ok += e.value != 0u ? 1u : 0u;
  SND_CHECK_EQ(ok, 128u);
}

// P26 end to end: the playback volume reaches the output (2D event, single of m1).
SND_TEST(IndepPlay, VolumeCurveReachesOutput) {
  double level[2] = {0, 0};
  for (int v = 0; v < 2; ++v) {
    Fx f;
    SND_REQUIRE(f.Ok());
    snd_physics_curves cv{};
    cv.impulse_to_volume = phystest::Linear({{0.0f, v == 0 ? 1.0f : 0.5f}});
    SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 1, &cv), SND_OK);
    f.s.Advance(2);
    SND_CHECK_EQ(f.Submit({ImpactAt(1, 0.0f, 5.0f, 1)}), SND_OK);
    const Lr m = Measure(f.s, 6, 8);
    level[v] = m.l;
    snd::test::Note("volume %s: L %.4f R %.4f", v == 0 ? "1" : "0.5", m.l, m.r);
  }
  SND_CHECK(level[0] > 0.1);
  SND_CHECK_NEAR(level[1] / level[0], 0.5, 0.02);
}

// P32 end to end: a sliding loop sounds while reported and is silent after timeout + fade.
SND_TEST(IndepPlay, SlideSoundsThenFadesOut) {
  Fx f;
  SND_REQUIRE(f.Ok());
  f.s.Advance(2);
  for (int i = 0; i < 20; ++i) {
    SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, -2.0f, 0, 0, 0, 10.0f)}), SND_OK);
    f.s.Advance(1);
  }
  const Lr on = Measure(f.s, 0, 4);
  snd::test::Note("sliding: L %.4f R %.4f", on.l, on.r);
  SND_CHECK(on.l > 0.05);
  SND_CHECK(on.r < 0.2 * on.l);
  // Stop reporting: timeout 0.2 s at 1/60 s per frame = 12 frames, then the 0.1 s fade.
  for (int i = 0; i < 60; ++i) {
    SND_CHECK_EQ(f.Submit({}), SND_OK);
    f.s.Advance(1);
  }
  const Lr off = Measure(f.s, 0, 4);
  snd::test::Note("stopped: L %.4f R %.4f", off.l, off.r);
  SND_CHECK(std::fabs(off.l) < 1e-4);
  SND_CHECK_EQ(phystest::Stats(f.phys).tracking_timeouts, 1u);
}
