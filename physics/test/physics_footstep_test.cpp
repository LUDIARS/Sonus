// 9.2 footsteps: the two largest weights (first on ties; <= 0, NaN, infinite ignored), weight
// -> volume (normalised by the sum of the chosen two, through the weight curve, identity by
// default), the event chain footstep[M][motion] -> footstep_any[M] -> footstep_default ->
// nothing, the parameters (motion, foot material, weight, surface parameters), the foot's
// emitter, invalid input.
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "phys_rig.hpp"

using namespace phystest;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

snd_id Foot(std::uint32_t m, std::uint32_t motion) { return 0x2000u + m * 16u + motion; }

struct Fixture {
  explicit Fixture(snd_physics_config cfg) {
    SND_REQUIRE_EQ(sys.created, SND_OK);
    SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, nullptr, &phys), SND_OK);
    // 4 materials: 0 has every motion, 1 has Run only + an any-motion event, 2 any-motion only, 3 none.
    footstep.assign(4 * SND_PHYSICS_MOTION_COUNT, SND_ID_NONE);
    for (std::uint32_t mo = 0; mo < SND_PHYSICS_MOTION_COUNT; ++mo) footstep[0 * 6 + mo] = Foot(0, mo);
    footstep[1 * 6 + SND_MOTION_RUN] = Foot(1, SND_MOTION_RUN);
    any.assign(4, SND_ID_NONE);
    any[1] = 0x2100u;
    any[2] = 0x2200u;
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 4;
    d.footstep = footstep.data();
    d.footstep_any = any.data();
    d.footstep_default = 0x2F00u;
    SND_REQUIRE_EQ(snd_physics_set_table(phys, &d), SND_OK);
  }
  ~Fixture() { snd_phys_destroy(phys); }
  snd_result Step(std::vector<snd_surface_weight> w, snd_motion motion = SND_MOTION_WALK, std::uint16_t foot = 0,
                  snd_emitter_handle e = snd_emitter_handle{0}) {
    return snd_phys_play_footstep(phys, e, foot, w.empty() ? nullptr : w.data(), static_cast<std::uint32_t>(w.size()), motion);
  }
  Sys sys;
  snd_phys* phys = nullptr;
  std::vector<snd_id> footstep, any;
};

snd_surface_weight W(std::uint16_t m, float w) { return snd_surface_weight{m, 0, w}; }

}  // namespace

SND_TEST(PhysicsFootstep, TwoLargestWeightsMappedToVolume) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  // 0.5, 0.2, 0.3: the two largest are 0.5 and 0.3 -> 0.625 and 0.375.
  SND_CHECK_EQ(f.Step({W(0, 0.5f), W(1, 0.2f), W(2, 0.3f)}), SND_OK);
  std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.game_material_a, 0u);
  SND_CHECK_EQ(p[0].r.volume, 0.5f / 0.8f);
  SND_CHECK_EQ(p[0].Param(P(SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME)), 0.5f / 0.8f);
  SND_CHECK_EQ(p[1].r.game_material_a, 2u);
  SND_CHECK_EQ(p[1].r.volume, 0.3f / 0.8f);
  SND_CHECK_EQ(p[0].r.source, SND_PHYSICS_SOURCE_FOOTSTEP);
  SND_CHECK_EQ(Stats(f.phys).footstep_surfaces_ignored, 1u);
  // One surface: volume 1 whatever its weight.
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(0, 0.01f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.volume, 1.0f);
  // Ties: the first in the array; ignored weights: 0, negative, NaN, infinite.
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(3, 0.0f), W(0, 0.4f), W(1, kNan), W(2, 0.4f), W(1, 0.4f), W(3, -1.0f), W(1, kInf)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.game_material_a, 0u);
  SND_CHECK_EQ(p[1].r.game_material_a, 2u);
  SND_CHECK_EQ(p[0].r.volume, 0.5f);
  SND_CHECK_EQ(p[1].r.volume, 0.5f);
  SND_CHECK_EQ(Stats(f.phys).footstep_surfaces_ignored, 1u + 5u);
  // No usable surface: nothing, SND_OK.
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(0, 0.0f)}), SND_OK);
  SND_CHECK_EQ(f.Step({}), SND_OK);
  SND_CHECK_EQ(rec.recs.size(), 0u);
  // A weight curve of the table: (0, 0) (0.5, 0.2) (1, 1).
  std::vector<snd_id> fs(6, SND_ID_NONE);
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 1;
  d.footstep_default = 0x2F00u;
  d.weight_to_volume = Linear({{0.0f, 0.0f}, {0.5f, 0.2f}, {1.0f, 1.0f}});
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(0, 3.0f), W(0, 1.0f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_NEAR(p[0].r.volume, 0.2f + 0.8f * 0.5f, 1e-6f);  // w = 0.75
  SND_CHECK_NEAR(p[1].r.volume, 0.1f, 1e-6f);                // 0.25
  SND_CHECK_EQ(Stats(f.phys).footsteps, 6u);
  // Huge finite weights: summed in double (3e38 + 3e38 does not overflow): 0.5 each.
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(0, 3.0e38f), W(0, 3.0e38f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_NEAR(p[0].r.volume, 0.2f, 1e-6f);  // the table's curve at 0.5
  SND_CHECK_EQ(p[0].Param(P(SND_PHYSICS_PARAM_SURFACE_WEIGHT_NAME)), 0.5f);
}

SND_TEST(PhysicsFootstep, EventChainAndParameters) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  const snd_id motionId = P(SND_PHYSICS_PARAM_MOTION_NAME);
  const snd_id footId = P(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME);
  struct Case {
    std::uint16_t m;
    snd_motion motion;
    snd_id event;
    snd_physics_stage stage;
  };
  const Case cases[] = {
      {0, SND_MOTION_SPRINT, Foot(0, SND_MOTION_SPRINT), SND_PHYSICS_STAGE_FOOT_MOTION},
      {1, SND_MOTION_RUN, Foot(1, SND_MOTION_RUN), SND_PHYSICS_STAGE_FOOT_MOTION},
      {1, SND_MOTION_LAND, 0x2100u, SND_PHYSICS_STAGE_FOOT_ANY},
      {2, SND_MOTION_WALK, 0x2200u, SND_PHYSICS_STAGE_FOOT_ANY},
      {3, SND_MOTION_JUMP, 0x2F00u, SND_PHYSICS_STAGE_FOOT_DEFAULT},
      {9, SND_MOTION_SCUFF, 0x2F00u, SND_PHYSICS_STAGE_FOOT_DEFAULT},  // unknown material
  };
  for (const Case& c : cases) {
    rec.Clear();
    SND_CHECK_EQ(f.Step({W(c.m, 1.0f)}, c.motion, 2), SND_OK);
    const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
    SND_REQUIRE_EQ(p.size(), 1u);
    SND_CHECK_EQ(p[0].r.event, c.event);
    SND_CHECK_EQ(p[0].r.stage, c.stage);
    SND_CHECK_EQ(p[0].r.contact_kind, c.motion);
    SND_CHECK_EQ(p[0].Param(motionId), static_cast<float>(c.motion));
    SND_CHECK_EQ(p[0].Param(footId), 2.0f);
    SND_CHECK_EQ(p[0].r.material_b, 2u);
    SND_CHECK(p[0].r.playback.value != 0u);
    SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_FALLBACK), c.stage == SND_PHYSICS_STAGE_FOOT_MOTION ? 0u : 1u);
  }
  snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.footstep_by_motion, 2u);
  SND_CHECK_EQ(s.footstep_fallback_any_motion, 2u);
  SND_CHECK_EQ(s.footstep_fallback_default, 2u);
  SND_CHECK_EQ(s.fallback_unknown_material, 1u);
  SND_CHECK_EQ(s.requested_footstep, 6u);
  // No default: nothing; an unknown foot material is not passed.
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, SND_PHYSICS_DEFAULT_MATERIAL, SND_PHYSICS_ANY_MOTION, SND_ID_NONE),
               SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(3, 1.0f), W(0, 0.5f)}, SND_MOTION_WALK, 77), SND_OK);
  const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, Foot(0, SND_MOTION_WALK));
  SND_CHECK(!p[0].Has(footId));
  SND_CHECK_EQ(p[0].r.material_b, SND_PHYSICS_UNMAPPED);
  SND_CHECK_NEAR(p[0].r.volume, 1.0f / 3.0f, 1e-6f);
  SND_CHECK_EQ(Stats(f.phys).footstep_fallback_none, 1u);
  // Setters of the chain.
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, 3, SND_MOTION_JUMP, 0x2333u), SND_OK);
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, 3, SND_PHYSICS_ANY_MOTION, 0x2300u), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(3, 1.0f)}, SND_MOTION_JUMP), SND_OK);
  SND_CHECK_EQ(f.Step({W(3, 1.0f)}, SND_MOTION_LAND), SND_OK);
  const std::vector<Rec> q = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(q.size(), 2u);
  SND_CHECK_EQ(q[0].r.event, 0x2333u);
  SND_CHECK_EQ(q[1].r.event, 0x2300u);
}

SND_TEST(PhysicsFootstep, SurfaceParametersEmitterAndErrors) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  const snd_id gravel = P("Surface/Gravel");
  snd_physics_material_entry map[3];
  std::memset(map, 0, sizeof(map));
  map[0].sound_material = 0;
  map[1].sound_material = 2;  // "gravel path": material 2 + Gravel 0.6
  map[1].param_count = 1;
  map[1].params[0] = {gravel, 0.6f};
  map[2].sound_material = 1;
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, map, 3), SND_OK);
  const snd_emitter_handle foot = snd_create_emitter(f.sys.sys);
  SND_REQUIRE(foot.value != 0u);
  SND_CHECK_EQ(f.Step({W(1, 0.8f), W(0, 0.2f)}, SND_MOTION_WALK, 2, foot), SND_OK);
  const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.event, 0x2200u);  // sound material 2: any-motion
  SND_CHECK_EQ(p[0].Param(gravel), 0.6f);
  SND_CHECK_EQ(p[0].r.emitter.value, foot.value);
  SND_CHECK_EQ(p[0].Param(P(SND_PHYSICS_PARAM_FOOT_MATERIAL_NAME)), 1.0f);  // game 2 -> sound 1
  SND_CHECK(!p[1].Has(gravel));
  // Invalid input: nothing done.
  rec.Clear();
  SND_CHECK_EQ(f.Step({W(0, 1.0f)}, 6), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_play_footstep(f.phys, foot, 0, nullptr, 2, SND_MOTION_WALK), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_play_footstep(nullptr, foot, 0, nullptr, 0, SND_MOTION_WALK), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(rec.recs.size(), 0u);
  SND_CHECK_EQ(Stats(f.phys).calls_rejected, 2u);
  SND_CHECK_EQ(snd_destroy_emitter(f.sys.sys, foot), SND_OK);
  // C++ wrapper.
  rec.Clear();
  const snd::phys::SurfaceWeight w{0, 0, 1.0f};
  SND_CHECK(snd::phys::PlayFootstep(f.phys, snd::kNoEmitter, 0, &w, 1, snd::phys::Motion::Land) == snd::Result::Ok);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.event, Foot(0, SND_MOTION_LAND));
}
