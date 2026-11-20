// Independent tests: footsteps (9.2, decision D). Properties P29, P30.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

constexpr snd_id kF0Run = 0xF0A1u, kF0Any = 0xF0AAu, kF1Walk = 0xF1A0u, kFD = 0xFDDDu;

struct Fx {
  Fx() {
    snd_physics_config c = Cfg(&rec);
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
    if (phys == nullptr) return;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 3;
    tableResult = snd_physics_set_table(phys, &d);
    snd_physics_set_footstep_event(phys, 0, SND_MOTION_RUN, kF0Run);
    snd_physics_set_footstep_event(phys, 0, SND_PHYSICS_ANY_MOTION, kF0Any);
    snd_physics_set_footstep_event(phys, 1, SND_MOTION_WALK, kF1Walk);
    snd_physics_set_footstep_event(phys, SND_PHYSICS_DEFAULT_MATERIAL, SND_PHYSICS_ANY_MOTION, kFD);
  }
  ~Fx() { snd_physics_destroy(phys); }
  snd_result Step(std::vector<snd_surface_weight> w, snd_motion motion, std::uint16_t foot = 9,
                  snd_emitter_handle e = snd_emitter_handle{0}) {
    rec.Clear();
    const snd_result r =
        snd_phys_play_footstep(phys, e, foot, w.empty() ? nullptr : w.data(), static_cast<std::uint32_t>(w.size()), motion);
    s.Advance(1);
    return r;
  }
  const Rec* PlayOf(snd_id ev) const {
    for (const Rec& r : rec.recs) {
      if (r.r.kind == SND_PHYSICS_RECORD_PLAY && r.r.event == ev) return &r;
    }
    return nullptr;
  }
  Sys s;
  Recorder rec;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  snd_result tableResult = SND_ERR_INVALID_ARGUMENT;
};

snd_surface_weight W(std::uint16_t m, float w) {
  snd_surface_weight x{};
  x.material = m;
  x.weight = w;
  return x;
}

}  // namespace

// P29, P30: two largest, ties first, weight / (w1 + w2), event chain, parameters.
SND_TEST(IndepFootstep, TwoLargestAndEventChain) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(f.tableResult, SND_OK);
  // Material 2 0.2 (first of the tie), 1 0.6, 0 0.2 (tie, later: ignored).
  SND_CHECK_EQ(f.Step({W(2, 0.2f), W(1, 0.6f), W(0, 0.2f)}, SND_MOTION_WALK, 1), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 2u);
  const Rec* a = f.PlayOf(kF1Walk);  // 1: footstep[1][walk]
  const Rec* d = f.PlayOf(kFD);      // 2: nothing set -> footstep_default
  SND_REQUIRE(a != nullptr);
  SND_REQUIRE(d != nullptr);
  SND_CHECK_EQ(a->r.stage, SND_PHYSICS_STAGE_FOOT_MOTION);
  SND_CHECK_EQ(d->r.stage, SND_PHYSICS_STAGE_FOOT_DEFAULT);
  SND_CHECK_NEAR(a->r.volume, 0.75f, 1e-6);
  SND_CHECK_NEAR(d->r.volume, 0.25f, 1e-6);
  SND_CHECK_EQ(d->r.game_material_a, 2u);
  SND_CHECK_EQ(a->Param(P(SND_PHYSICS_PARAM_MOTION_NAME)), static_cast<float>(SND_MOTION_WALK));
  SND_CHECK_EQ(a->Param(P(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME)), 1.0f);  // foot material 1
  SND_CHECK_NEAR(a->Param(P(SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME)), 0.75f, 1e-6);
  SND_CHECK_EQ(a->r.emitter.value, 0u);  // 2D
  // Run on material 0: motion column; walk on material 0: the any-motion column.
  SND_CHECK_EQ(f.Step({W(0, 1.0f)}, SND_MOTION_RUN), SND_OK);
  SND_REQUIRE(f.PlayOf(kF0Run) != nullptr);
  SND_CHECK_NEAR(f.PlayOf(kF0Run)->r.volume, 1.0f, 1e-6);  // one surface: curve(1)
  SND_CHECK_EQ(f.Step({W(0, 0.5f)}, SND_MOTION_WALK), SND_OK);
  SND_REQUIRE(f.PlayOf(kF0Any) != nullptr);
  SND_CHECK_EQ(f.PlayOf(kF0Any)->r.stage, SND_PHYSICS_STAGE_FOOT_ANY);
  // Unknown material: straight to the default.
  SND_CHECK_EQ(f.Step({W(17, 0.5f)}, SND_MOTION_RUN), SND_OK);
  SND_REQUIRE(f.PlayOf(kFD) != nullptr);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.footsteps, 4u);
  SND_CHECK_EQ(st.requested_footstep, 5u);
  SND_CHECK_EQ(st.footstep_by_motion, 2u);
  SND_CHECK_EQ(st.footstep_fallback_any_motion, 1u);
  SND_CHECK_EQ(st.footstep_fallback_default, 2u);
  SND_CHECK_EQ(st.footstep_surfaces_ignored, 1u);
  SND_CHECK_EQ(st.dropped_budget, 0u);  // footsteps are not budgeted
}

// P29: weights <= 0, NaN, infinite are ignored; nothing to play is fine.
SND_TEST(IndepFootstep, IgnoredWeights) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_CHECK_EQ(f.Step({W(0, 0.0f), W(0, -1.0f), W(1, kNaN), W(1, kInf), W(1, 2.0f)}, SND_MOTION_WALK), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_REQUIRE(f.PlayOf(kF1Walk) != nullptr);
  SND_CHECK_NEAR(f.PlayOf(kF1Walk)->r.volume, 1.0f, 1e-6);
  SND_CHECK_EQ(phystest::Stats(f.phys).footstep_surfaces_ignored, 4u);
  SND_CHECK_EQ(f.Step({W(0, 0.0f)}, SND_MOTION_WALK), SND_OK);
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 0u);
  SND_CHECK_EQ(f.Step({}, SND_MOTION_WALK), SND_OK);
  // (Very large weights: IndepReview.Defect_FootstepHugeWeightsOverflow.)
}

// P29: weight_to_volume curve; P30 errors.
SND_TEST(IndepFootstep, WeightCurveAndErrors) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 2;
  d.footstep_default = kFD;
  d.weight_to_volume = phystest::Linear({{0.0f, 0.0f}, {1.0f, 0.5f}});
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(f.Step({W(0, 3.0f), W(1, 1.0f)}, SND_MOTION_LAND), SND_OK);
  std::vector<float> v;
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) v.push_back(r.r.volume);
  std::sort(v.begin(), v.end());
  SND_REQUIRE_EQ(v.size(), 2u);
  SND_CHECK_NEAR(v[0], 0.125f, 1e-6);
  SND_CHECK_NEAR(v[1], 0.375f, 1e-6);
  SND_CHECK_EQ(f.Step({W(0, 1.0f)}, static_cast<snd_motion>(SND_PHYSICS_MOTION_COUNT)), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, snd_emitter_handle{0}, 0, nullptr, 2, SND_MOTION_WALK),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_play_footstep(nullptr, snd_emitter_handle{0}, 0, nullptr, 0, SND_MOTION_WALK),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, 2, SND_MOTION_WALK, kFD), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, 0, 6u, kFD), SND_ERR_INVALID_ARGUMENT);
}

// P30: played on the foot's emitter; surface parameters of the surface's game material.
SND_TEST(IndepFootstep, FootEmitterAndSurfaceParams) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  const snd_emitter_handle foot = snd_create_emitter(f.s.sys);
  SND_REQUIRE(foot.value != 0u);
  snd_physics_material_entry e[2] = {};
  e[0].sound_material = 1;
  e[0].param_count = 1;
  e[0].params[0] = snd_physics_param{0x9911u, 0.7f};
  e[1].sound_material = 0;
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 2), SND_OK);
  SND_CHECK_EQ(f.Step({W(0, 1.0f)}, SND_MOTION_WALK, 1, foot), SND_OK);  // game 0 -> sound 1
  const Rec* r = f.PlayOf(kF1Walk);
  SND_REQUIRE(r != nullptr);
  SND_CHECK_EQ(r->r.emitter.value, foot.value);
  SND_CHECK_EQ(r->Param(0x9911u), 0.7f);
  // The foot material parameter: the value given (game material) or its sound material?
  // Decision D only says "足の材質は Parameter"; recorded here for the report.
  snd::test::Note("foot material parameter for game material 1 (sound 0): %g",
                  static_cast<double>(r->Param(P(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME))));
  snd_destroy_emitter(f.s.sys, foot);
}
