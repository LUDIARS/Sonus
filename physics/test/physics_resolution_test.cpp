// 9.1 / 9.3: event resolution (pair -> singles overlaid -> default -> nothing, unknown
// materials), the statistics and observer records of each stage, strength curves (material,
// table default, built-in; lighter-object rule of pair / default events; clamping), the game
// material map with surface parameters, table replacement and the setters.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "phys_rig.hpp"

using namespace phystest;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

struct Fixture {
  explicit Fixture(snd_physics_config cfg) {
    SND_REQUIRE_EQ(sys.created, SND_OK);
    SND_REQUIRE_EQ(snd_physics_create(sys.sys, &cfg, nullptr, &phys), SND_OK);
  }
  ~Fixture() { snd_phys_destroy(phys); }
  snd_result Submit(const std::vector<snd_contact>& c, float dt = 0.01f) {
    return snd_phys_submit_contacts(phys, c.data(), static_cast<std::uint32_t>(c.size()), dt);
  }
  Sys sys;
  snd_phys* phys = nullptr;
};

const snd_id kVolumeless = 0;

}  // namespace

SND_TEST(PhysicsResolution, PairThenSinglesThenDefaultThenNothing) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  // Materials 0, 1, 2. Singles: 0 for every kind, 1 for Impact only, 2 none.
  std::vector<snd_id> single(3 * 4, SND_ID_NONE);
  for (std::uint32_t k = 0; k < 4; ++k) single[0 * 4 + k] = Ev(0, k);
  single[1 * 4 + SND_CONTACT_IMPACT] = Ev(1, SND_CONTACT_IMPACT);
  std::vector<snd_id> pair(9, SND_ID_NONE);
  pair[0 * 3 + 1] = 0x9001u;  // (0, 1) for every kind ...
  std::vector<snd_id> pairBreak(9, SND_ID_NONE);
  pairBreak[0 * 3 + 2] = 0x9002u;  // ... except Break, whose own column has (0, 2) only
  pairBreak[1 * 3 + 0] = 0x9999u;  // lower triangle: ignored
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 3;
  d.single = single.data();
  d.pair = pair.data();
  d.pair_by_kind[SND_CONTACT_BREAK] = pairBreak.data();
  d.default_event[SND_CONTACT_IMPACT] = 0x7000u;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 3u);
  auto at = [](float x, snd_contact_kind k, std::uint16_t a, std::uint16_t b) {
    return Contact(k, static_cast<std::uint64_t>(x) * 2u + 100u, static_cast<std::uint64_t>(x) * 2u + 101u, a, b, x, 0, 0, 1.0f);
  };
  struct Want {
    snd_contact c;
    std::vector<snd_id> events;
    snd_physics_stage stage;
  };
  const std::vector<Want> wants = {
      {at(0, SND_CONTACT_IMPACT, 0, 1), {0x9001u}, SND_PHYSICS_STAGE_PAIR},
      {at(10, SND_CONTACT_IMPACT, 1, 0), {0x9001u}, SND_PHYSICS_STAGE_PAIR},  // symmetric
      {at(20, SND_CONTACT_IMPACT, 0, 2), {Ev(0, 0)}, SND_PHYSICS_STAGE_SINGLE},  // only A has one
      {at(30, SND_CONTACT_IMPACT, 2, 1), {Ev(1, 0)}, SND_PHYSICS_STAGE_SINGLE},  // only B has one
      {at(40, SND_CONTACT_IMPACT, 1, 1), {Ev(1, 0)}, SND_PHYSICS_STAGE_SINGLE},  // same material: once
      {at(50, SND_CONTACT_IMPACT, 2, 2), {0x7000u}, SND_PHYSICS_STAGE_DEFAULT},
      {at(60, SND_CONTACT_IMPACT, 0, 5), {Ev(0, 0)}, SND_PHYSICS_STAGE_SINGLE},  // 5 >= N: the known side's single
      {at(65, SND_CONTACT_IMPACT, 9, 2), {0x7000u}, SND_PHYSICS_STAGE_DEFAULT},  // known side has none: default
      {at(66, SND_CONTACT_IMPACT, 9, 7), {0x7000u}, SND_PHYSICS_STAGE_DEFAULT},  // both unknown
      {at(70, SND_CONTACT_BREAK, 0, 1), {Ev(0, 3)}, SND_PHYSICS_STAGE_SINGLE},   // Break column has no (0, 1)
      {at(80, SND_CONTACT_BREAK, 2, 0), {0x9002u}, SND_PHYSICS_STAGE_PAIR},
      {at(90, SND_CONTACT_BREAK, 2, 2), {}, SND_PHYSICS_STAGE_NOTHING},
  };
  for (const Want& w : wants) {
    rec.Clear();
    SND_CHECK_EQ(f.Submit({w.c}), SND_OK);
    const std::vector<Rec> plays = rec.Of(SND_PHYSICS_RECORD_PLAY);
    SND_REQUIRE_EQ(plays.size(), w.events.size());
    for (std::size_t i = 0; i < plays.size(); ++i) {
      SND_CHECK_EQ(plays[i].r.event, w.events[i]);
      SND_CHECK_EQ(plays[i].r.stage, w.stage);
      SND_CHECK(plays[i].r.playback.value != 0u);
      SND_CHECK_EQ(plays[i].r.result, SND_OK);
    }
    const std::vector<Rec> fb = rec.Of(SND_PHYSICS_RECORD_FALLBACK);
    SND_CHECK_EQ(fb.size(), w.stage == SND_PHYSICS_STAGE_PAIR ? 0u : 1u);
    if (!fb.empty()) SND_CHECK_EQ(fb[0].r.stage, w.stage);
  }
  snd_physics_stats s = Stats(f.phys);
  SND_CHECK_EQ(s.resolved_pair, 3u);
  SND_CHECK_EQ(s.fallback_pair_to_single, 5u);
  SND_CHECK_EQ(s.fallback_single_to_default, 3u);
  SND_CHECK_EQ(s.fallback_none, 1u);
  SND_CHECK_EQ(s.fallback_unknown_material, 3u);
  SND_CHECK_EQ(s.dropped_no_event, 1u);
  SND_CHECK_EQ(s.group_requests, 11u);
  // Overlay: without the pair, both single events of (0, 1) play, A's first, on one emitter.
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 1, 0, SND_PHYSICS_ALL_KINDS, SND_ID_NONE), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({at(100, SND_CONTACT_IMPACT, 0, 1)}), SND_OK);
  const std::vector<Rec> two = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(two.size(), 2u);
  SND_CHECK_EQ(two[0].r.event, Ev(0, 0));
  SND_CHECK_EQ(two[1].r.event, Ev(1, 0));
  SND_CHECK_EQ(two[0].r.emitter.value, two[1].r.emitter.value);
  SND_CHECK_EQ(Stats(f.phys).requested_impact, 11u);  // 9 single-event groups + the overlay's 2
  SND_CHECK_EQ(Stats(f.phys).group_requests, 12u);
  // Unknown material and no default: nothing, counted twice (nothing + unknown).
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, SND_PHYSICS_ALL_KINDS, SND_ID_NONE), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({at(110, SND_CONTACT_IMPACT, 9, 2)}), SND_OK);
  s = Stats(f.phys);
  SND_CHECK_EQ(s.fallback_none, 2u);
  SND_CHECK_EQ(s.fallback_unknown_material, 4u);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_FALLBACK), 1u);
  SND_CHECK((rec.recs[0].r.flags & SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL) != 0u);
  SND_CHECK_EQ(rec.recs[0].r.material_a, SND_PHYSICS_UNMAPPED);
}

SND_TEST(PhysicsResolution, StrengthCurvesAndTheLighterObjectRule) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(2, &single);
  std::vector<snd_physics_curves> curves(2);
  std::memset(curves.data(), 0, sizeof(snd_physics_curves) * 2);
  curves[0].impulse_to_volume = Linear({{0.0f, 0.1f}, {10.0f, 1.0f}});
  curves[0].impulse_to_velocity = Linear({{0.0f, 0.0f}, {10.0f, 1.0f}});
  curves[0].mass_to_pitch = Linear({{1.0f, 2.0f}, {11.0f, 0.5f}});
  snd_physics_curves def;
  std::memset(&def, 0, sizeof(def));
  def.impulse_to_volume = Linear({{0.0f, 0.5f}});  // constant
  d.curves = curves.data();
  d.default_curves = &def;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  const snd_id velocityId = P(SND_PHYSICS_PARAM_VELOCITY_NAME);
  const snd_id massId = P(SND_PHYSICS_PARAM_MASS_NAME);
  // Singles: A uses material 0's curves and mass_a; B has none: table default volume,
  // built-in velocity (0.1, 0) (1, 1/3) (10, 2/3) (100, 1), built-in pitch 1.
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 1, 0, 0, 0, 5.0f, 2.0f, 6.0f, 3.0f)}), SND_OK);
  std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_NEAR(p[0].r.volume, 0.55f, 1e-6f);
  SND_CHECK_NEAR(p[0].r.velocity, 0.5f, 1e-6f);
  SND_CHECK_NEAR(p[0].Param(velocityId), 0.5f, 1e-6f);
  SND_CHECK_NEAR(p[0].r.pitch, 1.25f, 1e-6f);
  SND_CHECK_EQ(p[0].Param(massId), 6.0f);
  SND_CHECK_EQ(p[1].r.volume, 0.5f);
  SND_CHECK_NEAR(p[1].r.velocity, 1.0f / 3.0f + (1.0f / 3.0f) * (4.0f / 9.0f), 1e-6f);
  SND_CHECK_EQ(p[1].r.pitch, 1.0f);
  SND_CHECK_EQ(p[1].Param(massId), 3.0f);
  // Beyond the end points: the end values.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 10, 0, 0, 50.0f, 2.0f, 0.5f, 0.5f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.volume, 1.0f);
  SND_CHECK_EQ(p[0].r.velocity, 1.0f);
  SND_CHECK_EQ(p[0].r.pitch, 2.0f);
  // Pair override: the lighter object's material and mass (B, 3 kg: the default volume).
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 1, SND_PHYSICS_ALL_KINDS, 0x9000u), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 1, 20, 0, 0, 5.0f, 2.0f, 6.0f, 3.0f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, 0x9000u);
  SND_CHECK_EQ(p[0].r.volume, 0.5f);
  SND_CHECK_EQ(p[0].r.mass, 3.0f);
  // Equal masses: A (material 0's curves, pitch at 4 kg = 2 - 1.5 * 0.3 = 1.55).
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 1, 30, 0, 0, 5.0f, 2.0f, 4.0f, 4.0f)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_NEAR(p[0].r.volume, 0.55f, 1e-6f);
  SND_CHECK_NEAR(p[0].r.pitch, 1.55f, 1e-6f);
  // A NaN mass counts as 0: that object is the lighter one.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 1, 0, 40, 0, 0, 5.0f, 2.0f, 6.0f, kNan)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.mass, 0.0f);
  SND_CHECK_NEAR(p[0].r.volume, 0.55f, 1e-6f);  // B is material 0 here
  SND_CHECK_EQ(p[0].r.pitch, 2.0f);             // mass 0 is left of the first point
  // Curve values out of their meaning are refused: volume y in [0, 1000], pitch y in
  // [1/16, 16], velocity |y| <= 1e6; the bounds themselves are fine (and reach the record).
  {
    auto refused = [&](void (*set)(snd_physics_curves*)) {
      snd_physics_curves cv;
      std::memset(&cv, 0, sizeof(cv));
      set(&cv);
      return snd_physics_set_curves(f.phys, 0, &cv) == SND_ERR_INVALID_ARGUMENT;
    };
    SND_CHECK(refused([](snd_physics_curves* c) { c->impulse_to_volume = Linear({{0.0f, 1000.5f}}); }));
    SND_CHECK(refused([](snd_physics_curves* c) { c->speed_to_volume = Linear({{0.0f, -0.1f}}); }));
    SND_CHECK(refused([](snd_physics_curves* c) { c->mass_to_pitch = Linear({{0.0f, 0.0f}}); }));
    SND_CHECK(refused([](snd_physics_curves* c) { c->speed_to_pitch = Linear({{0.0f, 17.0f}}); }));
    SND_CHECK(refused([](snd_physics_curves* c) { c->impulse_to_velocity = Linear({{0.0f, 2.0e6f}}); }));
    std::vector<snd_id> s1;
    snd_physics_table_desc wd = SinglesTable(2, &s1);
    wd.weight_to_volume = Linear({{0.0f, 2000.0f}});
    SND_CHECK_EQ(snd_physics_set_table(f.phys, &wd), SND_ERR_INVALID_ARGUMENT);
    snd_physics_curves edge;
    std::memset(&edge, 0, sizeof(edge));
    edge.impulse_to_volume = Linear({{0.0f, 1000.0f}});
    edge.mass_to_pitch = Linear({{0.0f, 1.0f / 16.0f}, {10.0f, 16.0f}});
    SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &edge), SND_OK);
    SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 0, SND_PHYSICS_ALL_KINDS, 0x9100u), SND_OK);
    rec.Clear();
    SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 50, 0, 0, 5.0f, 2.0f, 0.0f, 0.0f),
                           Contact(SND_CONTACT_IMPACT, 3, 4, 0, 0, 60, 0, 0, 4.0f, 2.0f, 20.0f, 20.0f)}),
                 SND_OK);
    p = rec.Of(SND_PHYSICS_RECORD_PLAY);
    SND_REQUIRE_EQ(p.size(), 2u);
    SND_CHECK_EQ(p[0].r.volume, 1000.0f);
    SND_CHECK_EQ(p[0].r.pitch, 1.0f / 16.0f);
    SND_CHECK_EQ(p[1].r.pitch, 16.0f);
  }
  // Built-in curves once nothing is set: impulse 1 -> volume 0.5, velocity 1/3; 0.05 -> 0.25, 0.
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, nullptr), SND_OK);
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, SND_PHYSICS_DEFAULT_MATERIAL, nullptr), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 70, 0, 0, 1.0f, 2.0f, 1.0f, 1.0f),
                         Contact(SND_CONTACT_IMPACT, 3, 4, 0, 0, 80, 0, 0, 0.05f, 2.0f, 1.0f, 1.0f)}),
               SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.volume, 0.5f);
  SND_CHECK_NEAR(p[0].r.velocity, 1.0f / 3.0f, 1e-6f);
  SND_CHECK_EQ(p[0].r.pitch, 1.0f);
  SND_CHECK_EQ(p[1].r.volume, 0.25f);
  SND_CHECK_EQ(p[1].r.velocity, 0.0f);
}

SND_TEST(PhysicsResolution, MaterialMapAndSurfaceParameters) {
  Recorder rec;
  Fixture f(OpenConfig(&rec));
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(2, &single);  // 0 = stone, 1 = metal
  d.default_event[SND_CONTACT_IMPACT] = 0x7000u;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  const snd_id wet = P("Surface/Wetness");
  const snd_id rust = P("Surface/Rust");
  std::vector<snd_physics_material_entry> map(6);
  std::memset(map.data(), 0, sizeof(snd_physics_material_entry) * map.size());
  map[0].sound_material = 0;                           // dry stone
  map[1].sound_material = 0;                           // wet stone
  map[1].param_count = 1;
  map[1].params[0] = {wet, 1.0f};
  map[2].sound_material = 1;                           // rusty metal
  map[2].param_count = 1;
  map[2].params[0] = {rust, 0.3f};
  map[3].sound_material = SND_PHYSICS_UNMAPPED;        // unknown
  map[4].sound_material = 7;                           // >= N: unknown
  map[5].sound_material = 0;                           // damp stone
  map[5].param_count = 1;
  map[5].params[0] = {wet, 0.4f};
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, map.data(), 6), SND_OK);
  auto contact = [](float x, std::uint16_t a, std::uint16_t b) {
    return Contact(SND_CONTACT_IMPACT, 1, 2, a, b, x, 0, 0, 1.0f);
  };
  // Singles: each event gets its own material's surface parameters.
  SND_CHECK_EQ(f.Submit({contact(0, 1, 2)}), SND_OK);
  std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.event, Ev(0, 0));
  SND_CHECK_EQ(p[0].r.material_a, 0u);
  SND_CHECK_EQ(p[0].r.game_material_a, 1u);
  SND_CHECK_EQ(p[0].Param(wet), 1.0f);
  SND_CHECK(!p[0].Has(rust));
  SND_CHECK_EQ(p[1].r.event, Ev(1, 0));
  SND_CHECK_EQ(p[1].Param(rust), 0.3f);
  SND_CHECK(!p[1].Has(wet));
  // Pair override: both sides' parameters; the same id on both: the larger value.
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 1, SND_PHYSICS_ALL_KINDS, 0x9001u), SND_OK);
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 0, SND_PHYSICS_ALL_KINDS, 0x9000u), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({contact(10, 2, 1), contact(20, 5, 1)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.event, 0x9001u);
  SND_CHECK_EQ(p[0].Param(wet), 1.0f);
  SND_CHECK_EQ(p[0].Param(rust), 0.3f);
  SND_CHECK_EQ(p[1].r.event, 0x9000u);
  SND_CHECK_EQ(p[1].Param(wet), 1.0f);
  // It starts to rain: change the value only.
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 5, wet, 2.0f), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({contact(30, 5, 0)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].Param(wet), 2.0f);
  // Add parameters up to the limit, then LIMIT_REACHED; bad ids and values.
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("a"), 1.0f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("b"), 1.0f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("c"), 1.0f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("d"), 1.0f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("e"), 1.0f), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("d"), 3.0f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, SND_ID_NONE, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, SND_ID_RESERVED_FIRST, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, P("a"), kNan), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 6, P("a"), 1.0f), SND_ERR_NOT_FOUND);
  // Unknown game materials (unmapped, >= N, past the map): the known side's single (with its
  // surface parameters, game 0 has a..d now), no pair; both unknown: the default.
  rec.Clear();
  SND_CHECK_EQ(f.Submit({contact(40, 3, 0), contact(50, 4, 0), contact(60, 0, 6), contact(65, 3, 6)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 4u);
  for (std::size_t i = 0; i < 3; ++i) {
    SND_CHECK_EQ(p[i].r.event, Ev(0, 0));
    SND_CHECK_EQ(p[i].r.stage, SND_PHYSICS_STAGE_SINGLE);
    SND_CHECK((p[i].r.flags & SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL) != 0u);
    SND_CHECK_EQ(p[i].Param(P("d")), 3.0f);
  }
  SND_CHECK_EQ(p[3].r.event, 0x7000u);
  SND_CHECK_EQ(p[3].r.stage, SND_PHYSICS_STAGE_DEFAULT);
  SND_CHECK_EQ(Stats(f.phys).fallback_unknown_material, 4u);
  // count 0: the identity again (game 1 is sound material 1, no parameters).
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, nullptr, 0), SND_OK);
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 1, SND_PHYSICS_ALL_KINDS, SND_ID_NONE), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({contact(70, 1, 1)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, Ev(1, 0));
  SND_CHECK(!p[0].Has(wet));
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 0, wet, 1.0f), SND_ERR_NOT_FOUND);  // no map
  // One entry on the identity map: entries in between map to themselves, beyond is unknown.
  snd_physics_material_entry e{};
  e.sound_material = 1;
  e.param_count = 1;
  e.params[0] = {wet, 0.25f};
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 3, &e), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({contact(80, 3, 0), contact(90, 4, 4)}), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 3u);
  SND_CHECK_EQ(p[0].r.event, Ev(1, 0));
  SND_CHECK_EQ(p[0].Param(wet), 0.25f);
  SND_CHECK_EQ(p[1].r.event, Ev(0, 0));
  SND_CHECK_EQ(p[2].r.event, 0x7000u);
  // Entry errors.
  e.param_count = 5;
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &e), SND_ERR_INVALID_ARGUMENT);
  e.param_count = 2;
  e.params[1] = {wet, 1.0f};  // repeated id
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &e), SND_ERR_INVALID_ARGUMENT);
  e.param_count = 1;
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 1024, &e), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, nullptr), SND_ERR_INVALID_ARGUMENT);
  std::vector<snd_physics_material_entry> big(1025);
  std::memset(big.data(), 0, sizeof(snd_physics_material_entry) * big.size());
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, big.data(), 1025), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, nullptr, 3), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(PhysicsResolution, TableReplacementAndSetterErrors) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.max_materials = 4;
  Fixture f(cfg);
  // Empty table at creation: nothing plays, counted as "nothing".
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 0u);
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0, 1.0f)}), SND_OK);
  SND_CHECK_EQ(Stats(f.phys).dropped_no_event, 1u);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(4, &single);
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  d.material_count = 5;
  std::vector<snd_id> s5;
  snd_physics_table_desc d5 = SinglesTable(5, &s5);
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &d5), SND_ERR_LIMIT_REACHED);
  // A bad curve: refused, the table is unchanged.
  std::vector<snd_physics_curves> curves(4);
  std::memset(curves.data(), 0, sizeof(snd_physics_curves) * 4);
  curves[2].speed_to_pitch = Linear({{2.0f, 1.0f}, {1.0f, 1.0f}});
  snd_physics_table_desc bad = SinglesTable(2, &s5);
  bad.curves = curves.data();
  bad.material_count = 4;
  std::vector<snd_id> s4b;
  snd_physics_table_desc d4b = SinglesTable(4, &s4b);
  d4b.curves = curves.data();
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &d4b), SND_ERR_INVALID_ARGUMENT);
  snd_physics_table_desc nanTh = SinglesTable(1, &s5);
  nanTh.impulse_threshold = -1.0f;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &nanTh), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_table(f.phys, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 4u);
  (void)bad;
  // Setters.
  SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 4, SND_CONTACT_IMPACT, 1u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 0, 4, 1u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 4, SND_CONTACT_IMPACT, 1u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, 9, 1u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, 0, 6, 1u), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_footstep_event(f.phys, SND_PHYSICS_DEFAULT_MATERIAL, SND_MOTION_RUN, 1u),
               SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 4, nullptr), SND_ERR_INVALID_ARGUMENT);
  snd_physics_curves bc;
  std::memset(&bc, 0, sizeof(bc));
  bc.impulse_to_volume.count = 9;
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &bc), SND_ERR_INVALID_ARGUMENT);
  // One entry takes effect at the next call.
  SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 2, SND_CONTACT_IMPACT, 0xABCDu), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 0, 1.0f, 2, 2)}), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.event, 0xABCDu);
  // A smaller table: material 3 becomes unknown.
  std::vector<snd_id> s2;
  snd_physics_table_desc d2 = SinglesTable(2, &s2);
  d2.default_event[SND_CONTACT_IMPACT] = 0x7777u;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &d2), SND_OK);
  rec.Clear();
  SND_CHECK_EQ(f.Submit({Impact(1, 2, 10, 1.0f, 3, 3)}), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.event, 0x7777u);
  // An older (smaller) table desc: the fields it does not cover keep their defaults.
  snd_physics_table_desc old = SinglesTable(2, &s2);
  old.struct_size = static_cast<std::uint32_t>(offsetof(snd_physics_table_desc, footstep));
  old.footstep_default = 0x5555u;  // not covered: ignored
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &old), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 2u);
  old.struct_size = 6;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &old), SND_ERR_INVALID_ARGUMENT);
  (void)kVolumeless;
}

SND_TEST(PhysicsResolution, ParameterIdsComeFromTheConfig) {
  Recorder rec;
  snd_physics_config cfg = OpenConfig(&rec);
  cfg.params.count = SND_ID_NONE;  // not passed
  cfg.params.impulse = P("MyGame/Hit");
  Fixture f(cfg);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 3.0f, 2.5f, 7.0f, 9.0f)}), SND_OK);
  const std::vector<Rec> p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].Param(P("MyGame/Hit")), 3.0f);
  SND_CHECK(!p[0].Has(P(SND_PHYSICS_PARAM_IMPULSE_NAME)));
  SND_CHECK(!p[0].Has(P(SND_PHYSICS_PARAM_COUNT_NAME)));
  SND_CHECK_EQ(p[0].Param(P(SND_PHYSICS_PARAM_SPEED_NAME)), 2.5f);
  SND_CHECK_EQ(p[0].Param(P(SND_PHYSICS_PARAM_MASS_NAME)), 7.0f);  // single of A: mass_a
  SND_CHECK_EQ(p[0].r.param_count, 4u);  // impulse, velocity, speed, mass
  // The default names hash as documented; reserved ids are refused.
  snd_physics_config def;
  snd_physics_config_init(&def);
  SND_CHECK_EQ(def.params.velocity, snd_hash_id("Phys/Velocity"));
  SND_CHECK_EQ(def.params.surface_weight, snd_hash_id("Phys/SurfaceWeight"));
  def.params.mass = SND_ID_RESERVED_LAST;
  snd_phys* p2 = nullptr;
  SND_CHECK_EQ(snd_physics_create(f.sys.sys, &def, nullptr, &p2), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(p2 == nullptr);
}
