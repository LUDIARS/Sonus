// Independent tests: event resolution, material map, surface parameters, curves,
// parameter ids (9.1, 9.3, decision C). Properties P22..P28 of INDEP_PROPERTIES.md.
// Tests whose name contains "Undecided" pin a rule the implementer chose (the
// "実装者が決めたこと" list) that the acceptance side has not decided yet.
#include <cmath>
#include <cstdint>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

constexpr snd_id kPA = 0xA100u, kPB = 0xA200u, kS0 = 0xB000u, kS1 = 0xB001u, kS2 = 0xB002u, kD = 0xD000u;

struct Fx {
  explicit Fx(snd_physics_config c = Cfg(nullptr)) {
    c.observer = &Recorder::Fn;
    c.observer_user = &rec;
    c.budget_per_second = 1.0e6f;
    c.cooldown_seconds = 0.0f;
    created = snd_physics_create(s.sys, &c, nullptr, &phys);
  }
  ~Fx() { snd_physics_destroy(phys); }
  snd_result Submit(std::vector<snd_contact> cs) {
    rec.Clear();
    const snd_result r = snd_phys_submit_contacts(phys, cs.data(), static_cast<std::uint32_t>(cs.size()), 0.01f);
    s.Advance(1);
    return r;
  }
  std::vector<snd_id> PlayedEvents() const {
    std::vector<snd_id> v;
    for (const Rec& r : rec.Of(SND_PHYSICS_RECORD_PLAY)) v.push_back(r.r.event);
    return v;
  }
  Sys s;
  Recorder rec;
  snd_phys* phys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
};

// N = 3, Impact: pair (0, 2) = kPA; single 0 = kS0, single 1 = kS1, single 2 none; default kD.
snd_result ThreeMaterials(snd_phys* p) {
  static snd_id single[12];
  static snd_id pair[9];
  for (snd_id& x : single) x = SND_ID_NONE;
  for (snd_id& x : pair) x = SND_ID_NONE;
  single[0 * 4 + SND_CONTACT_IMPACT] = kS0;
  single[1 * 4 + SND_CONTACT_IMPACT] = kS1;
  single[0 * 4 + SND_CONTACT_BREAK] = kS0;
  pair[0 * 3 + 2] = kPA;
  pair[2 * 3 + 1] = kPB;  // lower triangle (2, 1): ignored
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 3;
  d.single = single;
  d.pair = pair;
  d.default_event[SND_CONTACT_IMPACT] = kD;
  return snd_physics_set_table(p, &d);
}

snd_contact Imp(std::uint16_t ma, std::uint16_t mb, float impulse = 1.0f, float massA = 1.0f, float massB = 1.0f,
                std::uint64_t oa = 1, std::uint64_t ob = 2) {
  return Contact(SND_CONTACT_IMPACT, oa, ob, ma, mb, 0.0f, 0.0f, 0.0f, impulse, 1.0f, massA, massB);
}

}  // namespace

// P22, P23: pair -> singles -> default -> nothing, with the stage statistics.
SND_TEST(IndepResolve, FallbackOrderAndStats) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(2, 0)}), SND_OK);  // pair, either order
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kPA});
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_FALLBACK), 0u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.stage, SND_PHYSICS_STAGE_PAIR);
  SND_CHECK_EQ(f.Submit({Imp(0, 1)}), SND_OK);  // both singles
  std::vector<snd_id> ev = f.PlayedEvents();
  std::sort(ev.begin(), ev.end());
  SND_CHECK(ev == (std::vector<snd_id>{kS0, kS1}));
  SND_CHECK_EQ(f.rec.Count(SND_PHYSICS_RECORD_FALLBACK), 1u);
  SND_CHECK_EQ(f.Submit({Imp(1, 2)}), SND_OK);  // (2, 1) lower triangle ignored; single of 1 only
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS1});
  SND_CHECK_EQ(f.Submit({Imp(2, 2)}), SND_OK);  // default
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  SND_CHECK_EQ(f.Submit({Imp(0, 0)}), SND_OK);  // same material: once (undecided 6)
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS0});
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, SND_CONTACT_IMPACT, SND_ID_NONE), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(2, 2)}), SND_OK);  // nothing
  SND_CHECK(f.PlayedEvents().empty());
  const std::vector<Rec> fb = f.rec.Of(SND_PHYSICS_RECORD_FALLBACK);
  SND_REQUIRE_EQ(fb.size(), 1u);
  SND_CHECK_EQ(fb[0].r.stage, SND_PHYSICS_STAGE_NOTHING);
  SND_CHECK_EQ(fb[0].r.event, SND_ID_NONE);
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.resolved_pair, 1u);
  SND_CHECK_EQ(st.fallback_pair_to_single, 3u);  // (0,1), (1,2), (0,0)
  SND_CHECK_EQ(st.fallback_single_to_default, 1u);
  SND_CHECK_EQ(st.dropped_no_event, 1u);
}

// P23: pair_by_kind replaces `pair` for its kind only.
SND_TEST(IndepResolve, PairByKind) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  snd_id single[8];
  for (snd_id& x : single) x = SND_ID_NONE;
  single[0 * 4 + SND_CONTACT_BREAK] = kS0;
  snd_id pair[4] = {SND_ID_NONE, kPA, SND_ID_NONE, SND_ID_NONE};
  snd_id pairBreak[4] = {SND_ID_NONE, SND_ID_NONE, SND_ID_NONE, SND_ID_NONE};
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 2;
  d.single = single;
  d.pair = pair;
  d.pair_by_kind[SND_CONTACT_BREAK] = pairBreak;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 0)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kPA});
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_BREAK, 1, 2, 1, 0, 0, 0, 0, 1.0f)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS0});
  // Setter per kind.
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 1, 0, SND_CONTACT_BREAK, kPB), SND_OK);
  SND_CHECK_EQ(f.Submit({Contact(SND_CONTACT_BREAK, 1, 2, 0, 1, 0, 0, 0, 1.0f)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kPB});
  SND_CHECK_EQ(f.Submit({Imp(1, 0)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kPA});
  // Out of range.
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 2, 0, SND_CONTACT_IMPACT, kPB), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_pair_event(f.phys, 0, 1, 4u, kPB), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_single_event(f.phys, 2, SND_CONTACT_IMPACT, kPB), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_default_event(f.phys, 5u, kPB), SND_ERR_INVALID_ARGUMENT);
}

// P24: unknown materials go to the default (with the statistic and flag); the identity
// map; a registered map; entries filled in by identity (undecided 11).
SND_TEST(IndepResolve, MaterialMapUnknownToDefault) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  // Identity: game 7 >= N = 3 is unknown; material 0 is known and has a single event: that
  // single plays (decision 1 of the review).
  SND_CHECK_EQ(f.Submit({Imp(0, 7)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS0});
  const Rec play = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  SND_CHECK(play.r.flags & SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL);
  SND_CHECK_EQ(play.r.game_material_b, 7u);
  SND_CHECK_EQ(play.r.material_b, SND_PHYSICS_UNMAPPED);
  SND_CHECK_EQ(phystest::Stats(f.phys).fallback_unknown_material, 1u);
  // A map: game 0 -> 2, game 1 -> unmapped; game 2 >= count -> unknown.
  snd_physics_material_entry e[2] = {};
  e[0].sound_material = 2;
  e[1].sound_material = SND_PHYSICS_UNMAPPED;
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 2), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 0)}), SND_OK);  // sound (2, 2): default
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.material_a, 2u);
  SND_CHECK_EQ(f.Submit({Imp(1, 1)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  SND_CHECK_EQ(f.Submit({Imp(2, 2)}), SND_OK);  // game 2 not in a map of 2
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  SND_CHECK_EQ(phystest::Stats(f.phys).fallback_unknown_material, 3u);
  // count 0 restores the identity.
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, nullptr, 0), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 1)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS1});
  // Undecided 11: one entry past the identity map fills the gap with identity entries.
  snd_physics_material_entry one{};
  one.sound_material = 0;
  SND_REQUIRE_EQ(snd_physics_set_material_entry(f.phys, 5, &one), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 5)}), SND_OK);  // game 1 -> 1, game 5 -> 0: singles of 1 and 0
  std::vector<snd_id> ev = f.PlayedEvents();
  std::sort(ev.begin(), ev.end());
  SND_CHECK(ev == (std::vector<snd_id>{kS0, kS1}));
  SND_CHECK_EQ(f.Submit({Imp(6, 6)}), SND_OK);  // past the map: unknown
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  // Invalid entries.
  snd_physics_material_entry bad{};
  bad.sound_material = 0;
  bad.param_count = SND_PHYSICS_MAX_SURFACE_PARAMS + 1;
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.param_count = 2;
  bad.params[0] = snd_physics_param{0x55u, 1.0f};
  bad.params[1] = snd_physics_param{0x55u, 2.0f};  // repeated id
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.params[1] = snd_physics_param{SND_ID_RESERVED_FIRST, 2.0f};
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.params[1] = snd_physics_param{0x56u, kNaN};
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, nullptr, 3), SND_ERR_INVALID_ARGUMENT);
}

// P24 capacity: max_game_materials exactly and + 1.
SND_TEST(IndepResolve, MaterialMapCapacity) {
  snd_physics_config c = Cfg(nullptr);
  c.max_game_materials = 4;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  snd_physics_material_entry e[5] = {};
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, e, 4), SND_OK);
  SND_CHECK_EQ(snd_physics_set_material_map(f.phys, e, 5), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 3, &e[0]), SND_OK);
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 4, &e[0]), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_set_material_entry(f.phys, 0xFFFFu, &e[0]), SND_ERR_LIMIT_REACHED);
  // Materials: exactly max_materials and + 1.
  snd_physics_config c2 = Cfg(nullptr);
  c2.max_materials = 3;
  Fx g(c2);
  SND_REQUIRE_EQ(g.created, SND_OK);
  SND_CHECK_EQ(ThreeMaterials(g.phys), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(g.phys), 3u);
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 4;
  SND_CHECK_EQ(snd_physics_set_table(g.phys, &d), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_material_count(g.phys), 3u);  // unchanged on error
}

// P25: surface parameters: single = own material's, pair / default = both (larger value).
SND_TEST(IndepResolve, SurfaceParameters) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  const snd_id W = 0x7001u, G = 0x7002u;
  snd_physics_material_entry e[3] = {};
  e[0].sound_material = 0;
  e[0].param_count = 2;
  e[0].params[0] = snd_physics_param{W, 0.3f};
  e[0].params[1] = snd_physics_param{G, 1.0f};
  e[1].sound_material = 1;
  e[1].param_count = 1;
  e[1].params[0] = snd_physics_param{W, 0.8f};
  e[2].sound_material = 2;
  e[2].param_count = 1;
  e[2].params[0] = snd_physics_param{W, 0.1f};
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 3), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 1)}), SND_OK);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    if (r.r.event == kS0) {
      SND_CHECK_EQ(r.Param(W), 0.3f);
      SND_CHECK_EQ(r.Param(G), 1.0f);
    } else {
      SND_CHECK_EQ(r.r.event, kS1);
      SND_CHECK_EQ(r.Param(W), 0.8f);
      SND_CHECK(!r.Has(G));
    }
  }
  SND_CHECK_EQ(f.Submit({Imp(0, 2)}), SND_OK);  // pair: both, larger W
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(W), 0.3f);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(G), 1.0f);
  SND_CHECK_EQ(f.Submit({Imp(2, 2)}), SND_OK);  // default of (2, 2)
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(W), 0.1f);
  // "It starts to raining": the new value from the next call.
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 2, W, 0.9f), SND_OK);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 2, G, 0.5f), SND_OK);  // added
  SND_CHECK_EQ(f.Submit({Imp(0, 2)}), SND_OK);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(W), 0.9f);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Param(G), 1.0f);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 3, W, 0.9f), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 2, SND_ID_NONE, 0.9f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_surface_param(f.phys, 2, W, kInf), SND_ERR_INVALID_ARGUMENT);
}

// P26: curves of a single event: own material, own mass; end values outside; clamping.
SND_TEST(IndepResolve, CurvesOfSingleEvents) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  snd_physics_curves c0{}, c1{};
  c0.impulse_to_volume = phystest::Linear({{0.0f, 0.0f}, {10.0f, 2.0f}});
  c0.impulse_to_velocity = phystest::Linear({{0.0f, 0.0f}, {4.0f, 1.0f}});
  c0.mass_to_pitch = phystest::Linear({{1.0f, 1.0f}, {10.0f, 2.0f}});
  c1.impulse_to_volume = phystest::Linear({{0.0f, 1000.0f}, {1.0f, 1000.0f}});  // the largest volume
  c1.mass_to_pitch = phystest::Linear({{1.0f, 16.0f}, {10.0f, 1.0f / 16.0f}});   // the pitch range
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &c0), SND_OK);
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 1, &c1), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 1, 2.5f, 5.5f, 20.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 2u);
  for (const Rec& r : f.rec.Of(SND_PHYSICS_RECORD_PLAY)) {
    if (r.r.event == kS0) {
      SND_CHECK_NEAR(r.r.volume, 0.5f, 1e-6);
      SND_CHECK_NEAR(r.r.velocity, 0.625f, 1e-6);
      SND_CHECK_NEAR(r.Param(P(SND_PHYSICS_PARAM_VELOCITY_NAME)), 0.625f, 1e-6);
      SND_CHECK_NEAR(r.r.pitch, 1.5f, 1e-6);  // mass_a 5.5 on (1,1)-(10,2)
      SND_CHECK_EQ(r.r.mass, 5.5f);
      SND_CHECK_EQ(r.Param(P(SND_PHYSICS_PARAM_MASS_NAME)), 5.5f);
    } else {
      SND_CHECK_EQ(r.r.volume, 1000.0f);
      SND_CHECK_EQ(r.r.pitch, 1.0f / 16.0f);  // mass_b 20 -> the end value 1/16
      SND_CHECK_EQ(r.r.mass, 20.0f);
    }
  }
  // Below the first point: the first value; a step (equal x).
  snd_physics_curves c2{};
  c2.impulse_to_volume = phystest::Linear({{1.0f, 0.2f}, {2.0f, 0.2f}, {2.0f, 0.6f}, {3.0f, 0.6f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &c2), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 0.5f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.2f, 1e-6);
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 1.99f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.2f, 1e-6);
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 2.01f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.6f, 1e-6);
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 1.0e9f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.6f, 1e-6);
  // Invalid curves.
  snd_physics_curves bad{};
  bad.mass_to_pitch = phystest::Linear({{2.0f, 1.0f}, {1.0f, 1.0f}});  // decreasing x
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.mass_to_pitch = phystest::Linear({{1.0f, kNaN}});
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.mass_to_pitch.count = SND_PHYSICS_MAX_CURVE_POINTS + 1;
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 0, &bad), SND_ERR_INVALID_ARGUMENT);
  // y ranges (decision 2 of the review): volume [0, 1000], pitch [1/16, 16], velocity |y| <= 1e6.
  struct YCase {
    int curve;  // 0 impulse_to_volume, 1 impulse_to_velocity, 2 mass_to_pitch, 3 speed_to_volume, 4 speed_to_pitch
    float y;
    bool ok;
  };
  const YCase ys[] = {{0, 1000.0f, true},  {0, 1000.5f, false}, {0, -0.001f, false}, {0, 0.0f, true},
                      {3, 1000.0f, true},  {3, 1001.0f, false}, {1, 1.0e6f, true},   {1, -1.0e6f, true},
                      {1, 1.1e6f, false},  {2, 16.0f, true},    {2, 16.5f, false},   {2, 1.0f / 16.0f, true},
                      {2, 0.06f, false},   {4, 0.0f, false},    {4, 16.0f, true},    {4, kInf, false}};
  for (const YCase& yc : ys) {
    snd_physics_curves t{};
    snd_physics_curve* cs[5] = {&t.impulse_to_volume, &t.impulse_to_velocity, &t.mass_to_pitch, &t.speed_to_volume,
                                &t.speed_to_pitch};
    *cs[yc.curve] = phystest::Linear({{0.0f, yc.curve == 2 || yc.curve == 4 ? 1.0f : 0.5f}, {1.0f, yc.y}});
    const snd_result r1 = snd_physics_set_curves(f.phys, 1, &t);
    SND_CHECK_MSG((r1 == SND_OK) == yc.ok, "set_curves curve %d y %g: %d", yc.curve, static_cast<double>(yc.y),
                  static_cast<int>(r1));
    // The same through set_table (default curves); a refused table leaves the old one.
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    d.material_count = 1;
    d.default_curves = &t;
    const snd_result r2 = snd_physics_set_table(f.phys, &d);
    SND_CHECK_MSG((r2 == SND_OK) == yc.ok, "set_table curve %d y %g: %d", yc.curve, static_cast<double>(yc.y),
                  static_cast<int>(r2));
    if (r2 == SND_OK) SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
    SND_CHECK_EQ(snd_physics_material_count(f.phys), 3u);
    SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &c2), SND_OK);
  }
  snd_physics_table_desc wd;
  snd_physics_table_desc_init(&wd);
  wd.weight_to_volume = phystest::Linear({{0.0f, 0.0f}, {1.0f, 1000.5f}});
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &wd), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 3u);
  SND_CHECK_EQ(snd_physics_set_curves(f.phys, 3, &c0), SND_ERR_INVALID_ARGUMENT);
  // The rejected curves left c2 in place.
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 2.5f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.6f, 1e-6);
}

// Approved (decision 6 of the review; were undecided 4 and 5): the curve fallback
// (material -> table default -> built-in) and the lighter object's material / mass for pair
// and default events.
SND_TEST(IndepResolve, CurveFallbackAndLighterObject) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  // Built-in impulse_to_volume (0.1,.25)(1,.5)(10,.75)(100,1): 5.5 -> 0.625.
  SND_CHECK_EQ(f.Submit({Imp(1, 1, 5.5f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.625f, 1e-5);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.velocity, 0.5f, 1e-5);  // 1/3 + 1/3 * 0.5
  snd_physics_curves def{};
  def.impulse_to_volume = phystest::Linear({{0.0f, 0.1f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, SND_PHYSICS_DEFAULT_MATERIAL, &def), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(1, 1, 5.5f)}), SND_OK);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.1f, 1e-6);
  // Pair (0, 2): the lighter object (B, 2 kg) decides material 2's curves and mass.
  snd_physics_curves c2{};
  c2.mass_to_pitch = phystest::Linear({{1.0f, 1.0f}, {10.0f, 10.0f}});  // pitch = mass on [1, 10]
  snd_physics_curves c0{};
  c0.mass_to_pitch = phystest::Linear({{0.0f, 4.0f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 2, &c2), SND_OK);
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &c0), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 2, 1.0f, 8.0f, 2.0f)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.pitch, 2.0f, 1e-6);
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.mass, 2.0f);
  SND_CHECK_EQ(f.Submit({Imp(0, 2, 1.0f, 3.0f, 3.0f)}), SND_OK);  // equal: A (material 0)
  SND_CHECK_NEAR(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.pitch, 4.0f, 1e-6);
  // A NaN mass counts as 0 (lighter) and must not reach the parameters as NaN.
  SND_CHECK_EQ(f.Submit({Imp(0, 2, 1.0f, 8.0f, kNaN)}), SND_OK);
  SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const Rec r = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  SND_CHECK(std::isfinite(r.r.pitch));
  SND_CHECK(std::isfinite(r.r.mass));
  for (const snd_physics_param& p : r.params) SND_CHECK(std::isfinite(p.value));
}

// P27: a replaced table takes effect from the next call.
SND_TEST(IndepResolve, TableReplacedNextCall) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 0)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS0});
  snd_id single[4] = {kS2, SND_ID_NONE, SND_ID_NONE, SND_ID_NONE};
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 1;
  d.single = single;
  SND_REQUIRE_EQ(snd_physics_set_table(f.phys, &d), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(f.phys), 1u);
  SND_CHECK_EQ(f.Submit({Imp(0, 0)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS2});
  SND_CHECK_EQ(f.Submit({Imp(1, 1)}), SND_OK);  // material 1 no longer exists: unknown, no default
  SND_CHECK(f.PlayedEvents().empty());
  // Bad descs leave the table as it is.
  snd_physics_table_desc bad = d;
  bad.struct_size = 4;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &bad), SND_ERR_INVALID_ARGUMENT);
  bad = d;
  bad.impulse_threshold = -1.0f;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &bad), SND_ERR_INVALID_ARGUMENT);
  bad.impulse_threshold = kNaN;
  SND_CHECK_EQ(snd_physics_set_table(f.phys, &bad), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_set_table(f.phys, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(f.Submit({Imp(0, 0)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kS2});
}

// P28: parameter ids replaceable; SND_ID_NONE not passed; defaults are the name hashes.
SND_TEST(IndepResolve, ParameterIds) {
  snd_physics_config c = Cfg(nullptr);
  SND_CHECK_EQ(c.params.impulse, snd_hash_id("Phys/Impulse"));
  SND_CHECK_EQ(c.params.count, snd_hash_id("Phys/Count"));
  c.params.impulse = 0x4242u;
  c.params.count = SND_ID_NONE;
  Fx f(c);
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  SND_CHECK_EQ(f.Submit({Imp(0, 0, 3.0f)}), SND_OK);
  const Rec r = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  SND_CHECK_EQ(r.Param(0x4242u), 3.0f);
  SND_CHECK(!r.Has(snd_hash_id("Phys/Impulse")));
  SND_CHECK(!r.Has(snd_hash_id("Phys/Count")));
  SND_CHECK(!r.Has(SND_ID_NONE));
  SND_CHECK(r.Has(snd_hash_id("Phys/Velocity")));
  // Reserved ids are refused at creation.
  Sys s;
  snd_physics_config bad = Cfg(nullptr);
  bad.params.speed = SND_ID_RESERVED_FIRST;
  snd_phys* p = reinterpret_cast<snd_phys*>(&bad);
  SND_CHECK_EQ(snd_physics_create(s.sys, &bad, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(p == nullptr);
}

// Decision 1 of the review: one material unknown -> the known side's single event (its
// curves, its object's mass, its surface parameters), else the default; both unknown -> the
// default; never the pair. Always fallback_unknown_material + the UNKNOWN_MATERIAL flag; the
// stage statistics count the stage that decided.
SND_TEST(IndepResolve, OneSideUnknownThreeCases) {
  Fx f;
  SND_REQUIRE_EQ(f.created, SND_OK);
  SND_REQUIRE_EQ(ThreeMaterials(f.phys), SND_OK);
  const snd_id W = 0x7101u;
  snd_physics_material_entry e[4] = {};
  e[0].sound_material = 0;
  e[0].param_count = 1;
  e[0].params[0] = snd_physics_param{W, 0.3f};
  e[1].sound_material = SND_PHYSICS_UNMAPPED;
  e[1].param_count = 1;
  e[1].params[0] = snd_physics_param{W, 0.9f};
  e[2].sound_material = 2;
  e[3].sound_material = SND_PHYSICS_UNMAPPED;
  SND_REQUIRE_EQ(snd_physics_set_material_map(f.phys, e, 4), SND_OK);
  snd_physics_curves c0{};
  c0.mass_to_pitch = phystest::Linear({{1.0f, 1.0f}, {10.0f, 10.0f}});
  SND_REQUIRE_EQ(snd_physics_set_curves(f.phys, 0, &c0), SND_OK);
  // (a) known A (game 0 -> 0, 5 kg) with unknown B (game 1, 2 kg): single of 0.
  for (int swapped = 0; swapped < 2; ++swapped) {
    SND_CHECK_EQ(f.Submit({swapped ? Imp(1, 0, 1.0f, 2.0f, 5.0f) : Imp(0, 1, 1.0f, 5.0f, 2.0f)}), SND_OK);
    SND_REQUIRE_EQ(f.rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
    const Rec r = f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
    SND_CHECK_EQ(r.r.event, kS0);
    SND_CHECK_EQ(r.r.stage, SND_PHYSICS_STAGE_SINGLE);
    SND_CHECK(r.r.flags & SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL);
    SND_CHECK_EQ(r.r.mass, 5.0f);
    SND_CHECK_NEAR(r.r.pitch, 5.0f, 1e-6);  // material 0's curve, its own object's mass
    SND_CHECK_EQ(r.Param(W), 0.3f);         // only the known side's surface parameters
  }
  // (b) known material 2 has no single event -> default.
  SND_CHECK_EQ(f.Submit({Imp(2, 1)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  SND_CHECK_EQ(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.stage, SND_PHYSICS_STAGE_DEFAULT);
  SND_CHECK(f.rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.flags & SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL);
  // (c) both unknown -> default.
  SND_CHECK_EQ(f.Submit({Imp(1, 3)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kD});
  // Pair (0, 2) is only for two known materials: game 0 with game 2 still uses it.
  SND_CHECK_EQ(f.Submit({Imp(0, 2)}), SND_OK);
  SND_CHECK(f.PlayedEvents() == std::vector<snd_id>{kPA});
  const snd_physics_stats st = phystest::Stats(f.phys);
  SND_CHECK_EQ(st.fallback_unknown_material, 4u);
  SND_CHECK_EQ(st.fallback_pair_to_single, 2u);
  SND_CHECK_EQ(st.fallback_single_to_default, 2u);
  SND_CHECK_EQ(st.resolved_pair, 1u);
}
