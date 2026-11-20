// Independent tests: creation, configuration, threads, memory, statistics, observer,
// play_at, C++ wrapper, C11 header (decision A, E). Properties P01..P05, P10, P39, P40.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "indep_phys_rig.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

extern "C" int snd_physics_indep_c11_check(unsigned* sizes, unsigned count);

using namespace indepphys;

namespace {

snd_physics_table_desc OneMaterial(std::vector<snd_id>* single) {
  single->assign(4, SND_ID_NONE);
  for (std::uint32_t k = 0; k < 4; ++k) (*single)[k] = 0xC000u + k;
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = 1;
  d.single = single->data();
  d.footstep_default = 0xC0F0u;
  return d;
}

}  // namespace

// P01, P03: argument and range checks; *out is NULL on error.
SND_TEST(IndepApi, CreateRejectsBadConfig) {
  Sys s;
  SND_REQUIRE_EQ(s.created, SND_OK);
  snd_phys* p = nullptr;
  snd_physics_config c = Cfg(nullptr);
  SND_CHECK_EQ(snd_physics_create(nullptr, &c, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_create(s.sys, nullptr, nullptr, &p), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_create(s.sys, &c, nullptr, nullptr), SND_ERR_INVALID_ARGUMENT);
  struct Bad {
    const char* what;
    void (*edit)(snd_physics_config&);
  };
  const Bad bad[] = {
      {"struct_size 0", [](snd_physics_config& x) { x.struct_size = 0; }},
      {"struct_size 4", [](snd_physics_config& x) { x.struct_size = 4; }},
      {"struct_size 10", [](snd_physics_config& x) { x.struct_size = 10; }},
      {"version 0", [](snd_physics_config& x) { x.version = 0; }},
      {"version 2", [](snd_physics_config& x) { x.version = SND_PHYSICS_CONFIG_VERSION + 1; }},
      {"grid 0", [](snd_physics_config& x) { x.grid_size = 0.0f; }},
      {"grid -1", [](snd_physics_config& x) { x.grid_size = -1.0f; }},
      {"grid NaN", [](snd_physics_config& x) { x.grid_size = kNaN; }},
      {"grid inf", [](snd_physics_config& x) { x.grid_size = kInf; }},
      {"budget -1", [](snd_physics_config& x) { x.budget_per_second = -1.0f; }},
      {"budget NaN", [](snd_physics_config& x) { x.budget_per_second = kNaN; }},
      {"budget inf", [](snd_physics_config& x) { x.budget_per_second = kInf; }},
      {"cooldown -1", [](snd_physics_config& x) { x.cooldown_seconds = -1.0f; }},
      {"cooldown inf", [](snd_physics_config& x) { x.cooldown_seconds = kInf; }},
      {"threshold -1", [](snd_physics_config& x) { x.impulse_threshold = -1.0f; }},
      {"threshold NaN", [](snd_physics_config& x) { x.impulse_threshold = kNaN; }},
      {"min slide -1", [](snd_physics_config& x) { x.min_slide_speed = -1.0f; }},
      {"timeout NaN", [](snd_physics_config& x) { x.tracking_timeout_seconds = kNaN; }},
      {"fade -1", [](snd_physics_config& x) { x.tracking_fade_seconds = -1.0f; }},
      {"max dt 0", [](snd_physics_config& x) { x.max_dt_seconds = 0.0f; }},
      {"max dt inf", [](snd_physics_config& x) { x.max_dt_seconds = kInf; }},
      {"materials 0", [](snd_physics_config& x) { x.max_materials = 0; }},
      {"materials limit + 1", [](snd_physics_config& x) { x.max_materials = SND_PHYSICS_MAX_MATERIALS_LIMIT + 1; }},
      {"cooldowns cap + 1", [](snd_physics_config& x) { x.max_cooldowns = SND_PHYSICS_MAX_CAPACITY + 1; }},
      {"groups cap + 1", [](snd_physics_config& x) { x.max_groups_per_call = SND_PHYSICS_MAX_CAPACITY + 1; }},
      {"tracked cap + 1", [](snd_physics_config& x) { x.max_tracked_pairs = SND_PHYSICS_MAX_CAPACITY + 1; }},
      {"game materials cap + 1",
       [](snd_physics_config& x) { x.max_game_materials = SND_PHYSICS_MAX_CAPACITY + 1; }},
      {"held cap + 1", [](snd_physics_config& x) { x.max_held_emitters = SND_PHYSICS_MAX_CAPACITY + 1; }},
      {"reserved param id", [](snd_physics_config& x) { x.params.mass = SND_ID_RESERVED_LAST; }},
  };
  for (const Bad& b : bad) {
    snd_physics_config x = Cfg(nullptr);
    b.edit(x);
    p = reinterpret_cast<snd_phys*>(&x);
    const snd_result r = snd_physics_create(s.sys, &x, nullptr, &p);
    SND_CHECK_MSG(r == SND_ERR_INVALID_ARGUMENT, "%s: %d", b.what, static_cast<int>(r));
    SND_CHECK_MSG(p == nullptr, "%s: *out not NULL", b.what);
    if (p != nullptr) snd_physics_destroy(p);
  }
  // Boundaries that are allowed: exactly the limits, zeros where >= 0.
  snd_physics_config ok = Cfg(nullptr);
  ok.max_materials = SND_PHYSICS_MAX_MATERIALS_LIMIT;
  ok.budget_per_second = 0.0f;
  ok.cooldown_seconds = 0.0f;
  ok.tracking_timeout_seconds = 0.0f;
  ok.tracking_fade_seconds = 0.0f;
  SND_CHECK_EQ(snd_physics_create(s.sys, &ok, nullptr, &p), SND_OK);
  snd_physics_destroy(p);
}

// P02: a smaller struct_size (older header) gets defaults for the fields it does not
// cover; the bytes past it are never read (garbage there must not matter).
SND_TEST(IndepApi, SmallerStructSizeUsesDefaults) {
  Sys s;
  Recorder rec;
  // Room for the config plus garbage after it.
  alignas(16) unsigned char buf[sizeof(snd_physics_config) + 64];
  std::memset(buf, 0xFF, sizeof buf);
  snd_physics_config c;
  snd_physics_config_init(&c);
  c.budget_per_second = 10.0f;  // covered: 1 per 0.1 s
  c.cooldown_seconds = 0.0f;
  const std::size_t cut = offsetof(snd_physics_config, max_tracked_pairs);
  c.struct_size = static_cast<std::uint32_t>(cut);
  std::memcpy(buf, &c, cut);  // the rest stays 0xFF (capacities 0xFFFFFFFF, observer = garbage)
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, reinterpret_cast<const snd_physics_config*>(buf), nullptr, &p), SND_OK);
  std::vector<snd_id> single;
  const snd_physics_table_desc d = OneMaterial(&single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  std::vector<snd_contact> cs;
  for (std::uint64_t i = 0; i < 3; ++i) cs.push_back(Contact(SND_CONTACT_IMPACT, i, i + 9, 0, 0, static_cast<float>(i), 0, 0, 1));
  SND_CHECK_EQ(snd_phys_submit_contacts(p, cs.data(), 3, 0.1f), SND_OK);
  const snd_physics_stats st = phystest::Stats(p);
  SND_CHECK_EQ(st.group_requests, 1u);
  SND_CHECK_EQ(st.dropped_budget, 2u);
  // Default parameter ids (not covered): Phys/Impulse is passed.
  SND_REQUIRE_EQ(snd_physics_set_observer(p, &Recorder::Fn, &rec), SND_OK);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, cs.data(), 1, 0.1f), SND_OK);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].Has(snd_hash_id("Phys/Impulse")));
  snd_physics_destroy(p);
  // A larger struct_size (a newer header): the implementer's choice is reported here.
  snd_physics_config big = Cfg(nullptr);
  big.struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_config) + 8u);
  std::memcpy(buf, &big, sizeof big);
  p = nullptr;
  const snd_result r = snd_physics_create(s.sys, reinterpret_cast<const snd_physics_config*>(buf), nullptr, &p);
  snd::test::Note("struct_size = sizeof + 8: %d", static_cast<int>(r));
  snd_physics_destroy(p);
}

// P04: memory is taken at creation only; everything after allocates nothing; destroy
// returns it all; a failed allocation is OUT_OF_BUDGET without a leak.
SND_TEST(IndepApi, NoAllocationAfterCreate) {
  Sys s;
  snd_bank_handle bank{};
  SND_REQUIRE(s.LoadBank("phys.sndb", TestBank(), &bank));
  phystest::CountingAllocator a;
  snd_allocator at = a.Table();
  // An observer that does not allocate (the recorder of the rig copies the parameters).
  std::uint64_t records = 0;
  snd_physics_config c = Cfg(nullptr);
  c.observer = [](void* u, const snd_physics_record*) { ++*static_cast<std::uint64_t*>(u); };
  c.observer_user = &records;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, &at, &p), SND_OK);
  const std::uint64_t allocs = a.allocs;
  SND_CHECK(allocs >= 1u);
  std::vector<snd_id> single;
  snd_physics_table_desc d = OneMaterial(&single);
  std::vector<snd_physics_material_entry> map(1024);
  for (std::size_t i = 0; i < map.size(); ++i) map[i].sound_material = static_cast<std::uint16_t>(i % 2);
  std::vector<snd_contact> cs;
  for (int j = 0; j < 1000; ++j) {
    cs.push_back(Contact(static_cast<snd_contact_kind>(j % 4), static_cast<std::uint64_t>(j % 37),
                         static_cast<std::uint64_t>(j % 11), static_cast<std::uint16_t>(j % 3), 0,
                         static_cast<float>(j % 13), 0, 0, 1.0f + static_cast<float>(j % 7), 1.0f));
  }
  snd_surface_weight w[3] = {{0, 0, 0.5f}, {1, 0, 0.25f}, {0, 0, 0.25f}};
  snd_physics_curves cv{};
  cv.impulse_to_volume = phystest::Linear({{0.0f, 0.5f}, {10.0f, 1.0f}});
  const snd_vec3 pos{1, 2, 3};
  std::uint32_t allocViolations = 0;
  for (int frame = 0; frame < 30; ++frame) {
    {
      snd::test::rt::ExpectViolations expect;  // core's own locks (if any) must not fail the test
      snd::test::rt::Reset();
      snd::rt::RtScope scope;
      SND_CHECK_EQ(snd_physics_set_table(p, &d), SND_OK);
      SND_CHECK_EQ(snd_physics_set_curves(p, 0, &cv), SND_OK);
      SND_CHECK_EQ(snd_physics_set_material_map(p, map.data(), static_cast<std::uint32_t>(map.size())), SND_OK);
      SND_CHECK_EQ(snd_physics_set_surface_param(p, 5, 0x1234u, 0.5f), SND_OK);
      snd_physics_material_entry one{};
      one.sound_material = 1;
      SND_CHECK_EQ(snd_physics_set_material_entry(p, 7, &one), SND_OK);
      if (frame % 10 == 9) SND_CHECK_EQ(snd_physics_load_bank_table(p, bank, snd_hash_id("I/Table")), SND_OK);
      SND_CHECK_EQ(snd_phys_submit_contacts(p, cs.data(), static_cast<std::uint32_t>(cs.size()), 1.0f / 60.0f), SND_OK);
      SND_CHECK_EQ(snd_phys_play_footstep(p, snd_emitter_handle{0}, 0, w, 3, SND_MOTION_RUN), SND_OK);
      SND_CHECK_EQ(snd_physics_play_at(p, 0xABCDu, &pos), SND_OK);
      snd_physics_stats st{};
      st.struct_size = sizeof st;
      SND_CHECK_EQ(snd_physics_get_stats(p, &st), SND_OK);
      if (frame == 15) SND_CHECK_EQ(snd_physics_reset_stats(p), SND_OK);
      allocViolations += snd::test::rt::Count(snd::rt::Violation::Alloc) + snd::test::rt::Count(snd::rt::Violation::Free);
    }
    s.Advance(1);
  }
  SND_CHECK_EQ(a.allocs, allocs);
  SND_CHECK_EQ(allocViolations, 0u);
  SND_CHECK(records > 1000u);
  snd_physics_destroy(p);
  SND_CHECK_EQ(a.frees, a.allocs);
  // Fault injection: every allocation index fails in turn.
  for (std::uint64_t failAt = 0; failAt < allocs; ++failAt) {
    phystest::CountingAllocator b;
    b.failAfter = failAt;
    snd_allocator bt = b.Table();
    snd_phys* q = reinterpret_cast<snd_phys*>(&b);
    SND_CHECK_EQ(snd_physics_create(s.sys, &c, &bt, &q), SND_ERR_OUT_OF_BUDGET);
    SND_CHECK(q == nullptr);
    SND_CHECK_EQ(b.frees, b.allocs);
  }
}

// P05: every function from another thread is WRONG_THREAD and changes nothing, even when
// that thread is registered with the system.
SND_TEST(IndepApi, WrongThread) {
  Sys s;
  Recorder rec;
  snd_physics_config c = Cfg(&rec);
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  std::vector<snd_id> single;
  const snd_physics_table_desc d = OneMaterial(&single);
  SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
  std::vector<snd_result> rs;
  std::uint32_t count = 99;
  bool registered = false;
  std::thread t([&] {
    registered = snd_register_thread(s.sys) == SND_OK;
    const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 5);
    const snd_surface_weight w{0, 0, 1.0f};
    const snd_vec3 pos{0, 0, 0};
    snd_physics_stats st{};
    st.struct_size = sizeof st;
    snd_physics_material_entry e{};
    rs.push_back(snd_phys_submit_contacts(p, &x, 1, 0.1f));
    rs.push_back(snd_phys_submit_contacts(p, nullptr, 0, 0.1f));
    rs.push_back(snd_phys_play_footstep(p, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK));
    rs.push_back(snd_physics_play_at(p, 0x99u, &pos));
    rs.push_back(snd_physics_set_observer(p, nullptr, nullptr));
    rs.push_back(snd_physics_set_table(p, &d));
    rs.push_back(snd_physics_set_single_event(p, 0, SND_CONTACT_IMPACT, 0x1u));
    rs.push_back(snd_physics_set_pair_event(p, 0, 0, SND_CONTACT_IMPACT, 0x1u));
    rs.push_back(snd_physics_set_default_event(p, SND_CONTACT_IMPACT, 0x1u));
    rs.push_back(snd_physics_set_footstep_event(p, 0, SND_MOTION_WALK, 0x1u));
    rs.push_back(snd_physics_set_curves(p, 0, nullptr));
    rs.push_back(snd_physics_set_material_map(p, &e, 1));
    rs.push_back(snd_physics_set_material_entry(p, 0, &e));
    rs.push_back(snd_physics_set_surface_param(p, 0, 0x1u, 1.0f));
    rs.push_back(snd_physics_get_stats(p, &st));
    rs.push_back(snd_physics_reset_stats(p));
    rs.push_back(snd_physics_load_bank_table(p, snd_bank_handle{1}, 0x1u));
    count = snd_physics_material_count(p);
    if (registered) snd_unregister_thread(s.sys);
  });
  t.join();
  SND_CHECK(registered);
  for (std::size_t i = 0; i < rs.size(); ++i) {
    SND_CHECK_MSG(rs[i] == SND_ERR_WRONG_THREAD, "call %llu: %d", static_cast<unsigned long long>(i), static_cast<int>(rs[i]));
  }
  SND_CHECK_EQ(count, 0u);
  // Nothing changed: no record, no statistics, the observer and table are still there.
  SND_CHECK(rec.recs.empty());
  const snd_physics_stats st = phystest::Stats(p);
  SND_CHECK_EQ(st.calls_submit, 0u);
  SND_CHECK_EQ(st.calls_rejected, 0u);
  SND_CHECK_EQ(st.footsteps, 0u);
  SND_CHECK_EQ(snd_physics_material_count(p), 1u);
  const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 5);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 0.1f), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  SND_CHECK_EQ(rec.Of(SND_PHYSICS_RECORD_PLAY)[0].r.event, 0xC000u);
  snd_physics_destroy(p);
}

// P39: statistics copy rules and reset.
SND_TEST(IndepApi, StatsStructSizeAndReset) {
  Sys s;
  snd_physics_config c = Cfg(nullptr);
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  const snd_contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, kNaN);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, 0.1f), SND_OK);
  SND_CHECK_EQ(snd_phys_submit_contacts(p, &x, 1, -1.0f), SND_ERR_INVALID_ARGUMENT);
  alignas(8) unsigned char buf[sizeof(snd_physics_stats) + 32];
  // Small: struct_size, reserved, calls_submit only.
  std::memset(buf, 0xAB, sizeof buf);
  snd_physics_stats* st = reinterpret_cast<snd_physics_stats*>(buf);
  st->struct_size = 16;
  SND_CHECK_EQ(snd_physics_get_stats(p, st), SND_OK);
  SND_CHECK_EQ(st->struct_size, 16u);
  SND_CHECK_EQ(st->calls_submit, 1u);
  for (std::size_t i = 16; i < sizeof buf; ++i) SND_CHECK_EQ(buf[i], 0xABu);
  // Larger than known: known part copied, struct_size kept, the tail untouched.
  std::memset(buf, 0xAB, sizeof buf);
  st->struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_stats) + 32);
  SND_CHECK_EQ(snd_physics_get_stats(p, st), SND_OK);
  SND_CHECK_EQ(st->struct_size, sizeof(snd_physics_stats) + 32);
  SND_CHECK_EQ(st->calls_rejected, 1u);
  SND_CHECK_EQ(st->dropped_invalid, 1u);
  for (std::size_t i = sizeof(snd_physics_stats); i < sizeof buf; ++i) SND_CHECK_EQ(buf[i], 0xABu);
  st->struct_size = 4;
  SND_CHECK_EQ(snd_physics_get_stats(p, st), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_get_stats(p, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_get_stats(nullptr, st), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_reset_stats(p), SND_OK);
  const snd_physics_stats z = phystest::Stats(p);
  SND_CHECK_EQ(z.calls_submit, 0u);
  SND_CHECK_EQ(z.calls_rejected, 0u);
  SND_CHECK_EQ(z.dropped_invalid, 0u);
  SND_CHECK_EQ(z.contacts_received, 0u);
  snd_physics_destroy(p);
}

// P40: the observer: one record at a time on the physics thread; physics calls from
// inside are UNSUPPORTED; destroy inside is a no-op.
namespace {
struct Reentry {
  snd_phys* p = nullptr;
  std::thread::id thread;
  bool otherThread = false;
  std::uint32_t depth = 0, maxDepth = 0, calls = 0;
  std::vector<snd_result> rs;
  static void SND_CALL Fn(void* u, const snd_physics_record* r) {
    Reentry* self = static_cast<Reentry*>(u);
    ++self->calls;
    if (std::this_thread::get_id() != self->thread) self->otherThread = true;
    ++self->depth;
    if (self->depth > self->maxDepth) self->maxDepth = self->depth;
    (void)r;
    const snd_contact x = Contact(SND_CONTACT_IMPACT, 7, 8, 0, 0, 0, 0, 0, 5);
    const snd_vec3 pos{0, 0, 0};
    const snd_surface_weight w{0, 0, 1.0f};
    snd_physics_stats st{};
    st.struct_size = sizeof st;
    self->rs.push_back(snd_phys_submit_contacts(self->p, &x, 1, 0.1f));
    self->rs.push_back(snd_phys_play_footstep(self->p, snd_emitter_handle{0}, 0, &w, 1, SND_MOTION_WALK));
    self->rs.push_back(snd_physics_play_at(self->p, 0x5u, &pos));
    self->rs.push_back(snd_physics_set_observer(self->p, nullptr, nullptr));
    self->rs.push_back(snd_physics_set_default_event(self->p, SND_CONTACT_IMPACT, 0x9u));
    self->rs.push_back(snd_physics_reset_stats(self->p));
    snd_physics_destroy(self->p);  // no-op
    snd_phys_destroy(self->p);     // no-op
    --self->depth;
  }
};
}  // namespace

SND_TEST(IndepApi, ObserverReentry) {
  Sys s;
  Reentry re;
  re.thread = std::this_thread::get_id();
  snd_physics_config c = Cfg(nullptr);
  c.observer = &Reentry::Fn;
  c.observer_user = &re;
  c.cooldown_seconds = 0.0f;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &re.p), SND_OK);
  std::vector<snd_id> single;
  const snd_physics_table_desc d = OneMaterial(&single);
  SND_REQUIRE_EQ(snd_physics_set_table(re.p, &d), SND_OK);
  const snd_contact x[2] = {Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 5),
                            Contact(SND_CONTACT_SLIDE, 3, 4, 0, 0, 0, 0, 0, 0, 1.0f)};
  SND_CHECK_EQ(snd_phys_submit_contacts(re.p, x, 2, 0.1f), SND_OK);
  SND_CHECK(re.calls >= 2u);
  SND_CHECK_EQ(re.maxDepth, 1u);
  SND_CHECK(!re.otherThread);
  for (snd_result r : re.rs) SND_CHECK_EQ(r, SND_ERR_UNSUPPORTED);
  // Still alive and usable, statistics not reset from inside.
  const snd_physics_stats st = phystest::Stats(re.p);
  SND_CHECK_EQ(st.calls_submit, 1u);
  SND_CHECK_EQ(st.tracking_started, 1u);
  re.rs.clear();
  const std::uint32_t before = re.calls;
  SND_CHECK_EQ(snd_phys_submit_contacts(re.p, x, 2, 0.1f), SND_OK);
  SND_CHECK(re.calls > before);
  // Destroy outside: the STOP record (destroy) also comes through the observer.
  re.rs.clear();
  snd_phys* p = re.p;
  snd_physics_destroy(p);
  for (snd_result r : re.rs) SND_CHECK_EQ(r, SND_ERR_UNSUPPORTED);
}

// 9.5 PlayAt: errors, statistics, not budgeted, record.
SND_TEST(IndepApi, PlayAt) {
  Sys s;
  Recorder rec;
  snd_physics_config c = Cfg(&rec);
  c.budget_per_second = 0.0f;
  snd_phys* p = nullptr;
  SND_REQUIRE_EQ(snd_physics_create(s.sys, &c, nullptr, &p), SND_OK);
  const snd_vec3 pos{1, 2, 3};
  const snd_vec3 bad{kNaN, 0, 0};
  SND_CHECK_EQ(snd_physics_play_at(p, 0x77u, &pos), SND_OK);
  SND_CHECK_EQ(snd_physics_play_at(p, SND_ID_NONE, &pos), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(p, 0x77u, nullptr), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(p, 0x77u, &bad), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_play_at(nullptr, 0x77u, &pos), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(phystest::Stats(p).requested_play_at, 1u);
  SND_REQUIRE_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  const Rec r = rec.Of(SND_PHYSICS_RECORD_PLAY)[0];
  SND_CHECK_EQ(r.r.source, SND_PHYSICS_SOURCE_PLAY_AT);
  SND_CHECK_EQ(r.r.stage, SND_PHYSICS_STAGE_DIRECT);
  SND_CHECK_EQ(r.r.event, 0x77u);
  SND_CHECK_EQ(r.r.position.z, 3.0f);
  snd_physics_destroy(p);
}

// P10: the C++ wrapper gives the C results; the C header compiles as C11 with the same
// layouts.
SND_TEST(IndepApi, CppWrapperAndC11) {
  unsigned sizes[8] = {};
  SND_CHECK_EQ(snd_physics_indep_c11_check(sizes, 8), 0);
  SND_CHECK_EQ(sizes[0], sizeof(snd_physics_config));
  SND_CHECK_EQ(sizes[1], sizeof(snd_physics_table_desc));
  SND_CHECK_EQ(sizes[2], sizeof(snd_physics_stats));
  SND_CHECK_EQ(sizes[3], sizeof(snd_physics_record));
  SND_CHECK_EQ(sizes[4], sizeof(snd_physics_material_entry));
  SND_CHECK_EQ(sizes[5], sizeof(snd_physics_curves));
  SND_CHECK_EQ(sizes[6], offsetof(snd_physics_config, params));
  SND_CHECK_EQ(sizes[7], offsetof(snd_physics_record, params));
  SND_CHECK_EQ(sizeof(snd_physics_material_entry), 36u);  // A-001: same layout as PhysMaterialMapEntry
  SND_CHECK_EQ(sizeof(snd_physics_curve), 72u);           // A-001: PhysCurve
  Sys s;
  snd::phys::PhysicsAudio* p = nullptr;
  snd::phys::Config c = snd::phys::DefaultConfig();
  SND_REQUIRE(snd::phys::Create(s.sys, c, nullptr, &p) == snd::Result::Ok);
  snd::phys::TableDesc t = snd::phys::EmptyTable();
  std::vector<snd_id> single(4, 0x31u);
  t.material_count = 1;
  t.single = single.data();
  SND_CHECK(snd::phys::SetTable(p, t) == snd::Result::Ok);
  SND_CHECK_EQ(snd::phys::MaterialCount(p), 1u);
  const snd::phys::Contact x = Contact(SND_CONTACT_IMPACT, 1, 2, 0, 0, 0, 0, 0, 5);
  SND_CHECK(snd::phys::SubmitContacts(p, &x, 1, -1.0f) == snd::Result::InvalidArgument);
  SND_CHECK(snd::phys::SubmitContacts(p, &x, 1, 0.1f) == snd::Result::Ok);
  snd::phys::Stats st{};
  SND_CHECK(snd::phys::GetStats(p, &st) == snd::Result::Ok);
  SND_CHECK_EQ(st.group_requests, 1u);
  SND_CHECK_EQ(st.calls_rejected, 1u);
  SND_CHECK(snd::phys::SetSingleEvent(p, 0, snd::phys::kAllKinds, 0x32u) == snd::Result::Ok);
  SND_CHECK(snd::phys::SetFootstepEvent(p, 0, snd::phys::kAnyMotion, 0x33u) == snd::Result::Ok);
  const snd::phys::SurfaceWeight w{0, 0, 1.0f};
  SND_CHECK(snd::phys::PlayFootstep(p, snd::EmitterHandle{}, 0, &w, 1, snd::phys::Motion::Land) == snd::Result::Ok);
  SND_CHECK(snd::phys::PlayAt(p, 0x34u, snd::Vec3{0, 0, 0}) == snd::Result::Ok);
  SND_CHECK(snd::phys::ResetStats(p) == snd::Result::Ok);
  snd::phys::Destroy(p);
}
