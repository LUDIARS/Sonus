// The PHYS chunk of a real bank made by buildtool (bank format 2.0 §6.2): snd_phys_create with
// a bank and table id, snd_physics_load_bank_table, the documented mapping (events, footsteps,
// curves, threshold), the error results (arguments, NOT_READY, NOT_FOUND, LIMIT_REACHED, the
// owner thread) and CORRUPT for re-signed banks whose PHYS bytes lie.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "mini_project.hpp"
#include "phys_rig.hpp"
#include "snd/buildtool/build.hpp"

using namespace phystest;
namespace bt = snd::buildtool;

namespace {

const char* kSine = "\"tracks\": [{\"clips\": [{\"synth\": {\"kind\": \"oscillator\", \"waveform\": \"sine\", \"frequencyHz\": 440}}]}]";

std::string EventBody() { return std::string("\"bank\": \"Main\", ") + kSine; }

// Builds Main.sndb with two material tables; returns the bank bytes (empty on failure).
std::vector<std::uint8_t> BuildBank(bool withTable = true) {
  mini::Project proj(withTable ? "phys-bank" : "phys-bank-none");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  for (const char* e : {"SE/Metal", "SE/Wood", "SE/MetalGlass", "SE/StepWood", "SE/Generic", "SE/MetalSlide", "SE/A"}) {
    std::string file = std::string("events/") + (e + 3) + ".json";
    proj.Add(file, "event", e, EventBody());
  }
  if (withTable) {
    proj.Add("physics/Default.json", "materialTable", "Phys/Default",
             "\"bank\": \"Main\", \"materials\": [\"metal\", \"wood\", \"glass\"], "
             "\"single\": {\"metal\": {\"impact\": \"SE/Metal\", \"slide\": \"SE/MetalSlide\"}, \"wood\": {\"impact\": \"SE/Wood\"}}, "
             "\"pairs\": [{\"a\": \"glass\", \"b\": \"metal\", \"event\": \"SE/MetalGlass\"}], "
             "\"footsteps\": {\"wood\": \"SE/StepWood\"}, \"defaults\": {\"impact\": \"SE/Generic\"}, "
             "\"curves\": {\"metal\": {\"impulseToVelocity\": [0.5, 2, 8, 20], \"massToPitchCents\": [100, -200], "
             "\"speedToVolumeDb\": [-20, -6]}, "
             "\"wood\": {\"impulseToVelocity\": [0.8, 1, 2, 3]}, \"glass\": {\"impulseToVelocity\": [0.6, 1, 2, 3]}}");
    proj.Add("physics/Other.json", "materialTable", "Phys/Other",
             "\"bank\": \"Main\", \"materials\": [\"a\"], \"single\": {\"a\": {\"impact\": \"SE/A\"}}");
  }
  bt::Diagnostics diag;
  const bool ok = proj.Build(&diag);
  if (!ok) snd::test::Note("build failed:\n%s", mini::Dump(diag).c_str());
  return ok ? mini::ReadBytes(proj.out() / "Main.sndb") : std::vector<std::uint8_t>{};
}

// CRC-32C (reflected, 0x82F63B78) as bank format §3 uses it.
std::uint32_t Crc32c(std::uint32_t crc, const std::uint8_t* d, std::size_t n) {
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
  }
  return ~crc;
}
template <typename T>
T Get(const std::vector<std::uint8_t>& b, std::size_t at) {
  T v;
  std::memcpy(&v, b.data() + at, sizeof(T));
  return v;
}
template <typename T>
void Put(std::vector<std::uint8_t>* b, std::size_t at, T v) {
  std::memcpy(b->data() + at, &v, sizeof(T));
}
// Recomputes checksum (48) and headerChecksum (52).
void Resign(std::vector<std::uint8_t>* b) {
  Put<std::uint32_t>(b, 48, Crc32c(0u, b->data() + 128, b->size() - 128));
  Put<std::uint32_t>(b, 52, 0u);
  Put<std::uint32_t>(b, 52, Crc32c(0u, b->data(), 128));
}
// Offset of the PHYS chunk in the file.
std::size_t PhysAt(const std::vector<std::uint8_t>& b, std::uint64_t* bytes) {
  const std::uint32_t count = Get<std::uint32_t>(b, 60);
  const std::uint64_t table = Get<std::uint64_t>(b, 72);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t e = static_cast<std::size_t>(table) + i * 32u;
    if (Get<std::uint32_t>(b, e) == SND_FOURCC('P', 'H', 'Y', 'S')) {
      *bytes = Get<std::uint64_t>(b, e + 24);
      return static_cast<std::size_t>(Get<std::uint64_t>(b, e + 16));
    }
  }
  return 0;
}
// Offset (in the file) of the PhysTable with `id`.
std::size_t TableAt(const std::vector<std::uint8_t>& b, std::size_t phys, snd_id id) {
  const std::uint32_t n = Get<std::uint32_t>(b, phys);
  const std::uint64_t at = Get<std::uint64_t>(b, phys + 8);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::size_t t = phys + static_cast<std::size_t>(at) + i * 64u;
    if (Get<std::uint32_t>(b, t) == id) return t;
  }
  return 0;
}

struct Loaded {
  explicit Loaded(const std::vector<std::uint8_t>& bank) {
    ok = sys.created == SND_OK && !bank.empty() && sys.LoadBank("Main.sndb", bank, &handle);
  }
  Sys sys;
  snd_bank_handle handle{};
  bool ok = false;
};

}  // namespace

SND_TEST(PhysicsBank, TableFromThePhysChunk) {
  const std::vector<std::uint8_t> bank = BuildBank();
  SND_REQUIRE(!bank.empty());
  Loaded L(bank);
  SND_REQUIRE(L.ok);
  snd_phys_desc desc;
  snd_phys_desc_init(&desc);
  desc.bank = L.handle;
  desc.table = snd_hash_id("Phys/Default");
  desc.cooldown_seconds = 0.0f;
  desc.budget_per_second = 1.0e6f;
  snd_phys* phys = nullptr;
  SND_REQUIRE_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_OK);
  Recorder rec;
  SND_REQUIRE_EQ(snd_physics_set_observer(phys, &Recorder::Fn, &rec), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(phys), 3u);
  const std::uint16_t metal = 0, wood = 1, glass = 2;
  auto play = [&](std::uint16_t a, std::uint16_t b, float impulse, snd_contact_kind k = SND_CONTACT_IMPACT,
                  float massA = 1.0f, float massB = 1.0f, float speed = 1.0f) {
    rec.Clear();
    static float x = 0.0f;
    x += 10.0f;
    const snd_contact c = Contact(k, 1, 2, a, b, x, 0, 0, impulse, speed, massA, massB);
    SND_CHECK_EQ(snd_phys_submit_contacts(phys, &c, 1, 0.01f), SND_OK);
    return rec.Of(k == SND_CONTACT_IMPACT || k == SND_CONTACT_BREAK ? SND_PHYSICS_RECORD_PLAY : SND_PHYSICS_RECORD_START);
  };
  // Events.
  std::vector<Rec> p = play(metal, glass, 5.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/MetalGlass"));
  p = play(metal, wood, 5.0f);
  SND_REQUIRE_EQ(p.size(), 2u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/Metal"));
  SND_CHECK_EQ(p[1].r.event, snd_hash_id("SE/Wood"));
  p = play(glass, glass, 5.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/Generic"));
  p = play(metal, metal, 5.0f, SND_CONTACT_SLIDE, 1.0f, 1.0f, 10.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/MetalSlide"));
  // Slide volume: speedToVolumeDb [-20, -6] at 0.1 and 10 m/s -> 10 m/s is the last point.
  SND_CHECK_NEAR(p[0].r.volume, static_cast<float>(std::pow(10.0, -6.0 / 20.0)), 1e-6f);
  // Curves of metal: impulse 2 is t1 -> velocity 1/3, volume 0.5; 20 -> 1, 1.
  p = play(metal, metal, 2.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_NEAR(p[0].r.velocity, 1.0f / 3.0f, 1e-6f);
  SND_CHECK_EQ(p[0].r.volume, 0.5f);
  SND_CHECK_EQ(p[0].r.pitch, static_cast<float>(std::pow(2.0, 100.0 / 1200.0)));  // mass 1 kg: the first point
  p = play(metal, metal, 30.0f, SND_CONTACT_IMPACT, 100.0f, 200.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.velocity, 1.0f);
  SND_CHECK_EQ(p[0].r.volume, 1.0f);
  SND_CHECK_NEAR(p[0].r.pitch, static_cast<float>(std::pow(2.0, -200.0 / 1200.0)), 1e-6f);  // 100 kg: the last point
  // Mass between the log-spaced points.
  {
    snd_physics_curve m{};
    m.count = 8;
    for (std::uint32_t i = 0; i < 8; ++i) {
      const double fr = static_cast<double>(i) / 7.0;
      m.points[i] = {static_cast<float>(std::pow(100.0, fr)),
                     static_cast<float>(std::pow(2.0, (100.0 + (-300.0) * fr) / 1200.0))};
    }
    p = play(metal, metal, 3.0f, SND_CONTACT_IMPACT, 10.0f, 10.0f);
    SND_REQUIRE_EQ(p.size(), 1u);
    SND_CHECK_NEAR(p[0].r.pitch, EvalCurve(m, 10.0f), 1e-6f);
  }
  // Threshold: the smallest t0 (metal 0.5); equal passes.
  rec.Clear();
  const snd_contact below = Contact(SND_CONTACT_IMPACT, 1, 2, wood, wood, 500, 0, 0, 0.49f);
  const snd_contact equal = Contact(SND_CONTACT_IMPACT, 1, 2, wood, wood, 510, 0, 0, 0.5f);
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, &below, 1, 0.01f), SND_OK);
  SND_CHECK_EQ(snd_phys_submit_contacts(phys, &equal, 1, 0.01f), SND_OK);
  SND_CHECK_EQ(rec.Count(SND_PHYSICS_RECORD_PLAY), 1u);
  // Footsteps: PHYS has one column per material -> every motion; no footstep default.
  rec.Clear();
  const snd_surface_weight w[2] = {{wood, 0, 1.0f}, {metal, 0, 0.5f}};
  SND_CHECK_EQ(snd_phys_play_footstep(phys, snd_emitter_handle{0}, metal, w, 2, SND_MOTION_SPRINT), SND_OK);
  p = rec.Of(SND_PHYSICS_RECORD_PLAY);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/StepWood"));
  SND_CHECK_EQ(p[0].r.stage, SND_PHYSICS_STAGE_FOOT_ANY);
  // The other table by id.
  SND_CHECK_EQ(snd_physics_load_bank_table(phys, L.handle, snd_hash_id("Phys/Other")), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(phys), 1u);
  p = play(0, 0, 5.0f);
  SND_REQUIRE_EQ(p.size(), 1u);
  SND_CHECK_EQ(p[0].r.event, snd_hash_id("SE/A"));
  // Errors; the table stays.
  SND_CHECK_EQ(snd_physics_load_bank_table(phys, L.handle, snd_hash_id("Phys/Nope")), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(snd_physics_load_bank_table(phys, L.handle, SND_ID_NONE), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(snd_physics_load_bank_table(phys, snd_bank_handle{0}, snd_hash_id("Phys/Other")), SND_ERR_INVALID_HANDLE);
  SND_CHECK_EQ(snd_physics_material_count(phys), 1u);
  snd_phys_destroy(phys);
  // snd_physics_create + a smaller max_materials: LIMIT_REACHED.
  snd_physics_config cfg;
  snd_physics_config_init(&cfg);
  cfg.max_materials = 2;
  SND_REQUIRE_EQ(snd_physics_create(L.sys.sys, &cfg, nullptr, &phys), SND_OK);
  SND_CHECK_EQ(snd_physics_load_bank_table(phys, L.handle, snd_hash_id("Phys/Default")), SND_ERR_LIMIT_REACHED);
  SND_CHECK_EQ(snd_physics_material_count(phys), 0u);
  // The owner thread only (snd_get_bank_chunk): a physics object on a registered thread.
  snd_result onOther = SND_OK;
  std::thread t([&] {
    snd_register_thread(L.sys.sys);
    snd_phys* q = nullptr;
    if (snd_physics_create(L.sys.sys, &cfg, nullptr, &q) == SND_OK) {
      onOther = snd_physics_load_bank_table(q, L.handle, snd_hash_id("Phys/Other"));
      snd_phys_destroy(q);
    }
    snd_unregister_thread(L.sys.sys);
  });
  t.join();
  SND_CHECK_EQ(onOther, SND_ERR_WRONG_THREAD);
  snd_phys_destroy(phys);
}

SND_TEST(PhysicsBank, CreateErrors) {
  const std::vector<std::uint8_t> bank = BuildBank();
  SND_REQUIRE(!bank.empty());
  Loaded L(bank);
  SND_REQUIRE(L.ok);
  snd_phys_desc desc;
  snd_phys_desc_init(&desc);
  snd_phys* phys = reinterpret_cast<snd_phys*>(0x1);
  // Bank and table must come together.
  desc.table = snd_hash_id("Phys/Default");
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK(phys == nullptr);
  desc.table = SND_ID_NONE;
  desc.bank = L.handle;
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_ERR_INVALID_ARGUMENT);
  desc.table = snd_hash_id("Phys/Nope");
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_ERR_NOT_FOUND);
  SND_CHECK(phys == nullptr);
  // A stale bank handle: INVALID_ARGUMENT (the frozen result list).
  desc.bank = snd_bank_handle{SND_HANDLE_MAKE(5, 77)};
  desc.table = snd_hash_id("Phys/Default");
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_ERR_INVALID_ARGUMENT);
  // Neither: an empty table.
  snd_phys_desc_init(&desc);
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_OK);
  SND_CHECK_EQ(snd_physics_material_count(phys), 0u);
  snd_phys_destroy(phys);
  // A bank still loading: NOT_READY. A bank without PHYS: NOT_FOUND.
  const std::vector<std::uint8_t> plain = BuildBank(false);
  SND_REQUIRE(!plain.empty());
  Sys s2;
  SND_REQUIRE_EQ(s2.created, SND_OK);
  s2.fs.AddFile("Plain.sndb", plain);
  const snd_bank_handle h = snd_load_bank_async(s2.sys, "Plain.sndb");
  snd_update(s2.sys);
  snd_phys_desc_init(&desc);
  desc.bank = h;
  desc.table = snd_hash_id("Phys/Default");
  if (snd_get_bank_state(s2.sys, h) != SND_BANK_STATE_LOADED) {
    SND_CHECK_EQ(snd_phys_create(s2.sys, &desc, nullptr, &phys), SND_ERR_NOT_READY);
  }
  for (int i = 0; i < 4000 && snd_get_bank_state(s2.sys, h) != SND_BANK_STATE_LOADED; ++i) s2.Advance(1);
  SND_REQUIRE_EQ(snd_get_bank_state(s2.sys, h), SND_BANK_STATE_LOADED);
  SND_CHECK_EQ(snd_phys_create(s2.sys, &desc, nullptr, &phys), SND_ERR_NOT_FOUND);
  // Out of budget: the allocator refuses.
  CountingAllocator a;
  a.failAfter = 0;
  const snd_allocator at = a.Table();
  snd_phys_desc_init(&desc);
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, &at, &phys), SND_ERR_OUT_OF_BUDGET);
  SND_CHECK(phys == nullptr);
  // Zero capacities are refused.
  desc.max_tracked_pairs = 0;
  SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &phys), SND_ERR_INVALID_ARGUMENT);
}

SND_TEST(PhysicsBank, CorruptPhysBytesAreRefused) {
  const std::vector<std::uint8_t> good = BuildBank();
  SND_REQUIRE(!good.empty());
  std::uint64_t physBytes = 0;
  const std::size_t phys = PhysAt(good, &physBytes);
  SND_REQUIRE(phys != 0u && physBytes >= 16u + 2u * 64u);
  const std::size_t table = TableAt(good, phys, snd_hash_id("Phys/Default"));
  SND_REQUIRE(table != 0u);
  const std::uint64_t curveOffset = Get<std::uint64_t>(good, table + 48);
  // byBank: the bank loader's own check of the PHYS structure (bank_check.hpp) refuses the bank
  // already; physics checks the same again (it does not trust the chunk) and also the values.
  struct Mutation {
    const char* what;
    bool byBank;
    void (*apply)(std::vector<std::uint8_t>*, std::size_t phys, std::size_t table, std::uint64_t physBytes, std::uint64_t curves);
  };
  const Mutation mutations[] = {
      {"tablesOffset past the chunk", true, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t n, std::uint64_t) {
         Put<std::uint64_t>(b, p + 8, n + 64u);
       }},
      {"tablesOffset misaligned", true, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t) {
         Put<std::uint64_t>(b, p + 8, 12u);
       }},
      {"tableCount too large", true, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t) {
         Put<std::uint32_t>(b, p, 1000u);
       }},
      {"materialCount 0xFFFF", true, [](std::vector<std::uint8_t>* b, std::size_t, std::size_t t, std::uint64_t, std::uint64_t) {
         Put<std::uint32_t>(b, t + 4, 0xFFFFu);
       }},
      {"materialCount past the arrays", true, [](std::vector<std::uint8_t>* b, std::size_t, std::size_t t, std::uint64_t, std::uint64_t) {
         Put<std::uint32_t>(b, t + 4, 3000u);
       }},
      {"singleOffset past the chunk", true, [](std::vector<std::uint8_t>* b, std::size_t, std::size_t t, std::uint64_t n, std::uint64_t) {
         Put<std::uint64_t>(b, t + 8, n - 8u);
       }},
      {"pairOffset misaligned", true, [](std::vector<std::uint8_t>* b, std::size_t, std::size_t t, std::uint64_t, std::uint64_t) {
         Put<std::uint64_t>(b, t + 16, Get<std::uint64_t>(*b, t + 16) + 2u);
       }},
      {"footstepOffset huge", true, [](std::vector<std::uint8_t>* b, std::size_t, std::size_t t, std::uint64_t, std::uint64_t) {
         Put<std::uint64_t>(b, t + 24, ~std::uint64_t{0} - 3u);
       }},
      {"curve NaN", false, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t c) {
         Put<float>(b, p + static_cast<std::size_t>(c) + 20u, std::numeric_limits<float>::quiet_NaN());  // massToPitchCents[1] of metal
       }},
      {"thresholds decreasing", false, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t c) {
         Put<float>(b, p + static_cast<std::size_t>(c) + 4u, 0.1f);  // t1 < t0 (0.5)
       }},
      {"massToPitchCents beyond 4800", false, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t c) {
         Put<float>(b, p + static_cast<std::size_t>(c) + 16u, -4800.5f);  // metal
       }},
      {"speedToVolumeDb beyond 200", false, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t c) {
         Put<float>(b, p + static_cast<std::size_t>(c) + 24u, 200.5f);  // metal
       }},
      {"speedToVolumeDb infinite", false, [](std::vector<std::uint8_t>* b, std::size_t p, std::size_t, std::uint64_t, std::uint64_t c) {
         Put<float>(b, p + static_cast<std::size_t>(c) + 32u + 28u, std::numeric_limits<float>::infinity());  // wood
       }},
  };
  for (const Mutation& m : mutations) {
    std::vector<std::uint8_t> bad = good;
    m.apply(&bad, phys, table, physBytes, curveOffset);
    Resign(&bad);
    Loaded L(bad);
    if (m.byBank) {
      SND_CHECK_MSG(!L.ok, "%s: the bank was accepted", m.what);
      continue;
    }
    SND_CHECK_MSG(L.ok, "%s: the bank itself was refused", m.what);
    if (!L.ok) continue;
    snd_physics_config cfg;
    snd_physics_config_init(&cfg);
    snd_phys* p = nullptr;
    SND_REQUIRE_EQ(snd_physics_create(L.sys.sys, &cfg, nullptr, &p), SND_OK);
    // A good table first: it must survive the failed load.
    std::vector<snd_id> single;
    snd_physics_table_desc d = SinglesTable(5, &single);
    SND_REQUIRE_EQ(snd_physics_set_table(p, &d), SND_OK);
    const snd_result r = snd_physics_load_bank_table(p, L.handle, snd_hash_id("Phys/Default"));
    SND_CHECK_MSG(r == SND_ERR_CORRUPT, "%s: %d", m.what, r);
    SND_CHECK_EQ(snd_physics_material_count(p), 5u);
    snd_phys_destroy(p);
    snd_phys_desc desc;
    snd_phys_desc_init(&desc);
    desc.bank = L.handle;
    desc.table = snd_hash_id("Phys/Default");
    p = reinterpret_cast<snd_phys*>(0x1);
    SND_CHECK_EQ(snd_phys_create(L.sys.sys, &desc, nullptr, &p), SND_ERR_CORRUPT);
    SND_CHECK(p == nullptr);
  }
}
