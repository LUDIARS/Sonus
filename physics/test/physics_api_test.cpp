// Entry rules of the physics C API and the C++ wrapper: config struct_size / version and value
// ranges, snd_phys_desc mapping, the physics thread (WRONG_THREAD), observer re-entry,
// statistics copy and reset, invalid submit / play_at input (nothing done, the clock does not
// move), held one-shot emitters (hold time, release order, capacity), allocation once.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "phys_rig.hpp"
#include "snd/ext/allocator.hpp"

using namespace phystest;

namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

snd_result CreateWith(snd_system* sys, const snd_physics_config& c) {
  snd_phys* p = reinterpret_cast<snd_phys*>(0x1);
  const snd_result r = snd_physics_create(sys, &c, nullptr, &p);
  if (r == SND_OK) {
    snd_phys_destroy(p);
  } else if (p != nullptr) {
    return SND_ERR_UNSUPPORTED;  // *out must be NULL on error
  }
  return r;
}

}  // namespace

SND_TEST(PhysicsApi, ConfigVersionsAndRanges) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  snd_physics_config c;
  snd_physics_config_init(&c);
  SND_CHECK_EQ(CreateWith(sys.sys, c), SND_OK);
  SND_CHECK_EQ(snd_physics_create(nullptr, &c, nullptr, nullptr), SND_ERR_INVALID_ARGUMENT);
  snd_phys* p = nullptr;
  SND_CHECK_EQ(snd_physics_create(sys.sys, nullptr, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_create(nullptr, &c, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  // An older header: a prefix; the fields past it take their defaults (here a bad grid size
  // past the prefix is ignored).
  snd_physics_config old = c;
  old.struct_size = static_cast<std::uint32_t>(offsetof(snd_physics_config, grid_size));
  old.grid_size = -1.0f;
  old.budget_per_second = 60.0f;
  Recorder rec;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &old, nullptr, &p), SND_OK);
  SND_CHECK_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  std::vector<snd_contact> many;
  for (int i = 0; i < 10; ++i) many.push_back(Impact(static_cast<std::uint64_t>(i) * 2u + 1u, 0, static_cast<float>(i), 1.0f));
  SND_CHECK_EQ(snd_phys_submit_contacts(p, many.data(), 10, 0.1f), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 6u);  // 60 / s: the covered field counts
  snd_phys_destroy(p);
  // Bad headers.
  for (std::uint32_t size : {0u, 4u, 6u, 10u}) {
    snd_physics_config b = c;
    b.struct_size = size;
    SND_CHECK_MSG(CreateWith(sys.sys, b) == SND_ERR_INVALID_ARGUMENT, "struct_size %u", size);
  }
  for (std::uint32_t v : {0u, SND_PHYSICS_CONFIG_VERSION + 1u}) {
    snd_physics_config b = c;
    b.version = v;
    SND_CHECK_EQ(CreateWith(sys.sys, b), SND_ERR_INVALID_ARGUMENT);
  }
  // A newer header (larger struct_size, the same version): the known part is read, the rest ignored.
  {
    alignas(16) unsigned char buf[sizeof(snd_physics_config) + 32];
    std::memset(buf, 0xFF, sizeof buf);
    snd_physics_config b = c;
    b.struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_config) + 32u);
    std::memcpy(buf, &b, sizeof b);
    snd_phys* q = nullptr;
    SND_CHECK_EQ(snd_physics_create(sys.sys, reinterpret_cast<const snd_physics_config*>(buf), nullptr, &q), SND_OK);
    alignas(16) unsigned char tb[sizeof(snd_physics_table_desc) + 16];
    std::memset(tb, 0xFF, sizeof tb);
    std::vector<snd_id> s1;
    snd_physics_table_desc td = SinglesTable(3, &s1);
    td.struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_table_desc) + 16u);
    std::memcpy(tb, &td, sizeof td);
    SND_CHECK_EQ(snd_physics_set_table(q, reinterpret_cast<const snd_physics_table_desc*>(tb)), SND_OK);
    SND_CHECK_EQ(snd_physics_material_count(q), 3u);
    snd_phys_destroy(q);
  }
  // Values.
  struct Bad {
    const char* what;
    void (*set)(snd_physics_config*);
  };
  const Bad bad[] = {
      {"budget NaN", [](snd_physics_config* x) { x->budget_per_second = kNan; }},
      {"budget < 0", [](snd_physics_config* x) { x->budget_per_second = -1.0f; }},
      {"grid 0", [](snd_physics_config* x) { x->grid_size = 0.0f; }},
      {"grid inf", [](snd_physics_config* x) { x->grid_size = kInf; }},
      {"cooldown < 0", [](snd_physics_config* x) { x->cooldown_seconds = -0.1f; }},
      {"threshold < 0", [](snd_physics_config* x) { x->impulse_threshold = -1.0f; }},
      {"slide speed NaN", [](snd_physics_config* x) { x->min_slide_speed = kNan; }},
      {"timeout inf", [](snd_physics_config* x) { x->tracking_timeout_seconds = kInf; }},
      {"fade < 0", [](snd_physics_config* x) { x->tracking_fade_seconds = -1.0f; }},
      {"max dt 0", [](snd_physics_config* x) { x->max_dt_seconds = 0.0f; }},
      {"tracked 0", [](snd_physics_config* x) { x->max_tracked_pairs = 0; }},
      {"cooldowns 0", [](snd_physics_config* x) { x->max_cooldowns = 0; }},
      {"groups too many", [](snd_physics_config* x) { x->max_groups_per_call = SND_PHYSICS_MAX_CAPACITY + 1u; }},
      {"materials 0", [](snd_physics_config* x) { x->max_materials = 0; }},
      {"materials too many", [](snd_physics_config* x) { x->max_materials = SND_PHYSICS_MAX_MATERIALS_LIMIT + 1u; }},
      {"game materials 0", [](snd_physics_config* x) { x->max_game_materials = 0; }},
      {"held 0", [](snd_physics_config* x) { x->max_held_emitters = 0; }},
      {"reserved id", [](snd_physics_config* x) { x->params.speed = SND_ID_RESERVED_FIRST; }},
  };
  for (const Bad& b : bad) {
    snd_physics_config x = c;
    b.set(&x);
    SND_CHECK_MSG(CreateWith(sys.sys, x) == SND_ERR_INVALID_ARGUMENT, "%s", b.what);
  }
  // Edge values that are fine.
  snd_physics_config ok = c;
  ok.budget_per_second = 0.0f;
  ok.cooldown_seconds = 0.0f;
  ok.tracking_timeout_seconds = 0.0f;
  ok.max_materials = SND_PHYSICS_MAX_MATERIALS_LIMIT;
  ok.max_game_materials = 0x10000u;
  SND_CHECK_EQ(CreateWith(sys.sys, ok), SND_OK);
}

SND_TEST(PhysicsApi, DescMapsOntoTheConfig) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  snd_phys_desc desc;
  snd_phys_desc_init(&desc);
  desc.impulse_threshold = 2.0f;
  desc.grid_size = 10.0f;
  desc.cooldown_seconds = 0.0f;
  desc.budget_per_second = 30.0f;  // 3 at 0.1 s
  desc.tracking_timeout_seconds = 0.05f;
  desc.max_tracked_pairs = 1;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_phys_create(sys.sys, &desc, nullptr, &p), SND_OK);
  Recorder rec;
  SND_CHECK_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  // Threshold 2; grid 10 (x 0..9.9 is one cell); budget 3.
  std::vector<snd_contact> c = {Impact(1, 2, 0.0f, 1.9f), Impact(3, 4, 9.9f, 2.0f), Impact(5, 6, 10.0f, 3.0f),
                                Impact(7, 8, 25.0f, 4.0f), Impact(9, 10, 35.0f, 5.0f), Impact(11, 12, 45.0f, 6.0f)};
  SND_CHECK_EQ(snd_phys_submit_contacts(p, c.data(), static_cast<std::uint32_t>(c.size()), 0.1f), SND_OK);
  snd_physics_stats s = Stats(p);
  SND_CHECK_EQ(s.dropped_threshold, 1u);
  SND_CHECK_EQ(s.groups, 5u);
  SND_CHECK_EQ(s.group_requests, 3u);
  // One tracked pair; timeout 50 ms.
  const snd_contact s1 = Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, 0, 0, 0, 0, 1.0f);
  const snd_contact s2 = Contact(SND_CONTACT_SLIDE, 3, 4, 0, 0, 0, 0, 0, 0, 1.0f);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &s1, 1, 0.01f), SND_OK);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &s2, 1, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(p).tracking_evictions, 1u);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 0.05f), SND_OK);
  SND_CHECK_EQ(Stats(p).tracking_timeouts, 1u);
  snd_phys_destroy(p);
  snd_phys_destroy(nullptr);
  snd_physics_destroy(nullptr);
  SND_CHECK_EQ(snd_phys_create(sys.sys, nullptr, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_create(sys.sys, &desc, nullptr, nullptr), SND_ERR_INVALID_ARGUMENT);
  desc.grid_size = 0.0f;
  SND_CHECK_EQ(snd_phys_create(sys.sys, &desc, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(PhysicsApi, OnlyThePhysicsThread) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  CountingAllocator alloc;
  const snd_allocator at = alloc.Table();
  snd_physics_config c = OpenConfig(nullptr);
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, &at, &p), SND_OK);
  std::vector<snd_result> r;
  std::uint32_t count = 99;
  std::thread t([&] {
    const snd_contact x = Impact(1, 2, 0, 1.0f);
    const snd_surface_weight w{0, 0, 1.0f};
    const snd_vec3 pos{0, 0, 0};
    snd_physics_table_desc d;
    snd_physics_table_desc_init(&d);
    snd_physics_material_entry e{};
    snd_physics_stats st{};
    st.struct_size = sizeof(st);
    r.push_back(snd_phys_submit_contacts(p, &x, 1, 0.01f));
    r.push_back(snd_phys_play_footstep(p, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK));
    r.push_back(snd_physics_play_at(p, 1u, &pos));
    r.push_back(snd_physics_set_observer(p, nullptr, nullptr));
    r.push_back(snd_physics_set_table(p, &d));
    r.push_back(snd_physics_load_bank_table(p, snd_bank_handle{1}, 1u));
    r.push_back(snd_physics_set_single_event(p, 0, 0, 1u));
    r.push_back(snd_physics_set_pair_event(p, 0, 0, 0, 1u));
    r.push_back(snd_physics_set_default_event(p, 0, 1u));
    r.push_back(snd_physics_set_footstep_event(p, 0, 0, 1u));
    r.push_back(snd_physics_set_curves(p, 0, nullptr));
    r.push_back(snd_physics_set_material_map(p, nullptr, 0));
    r.push_back(snd_physics_set_material_entry(p, 0, &e));
    r.push_back(snd_physics_set_surface_param(p, 0, 1u, 1.0f));
    r.push_back(snd_physics_get_stats(p, &st));
    r.push_back(snd_physics_reset_stats(p));
    count = snd_physics_material_count(p);
  });
  t.join();
  SND_CHECK_EQ(r.size(), 16u);
  for (std::size_t i = 0; i < r.size(); ++i) SND_CHECK_MSG(r[i] == SND_ERR_WRONG_THREAD, "call %u: %d", static_cast<unsigned>(i), r[i]);
  SND_CHECK_EQ(count, 0u);
  SND_CHECK_EQ(Stats(p).contacts_received, 0u);
  snd_phys_destroy(p);
  SND_CHECK_EQ(alloc.allocs, 1u);
  SND_CHECK_EQ(alloc.frees, 1u);
  // Destroyed on another thread: nothing happens; the object still works and is destroyed on
  // its own thread.
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, &at, &p), SND_OK);
  std::thread t2([&] { snd_phys_destroy(p); });
  t2.join();
  SND_CHECK_EQ(alloc.frees, 1u);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 0.01f), SND_OK);
  snd_phys_destroy(p);
  SND_CHECK_EQ(alloc.frees, 2u);
}

namespace {
struct Reentry {
  snd_phys* phys = nullptr;
  std::vector<snd_result> results;
  static void SND_CALL Fn(void* u, const snd_physics_record*) {
    Reentry* self = static_cast<Reentry*>(u);
    const snd_contact x = Impact(9, 9, 0, 1.0f);
    self->results.push_back(snd_phys_submit_contacts(self->phys, &x, 1, 0.01f));
    snd_physics_stats st{};
    st.struct_size = sizeof(st);
    self->results.push_back(snd_physics_get_stats(self->phys, &st));
  }
};
}  // namespace

SND_TEST(PhysicsApi, ObserverCannotCallBack) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  Reentry re;
  snd_physics_config c = OpenConfig(nullptr);
  c.observer = &Reentry::Fn;
  c.observer_user = &re;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, nullptr, &re.phys), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(re.phys, &d), SND_OK);
  const snd_contact x = Impact(1, 2, 0, 1.0f);
  SND_CHECK_EQ(snd_phys_submit_contacts(re.phys, &x, 1, 0.01f), SND_OK);
  SND_REQUIRE_EQ(re.results.size(), 4u);  // FALLBACK (single stage) and PLAY records
  for (snd_result r : re.results) SND_CHECK_EQ(r, SND_ERR_UNSUPPORTED);
  SND_CHECK_EQ(Stats(re.phys).contacts_received, 1u);
  snd_phys_destroy(re.phys);
}

SND_TEST(PhysicsApi, StatsCopyResetAndInvalidInput) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  Recorder rec;
  snd_physics_config c = OpenConfig(&rec);
  c.tracking_timeout_seconds = 0.1f;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, nullptr, &p), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  const snd_contact slide = Contact(SND_CONTACT_SLIDE, 1, 2, 0, 0, 0, 0, 0, 0, 1.0f);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &slide, 1, 0.0f), SND_OK);
  // Refused calls do nothing: no time passes (the slide does not time out), nothing is counted
  // but calls_rejected.
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, kNan), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, -0.5f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, kInf), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 3, 0.5f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_phys_submit_contacts(nullptr, &slide, 1, 0.5f), SND_ERR_INVALID_ARGUMENT);
  snd_physics_stats s = Stats(p);
  SND_CHECK_EQ(s.calls_rejected, 4u);
  SND_CHECK_EQ(s.calls_submit, 1u);
  SND_CHECK_EQ(s.tracking_timeouts, 0u);
  SND_CHECK_EQ(s.contacts_received, 1u);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 0.1f), SND_OK);  // count 0: time moves, it times out
  SND_CHECK_EQ(Stats(p).tracking_timeouts, 1u);
  // play_at input.
  const snd_vec3 pos{1, 2, 3};
  const snd_vec3 nanPos{kNan, 0, 0};
  SND_CHECK_EQ(snd_physics_play_at(p, SND_ID_NONE, &pos), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(p, 5u, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(p, 5u, &nanPos), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(nullptr, 5u, &pos), SND_ERR_INVALID_ARGUMENT);
  rec.Clear();
  SND_CHECK_EQ(snd_physics_play_at(p, 5u, &pos), SND_OK);
  SND_REQUIRE_EQ(rec.recs.size(), 1u);
  SND_CHECK_EQ(rec.recs[0].r.kind, SND_PHYSICS_RECORD_PLAY);
  SND_CHECK_EQ(rec.recs[0].r.stage, SND_PHYSICS_STAGE_DIRECT);
  SND_CHECK_EQ(rec.recs[0].r.position.z, 3.0f);
  SND_CHECK((rec.recs[0].r.flags & SND_PHYSICS_RECORD_FLAG_POSITION_ONLY) != 0u);
  s = Stats(p);
  SND_CHECK_EQ(s.requested_play_at, 1u);
  SND_CHECK_EQ(s.calls_rejected, 7u);
  // A smaller stats struct gets a prefix; struct_size stays the caller's.
  snd_physics_stats small;
  std::memset(&small, 0xAB, sizeof(small));
  small.struct_size = static_cast<std::uint32_t>(offsetof(snd_physics_stats, contacts_received) + 8u);
  SND_CHECK_EQ(snd_physics_get_stats(p, &small), SND_OK);
  SND_CHECK_EQ(small.struct_size, static_cast<std::uint32_t>(offsetof(snd_physics_stats, contacts_received) + 8u));
  SND_CHECK_EQ(small.contacts_received, 1u);
  std::uint64_t untouched;
  std::memset(&untouched, 0xAB, sizeof(untouched));
  SND_CHECK_EQ(small.dropped_invalid, untouched);
  small.struct_size = 4;
  SND_CHECK_EQ(snd_physics_get_stats(p, &small), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_get_stats(p, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_reset_stats(p), SND_OK);
  s = Stats(p);
  SND_CHECK_EQ(s.contacts_received, 0u);
  SND_CHECK_EQ(s.calls_rejected, 0u);
  snd_phys_destroy(p);
}

SND_TEST(PhysicsApi, HeldEmittersReleaseAndCapacity) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  sys.Advance(4);
  Recorder rec;
  snd_physics_config c = OpenConfig(&rec);
  c.max_held_emitters = 2;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, nullptr, &p), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  auto submit = [&](int n, float dt) {
    std::vector<snd_contact> v;
    for (int i = 0; i < n; ++i) v.push_back(Impact(static_cast<std::uint64_t>(i) * 2u + 1u, 0, static_cast<float>(i) * 10.0f, 1.0f));
    rec.Clear();
    SND_CHECK_EQ(snd_phys_submit_contacts(p, v.empty() ? nullptr : v.data(), static_cast<std::uint32_t>(n), dt), SND_OK);
  };
  // 3 groups in one call with room for 2: the third is dropped.
  submit(3, 0.01f);
  snd_physics_stats s = Stats(p);
  SND_CHECK_EQ(s.dropped_no_emitter, 1u);
  const std::vector<Rec> first = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(first.size(), 2u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
  // Not applied yet (no update): however much physics time passes they stay held, and a new
  // group in a later call is dropped too (a held emitter is never destroyed early).
  submit(0, 1.0f);
  submit(0, 1.0f);
  submit(1, 0.01f);
  s = Stats(p);
  SND_CHECK_EQ(s.dropped_no_emitter, 2u);
  SND_CHECK_EQ(s.group_requests, 2u);
  SND_CHECK_EQ(s.groups, s.dropped_no_event + s.dropped_cooldown + s.dropped_budget + s.dropped_no_emitter + s.group_requests);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_DROP)[0].r.result, SND_ERR_LIMIT_REACHED);
  // Both emitters are alive until the plays are seen applied.
  sys.Advance(8);
  SND_CHECK_EQ(snd_set_transform(sys.sys, first[0].r.emitter, nullptr, nullptr, nullptr), SND_OK);
  SND_CHECK_EQ(snd_set_transform(sys.sys, first[1].r.emitter, nullptr, nullptr, nullptr), SND_OK);
  // Applied (the system advanced): the next call releases both, even with dt 0, and new
  // groups get emitters again.
  submit(0, 0.0f);
  sys.Advance(8);
  SND_CHECK_EQ(snd_set_transform(sys.sys, first[0].r.emitter, nullptr, nullptr, nullptr), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_set_transform(sys.sys, first[1].r.emitter, nullptr, nullptr, nullptr), SND_ERR_INVALID_HANDLE);
  submit(2, 0.01f);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 2u);
  SND_CHECK_EQ(Stats(p).dropped_no_emitter, 2u);
  snd_phys_destroy(p);
}

SND_TEST(PhysicsApi, CppWrapper) {
  static_assert(static_cast<std::uint32_t>(snd::phys::Stage::FootDefault) == SND_PHYSICS_STAGE_FOOT_DEFAULT);
  static_assert(static_cast<std::uint32_t>(snd::phys::RecordKind::Fallback) == SND_PHYSICS_RECORD_FALLBACK);
  static_assert(static_cast<std::uint32_t>(snd::phys::StopReason::Ended) == SND_PHYSICS_STOP_ENDED);
  static_assert(static_cast<std::uint32_t>(snd::phys::Source::PlayAt) == SND_PHYSICS_SOURCE_PLAY_AT);
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  CountingAllocator counting;
  snd_allocator table = counting.Table();
  snd::CAllocator alloc(table);
  snd::phys::Config cfg = snd::phys::DefaultConfig();
  cfg.cooldown_seconds = 0.0f;
  snd::phys::PhysicsAudio* p = nullptr;
  SND_REQUIRE(snd::phys::Create(sys.sys, cfg, &alloc, &p) == snd::Result::Ok);
  SND_CHECK_EQ(counting.allocs, 1u);
  Recorder rec;
  SND_CHECK(snd::phys::SetObserver(p, &Recorder::Fn, &rec) == snd::Result::Ok);
  snd::phys::TableDesc d = snd::phys::EmptyTable();
  d.material_count = 2;
  SND_CHECK(snd::phys::SetTable(p, d) == snd::Result::Ok);
  SND_CHECK_EQ(snd::phys::MaterialCount(p), 2u);
  SND_CHECK(snd::phys::SetSingleEvent(p, 1, snd::phys::ContactKind::Break, 0x77u) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetPairEvent(p, 0, 1, snd::phys::kAllKinds, 0x88u) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetDefaultEvent(p, snd::phys::ContactKind::Impact, 0x99u) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetFootstepEvent(p, 0, snd::phys::kAnyMotion, 0x55u) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetCurves(p, snd::phys::kDefaultMaterial, nullptr) == snd::Result::Ok);
  snd::phys::MaterialEntry e{};
  e.sound_material = 1;
  SND_CHECK(snd::phys::SetMaterialEntry(p, 3, e) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetSurfaceParam(p, 3, 7u, 0.5f) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetMaterialMap(p, nullptr, 0) == snd::Result::Ok);
  const snd::phys::Contact c[2] = {Contact(SND_CONTACT_BREAK, 1, 2, 1, 1, 0, 0, 0, 1.0f),
                                   Contact(SND_CONTACT_IMPACT, 1, 2, 0, 1, 50, 0, 0, 1.0f)};
  SND_CHECK(snd::phys::SubmitContacts(p, c, 2, 0.05f) == snd::Result::Ok);
  SND_CHECK(snd::phys::PlayAt(p, 0x66u, snd::Vec3{0, 0, 0}) == snd::Result::Ok);
  const std::vector<Rec> plays = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(plays.size(), 3u);
  SND_CHECK_EQ(plays[0].r.event, 0x77u);
  SND_CHECK_EQ(plays[1].r.event, 0x88u);
  SND_CHECK_EQ(plays[2].r.event, 0x66u);
  snd::phys::Stats st{};
  SND_CHECK(snd::phys::GetStats(p, &st) == snd::Result::Ok);
  SND_CHECK_EQ(st.requested_break, 1u);
  SND_CHECK(snd::phys::ResetStats(p) == snd::Result::Ok);
  snd::phys::Destroy(p);
  SND_CHECK_EQ(counting.frees, 1u);
}

// D-125: the audio side counts as stalled after stall_calls calls in a row with the same
// block_index (63 sub-steps with the default 64 are not a stall); while stalled, Impact /
// Break groups are dropped (dropped_audio_stalled), nothing is recorded; a moved
// block_index ends it.
SND_TEST(PhysicsApi, AudioStallDetectionBoundaryAndDrops) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  sys.Advance(4);
  Recorder rec;
  snd_physics_config c = OpenConfig(&rec);
  c.max_held_emitters = 256;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, nullptr, &p), SND_OK);
  SND_CHECK_EQ(c.stall_calls, 64u);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  std::uint64_t obj = 1;
  auto hit = [&] {
    const snd_contact x = Impact(obj, obj + 1, 0.0f, 1.0f);
    obj += 2;
    rec.Clear();
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 1.0f / 240.0f), SND_OK);
  };
  // Call 1 reads the block; calls 2..64 see it unchanged 1..63 times: not stalled.
  for (int i = 0; i < 64; ++i) hit();
  snd_physics_stats s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 0u);
  SND_CHECK_EQ(s.dropped_audio_stalled, 0u);
  SND_CHECK_EQ(s.group_requests, 64u);
  // Call 65: the 64th unchanged in a row: stalled.
  hit();
  s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 1u);
  SND_CHECK_EQ(s.dropped_audio_stalled, 1u);
  SND_CHECK_EQ(s.group_requests, 64u);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_DROP), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_DROP)[0].r.result, SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 0u);
  for (int i = 0; i < 10; ++i) hit();
  s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 11u);
  SND_CHECK_EQ(s.dropped_audio_stalled, 11u);
  SND_CHECK_EQ(s.groups, s.dropped_no_event + s.dropped_cooldown + s.dropped_budget + s.dropped_audio_stalled +
                             s.dropped_no_emitter + s.group_requests);
  // Footsteps and play_at still record while stalled.
  const snd_vec3 pos{0, 0, 0};
  SND_CHECK_EQ(snd_physics_play_at(p, 5u, &pos), SND_OK);
  SND_CHECK_EQ(Stats(p).requested_play_at, 1u);
  // The system moves: the stall ends at the next call and hits play again.
  SND_REQUIRE(sys.AdvanceToNewBlock());
  hit();
  s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 11u);
  SND_CHECK_EQ(s.group_requests, 65u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  // stall_calls 0: never stalled.
  snd_physics_config c0 = OpenConfig(nullptr);
  c0.stall_calls = 0;
  c0.max_held_emitters = 256;
  snd_phys* q = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c0, nullptr, &q), SND_OK);
  SND_REQUIRE_EQ(snd_physics_set_table(q, &d), SND_OK);
  for (int i = 0; i < 100; ++i) SND_CHECK_EQ(snd_phys_submit_contacts(q, nullptr, 0, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(q).audio_stalled_calls, 0u);
  snd_phys_destroy(q);
  // An older config without the field gets the default 64.
  snd_physics_config old = OpenConfig(nullptr);
  old.struct_size = static_cast<std::uint32_t>(offsetof(snd_physics_config, stall_calls));
  old.stall_calls = 1;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &old, nullptr, &q), SND_OK);
  for (int i = 0; i < 64; ++i) SND_CHECK_EQ(snd_phys_submit_contacts(q, nullptr, 0, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(q).audio_stalled_calls, 0u);
  SND_CHECK_EQ(snd_phys_submit_contacts(q, nullptr, 0, 0.01f), SND_OK);
  SND_CHECK_EQ(Stats(q).audio_stalled_calls, 1u);
  snd_phys_destroy(q);
  snd_phys_destroy(p);
}

// D-125 for sliding contacts: while stalled the reports update the entries without
// recording, new keys are dropped, a timeout waits for the first call after the stall; then
// the latest values are recorded.
SND_TEST(PhysicsApi, AudioStallHoldsSlidesAndCatchesUp) {
  Sys sys;
  SND_REQUIRE_EQ(sys.created, SND_OK);
  sys.Advance(4);
  Recorder rec;
  snd_physics_config c = OpenConfig(&rec);
  c.stall_calls = 4;
  c.tracking_timeout_seconds = 0.05f;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(sys.sys, &c, nullptr, &p), SND_OK);
  std::vector<snd_id> single;
  snd_physics_table_desc d = SinglesTable(1, &single);
  std::vector<snd_physics_curves> cv(1);
  std::memset(cv.data(), 0, sizeof(snd_physics_curves));
  cv[0].speed_to_volume = Linear({{0.0f, 0.0f}, {10.0f, 1.0f}});
  d.curves = cv.data();
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  auto slide = [](std::uint64_t a, float speed, float x) {
    return Contact(SND_CONTACT_SLIDE, a, a + 1, 0, 0, x, 0, 0, 0.0f, speed);
  };
  // Call 1: A and B start. Calls 2..4 report A only (3 unchanged blocks); call 5 is stalled.
  SND_CHECK_EQ(snd_phys_submit_contacts(p, std::vector<snd_contact>{slide(1, 1.0f, 0), slide(3, 1.0f, 0)}.data(), 2, 0.02f),
               SND_OK);
  for (int i = 0; i < 3; ++i) {
    const snd_contact a = slide(1, 1.0f, 0);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.02f), SND_OK);
  }
  SND_CHECK_EQ(Stats(p).tracking_timeouts, 1u);  // B: 60 ms without a report (not stalled yet)
  // Restart B and let it go silent during the stall.
  {
    const snd_contact b = slide(3, 1.0f, 0);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &b, 1, 0.02f), SND_OK);  // stalled: B is new -> dropped
  }
  snd_physics_stats s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 1u);
  SND_CHECK_EQ(s.tracking_dropped, 1u);
  rec.Clear();
  for (int i = 0; i < 10; ++i) {
    const snd_contact a = slide(1, 5.0f + static_cast<float>(i), 1.0f + static_cast<float>(i));
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.02f), SND_OK);
  }
  s = Stats(p);
  SND_CHECK_EQ(s.audio_stalled_calls, 11u);
  SND_CHECK_EQ(rec.recs.size(), 0u);  // nothing recorded, nothing observed
  // Stalled: A not reported for 100 ms -> no timeout yet.
  for (int i = 0; i < 5; ++i) SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 0.02f), SND_OK);
  SND_CHECK_EQ(Stats(p).tracking_timeouts, 1u);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 0u);
  // The system moves: the first call times A out (its last report is 100 ms old).
  SND_REQUIRE(sys.AdvanceToNewBlock());
  SND_CHECK_EQ(snd_phys_submit_contacts(p, nullptr, 0, 0.02f), SND_OK);
  SND_CHECK_EQ(Stats(p).tracking_timeouts, 2u);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_STOP), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_STOP)[0].r.stop_reason, SND_PHYSICS_STOP_TIMEOUT);
  // A reported through a stall keeps its entry and records the latest values after it.
  rec.Clear();
  {
    const snd_contact a = slide(1, 1.0f, 0);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.02f), SND_OK);  // starts again
  }
  for (int i = 0; i < 5; ++i) {
    const snd_contact a = slide(1, 2.0f, 0);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.0f), SND_OK);  // the 4th unchanged call stalls
  }
  SND_CHECK(Stats(p).audio_stalled_calls >= 12u);
  rec.Clear();
  {
    const snd_contact a = slide(1, 8.0f, 3.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.0f), SND_OK);  // still stalled
  }
  SND_CHECK_EQ(rec.recs.size(), 0u);
  SND_REQUIRE(sys.AdvanceToNewBlock());
  {
    const snd_contact a = slide(1, 8.0f, 3.0f);
    SND_CHECK_EQ(snd_phys_submit_contacts(p, &a, 1, 0.0f), SND_OK);
  }
  // UPDATE, or (the events are not loaded here: the playback may have ended meanwhile) a
  // restart; either way the latest values.
  bool latest = false;
  for (const Rec& r : rec.recs) {
    if (r.r.kind == SND_PHYSICS_RECORD_UPDATE || r.r.kind == SND_PHYSICS_RECORD_START) {
      latest = std::fabs(r.r.volume - 0.8f) < 1e-6f && r.r.position.x == 3.0f;
    }
  }
  SND_CHECK(latest);
  snd_phys_destroy(p);
}
