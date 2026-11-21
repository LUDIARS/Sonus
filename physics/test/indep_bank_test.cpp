// Independent tests: the PHYS chunk (bank format 2.0, decision B), snd_phys_create with a
// bank, load_bank_table errors, corrupted chunks, unloading. Properties P06..P09.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/test.hpp"

using namespace indepphys;

namespace {

const snd_id kTable = snd_hash_id("I/Table");
const snd_id kDc = snd_hash_id("I/Dc");
const snd_id kDc2D = snd_hash_id("I/Dc2D");
const snd_id kLoop = snd_hash_id("I/Loop");

std::vector<snd_id> Played(const Recorder& rec, snd_physics_record_kind k = SND_PHYSICS_RECORD_PLAY) {
  std::vector<snd_id> v;
  for (const Rec& r : rec.Of(k)) v.push_back(r.r.event);
  return v;
}

snd_result Submit(Sys& s, Recorder& rec, snd_phys* p, std::vector<snd_contact> cs, float dt = 0.1f) {
  rec.Clear();
  const snd_result r = snd_phys_submit_contacts(p, cs.data(), static_cast<std::uint32_t>(cs.size()), dt);
  s.Advance(1);
  return r;
}

snd_contact C(snd_contact_kind k, std::uint16_t ma, std::uint16_t mb, float impulse, std::uint64_t oa,
              float massA = 1.0f, float massB = 1.0f, float speed = 1.0f) {
  return Contact(k, oa, oa + 1000u, ma, mb, static_cast<float>(oa), 0, 0, impulse, speed, massA, massB);
}

}  // namespace

// P06, P09: snd_phys_create reads the bank's table; resolution, threshold and the
// curve mapping of PHYS (undecided 4).
SND_TEST(IndepBank, CreateFromBankAndMapping) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  snd_phys_desc d;
  snd_phys_desc_init(&d);
  d.bank = bank;
  d.table = kTable;
  d.cooldown_seconds = 0.0f;
  d.budget_per_second = 1000.0f;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_OK);
  Recorder rec;
  SND_REQUIRE_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(p), 3u);
  // Single, pair (min*N+max from the "m2, m0" pair), single of m1, default.
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 1.0f, 1)}), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc});
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 2, 1.0f, 2)}), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc2D});
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_BREAK, 2, 0, 1.0f, 3)}), SND_OK);  // pair is for every kind
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc2D});
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 1, 1, 1.0f, 4)}), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc2D});
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 2, 2, 1.0f, 5)}), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc});
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_BREAK, 1, 1, 1.0f, 6)}), SND_OK);  // nothing
  SND_CHECK(Played(rec).empty());
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_SLIDE, 2, 2, 0.0f, 7)}), SND_OK);  // default slide
  SND_CHECK(Played(rec, SND_PHYSICS_RECORD_START) == std::vector<snd_id>{kLoop});
  // Threshold = smallest t0 = 0.1 (m1 / m2 have the buildtool default curves).
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 0.099f, 8), C(SND_CONTACT_IMPACT, 0, 0, 0.1f, 9)}), SND_OK);
  SND_CHECK_EQ(Played(rec).size(), 1u);
  // m0 curves {0.2, 2, 20, 200}, cents {-1200, 1200}: impulse 2 -> volume 0.5,
  // impulse 11 -> velocity 1/3 + 1/3 * 9/18 = 0.5; mass 1 -> pitch 2^-1, mass 100 -> 2.
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 2.0f, 10, 1.0f, 1.0f)}), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_NEAR(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.volume, 0.5f, 1e-5);
  SND_CHECK_NEAR(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.pitch, 0.5f, 1e-5);
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 11.0f, 11, 100.0f, 100.0f)}), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_NEAR(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.velocity, 0.5f, 1e-5);
  SND_CHECK_NEAR(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.pitch, 2.0f, 1e-4);
  // Footstep of m1 (motion-independent column).
  rec.Clear();
  snd_surface_weight w{};
  w.material = 1;
  w.weight = 1.0f;
  SND_CHECK_EQ(snd_phys_play_footstep(p, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_SPRINT), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc2D});
  snd_phys_destroy(p);
}

// P06, undecided 10: the bank / table pairing of snd_phys_create; the desc fields map.
SND_TEST(IndepBank, CreateDescRules) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  snd_phys_desc d;
  snd_phys_desc_init(&d);
  snd_phys* p = nullptr;
  SND_CHECK_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_OK);  // bank 0, table NONE: empty
  SND_CHECK_EQ(snd_physics_material_count(p), 0u);
  snd_phys_destroy(p);
  p = nullptr;
  d.bank = bank;  // table NONE
  SND_CHECK_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(p == nullptr);
  d.bank.value = 0;
  d.table = kTable;  // no bank
  SND_CHECK_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  d.bank = bank;
  d.table = 0x1234u;  // no such table
  SND_CHECK_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_ERR_NOT_FOUND);
  SND_CHECK(p == nullptr);
  d.bank.value = bank.value ^ 0x00010000u;  // stale / invalid handle
  SND_CHECK(snd_phys_create(s.sys, &d, nullptr, &p) != SND_OK);
  SND_CHECK(p == nullptr);
  SND_CHECK_EQ(snd_phys_create(s.sys, nullptr, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_create(nullptr, &d, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  // The desc's culling fields reach the configuration: budget 10/s, threshold 3,
  // max_tracked_pairs 1.
  snd_phys_desc_init(&d);
  d.budget_per_second = 10.0f;
  d.impulse_threshold = 3.0f;
  d.cooldown_seconds = 0.0f;
  d.max_tracked_pairs = 1;
  SND_REQUIRE_EQ(snd_phys_create(s.sys, &d, nullptr, &p), SND_OK);
  snd_physics_table_desc t;
  snd_physics_table_desc_init(&t);
  snd_id single[4] = {0x1u, 0x2u, 0x3u, 0x4u};
  t.material_count = 1;
  t.single = single;
  SND_REQUIRE_EQ(snd_physics_set_table(p, &t), SND_OK);
  Recorder rec;
  SND_REQUIRE_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 2.9f, 1), C(SND_CONTACT_IMPACT, 0, 0, 3.0f, 2),
                                  C(SND_CONTACT_IMPACT, 0, 0, 4.0f, 3), C(SND_CONTACT_SLIDE, 0, 0, 0, 4),
                                  C(SND_CONTACT_SLIDE, 0, 0, 0, 5)}),
               SND_OK);
  const snd_physics_stats st = phystest::Stats(p);
  SND_CHECK_EQ(st.dropped_threshold, 1u);
  SND_CHECK_EQ(st.group_requests, 1u);
  SND_CHECK_EQ(st.dropped_budget, 1u);
  SND_CHECK_EQ(st.tracking_started, 1u);
  // Decision 3 of the review: the second new key overflows the new-key area (capacity =
  // max_tracked_pairs = 1) -> dropped_work_overflow, not tracking_dropped.
  SND_CHECK_EQ(st.dropped_work_overflow, 1u);
  SND_CHECK_EQ(st.tracking_dropped, 0u);
  snd_phys_destroy(p);
}

// P07: load_bank_table errors leave the table unchanged.
SND_TEST(IndepBank, LoadBankTableErrors) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  // A bank without PHYS.
  snd::buildtool::BankWriter w;
  snd::buildtool::BankHeaderInfo h;
  h.bankId = 0x4444u;
  snd_bank_handle plain{};
  SND_REQUIRE(s.LoadBank("plain.sndb", w.Finish(h), &plain));
  snd_physics_config c = Cfg(nullptr);
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  SND_CHECK_EQ(snd_physics_load_bank_table(p, bank, kTable), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(p), 3u);
  SND_CHECK_EQ(snd_physics_load_bank_table(p, bank, SND_ID_NONE), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_load_bank_table(p, bank, 0x9999u), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(snd_physics_load_bank_table(p, plain, kTable), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(snd_physics_load_bank_table(p, snd_bank_handle{0}, kTable), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_physics_load_bank_table(nullptr, bank, kTable), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_material_count(p), 3u);
  // Unload: the table was copied, so contacts still resolve (ASan: no use of the chunk).
  SND_REQUIRE_EQ(snd_unload_bank(s.sys, bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 400 && snd_get_bank_state(s.sys, bank) != SND_BANK_STATE_UNLOADED; ++i) s.Advance(1);
  SND_CHECK_EQ(snd_get_bank_state(s.sys, bank), SND_BANK_STATE_UNLOADED);
  SND_CHECK(snd_physics_load_bank_table(p, bank, kTable) != SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(p), 3u);
  Recorder rec;
  SND_REQUIRE_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 1.0f, 1), C(SND_CONTACT_SLIDE, 0, 0, 0, 2)}), SND_OK);
  SND_CHECK(Played(rec) == std::vector<snd_id>{kDc});
  SND_CHECK(Played(rec, SND_PHYSICS_RECORD_START) == std::vector<snd_id>{kLoop});
  snd_physics_destroy(p);
}

// P08: hand-made PHYS chunks that pass the bank checker but carry bad values.
SND_TEST(IndepBank, CorruptValuesInPhys) {
  Sys s;
  snd_physics_config c = Cfg(nullptr);
  c.max_materials = 4;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  struct Case {
    const char* what;
    void (*edit)(PhysBuilder&);
    snd_result want;
  };
  const Case cases[] = {
      {"valid", [](PhysBuilder&) {}, SND_OK},
      {"NaN threshold", [](PhysBuilder& b) { b.curves[1].impulseToVelocity[2] = kNaN; }, SND_ERR_CORRUPT},
      {"inf cents", [](PhysBuilder& b) { b.curves[0].massToPitchCents[0] = kInf; }, SND_ERR_CORRUPT},
      {"NaN dB", [](PhysBuilder& b) { b.curves[0].speedToVolumeDb[1] = kNaN; }, SND_ERR_CORRUPT},
      {"decreasing thresholds", [](PhysBuilder& b) { b.curves[0].impulseToVelocity[1] = 0.01f; }, SND_ERR_CORRUPT},
      {"5 materials > 4", [](PhysBuilder& b) { b = PhysBuilder(5); }, SND_ERR_LIMIT_REACHED},
      // Ranges (decision 5 of the review): |cents| <= 4800, |dB| <= 200.
      {"cents +-4800", [](PhysBuilder& b) { b.curves[0].massToPitchCents[0] = -4800.0f; b.curves[1].massToPitchCents[1] = 4800.0f; }, SND_OK},
      {"cents 4800.5", [](PhysBuilder& b) { b.curves[1].massToPitchCents[1] = 4800.5f; }, SND_ERR_CORRUPT},
      {"cents -4801", [](PhysBuilder& b) { b.curves[0].massToPitchCents[0] = -4801.0f; }, SND_ERR_CORRUPT},
      {"dB +-200", [](PhysBuilder& b) { b.curves[0].speedToVolumeDb[0] = -200.0f; b.curves[0].speedToVolumeDb[1] = 200.0f; }, SND_OK},
      {"dB 200.1", [](PhysBuilder& b) { b.curves[1].speedToVolumeDb[1] = 200.1f; }, SND_ERR_CORRUPT},
      {"dB -201", [](PhysBuilder& b) { b.curves[1].speedToVolumeDb[0] = -201.0f; }, SND_ERR_CORRUPT},
  };
  std::uint32_t id = 0x5000u;
  for (const Case& k : cases) {
    PhysBuilder b(2);
    b.single[0] = 0xAAu;
    k.edit(b);
    snd_bank_handle bank{};
    ++id;
    const bool loaded = s.LoadBank(std::string("c") + std::to_string(id) + ".sndb", PhysOnlyBank(b.Build(), id), &bank);
    SND_CHECK_MSG(loaded, "%s: bank did not load", k.what);
    if (!loaded) continue;
    const snd_result r = snd_physics_load_bank_table(p, bank, b.id);
    SND_CHECK_MSG(r == k.want, "%s: %d, want %d", k.what, static_cast<int>(r), static_cast<int>(k.want));
    snd_unload_bank(s.sys, bank, SND_UNLOAD_STOP_AND_UNLOAD);
    s.Advance(2);
  }
  snd_physics_destroy(p);
}

// P08: extreme but finite curve values in PHYS are refused (decision 5 of the review); at
// the limits of the ranges every value the module produces stays finite and in range.
SND_TEST(IndepBank, ExtremeFiniteCurves) {
  Sys s;
  snd_physics_config c = Cfg(nullptr);
  c.cooldown_seconds = 0.0f;
  Recorder rec;
  c.observer_user = &rec;
  c.observer = &Recorder::Fn;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  PhysBuilder b(1);
  b.single[SND_CONTACT_IMPACT] = 0xAAu;
  b.single[SND_CONTACT_SLIDE] = 0xBBu;
  b.curves[0].massToPitchCents[0] = 1.0e30f;
  b.curves[0].massToPitchCents[1] = -1.0e30f;
  b.curves[0].speedToVolumeDb[0] = -3.0e38f;
  b.curves[0].speedToVolumeDb[1] = 3.0e38f;
  {
    snd_bank_handle big{};
    SND_REQUIRE(s.LoadBank("big.sndb", PhysOnlyBank(b.Build(), 0x6000u), &big));
    SND_CHECK_EQ(snd_physics_load_bank_table(p, big, b.id), SND_ERR_CORRUPT);
  }
  b.curves[0].massToPitchCents[0] = 4800.0f;
  b.curves[0].massToPitchCents[1] = -4800.0f;
  b.curves[0].speedToVolumeDb[0] = -200.0f;
  b.curves[0].speedToVolumeDb[1] = 200.0f;
  b.curves[0].impulseToVelocity[0] = 0.0f;
  b.curves[0].impulseToVelocity[1] = 0.0f;
  b.curves[0].impulseToVelocity[2] = 0.0f;
  b.curves[0].impulseToVelocity[3] = 3.0e38f;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("x.sndb", PhysOnlyBank(b.Build(), 0x6001u), &bank));
  const snd_result r = snd_physics_load_bank_table(p, bank, b.id);
  SND_CHECK_EQ(r, SND_OK);
  if (r == SND_OK) {
    SND_CHECK_EQ(Submit(s, rec, p, {C(SND_CONTACT_IMPACT, 0, 0, 1.0f, 1, 1.0f, 1.0f), C(SND_CONTACT_IMPACT, 0, 0, 1.0f, 2, 50.0f, 50.0f),
                                    C(SND_CONTACT_SLIDE, 0, 0, 0, 3, 1, 1, 0.05f), C(SND_CONTACT_SLIDE, 0, 0, 0, 4, 1, 1, 50.0f),
                                    C(SND_CONTACT_SLIDE, 0, 0, 0, 5, 1, 1, 1.0f)}),
                 SND_OK);
    SND_CHECK(!rec.recs.empty());
    for (const Rec& x : rec.recs) {
      SND_CHECK(std::isfinite(x.r.volume) && x.r.volume >= 0.0f && x.r.volume <= 1000.0f);
      SND_CHECK(std::isfinite(x.r.pitch) && x.r.pitch >= 1.0f / 16.0f && x.r.pitch <= 16.0f);
      SND_CHECK(std::isfinite(x.r.velocity));
      for (const snd_physics_param& q : x.params) SND_CHECK(std::isfinite(q.value));
    }
  }
  snd_physics_destroy(p);
}
