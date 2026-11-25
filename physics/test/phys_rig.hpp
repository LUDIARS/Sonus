// Test rig of the physics module: a real snd_system through the C API on the built-in
// manual output device (SND_OUTPUT_MANUAL, D-102) and the in-memory file system mock, a
// recorder for the physics observer, a counting allocator, banks made with buildtool.
#ifndef SND_PHYSICS_TEST_PHYS_RIG_HPP
#define SND_PHYSICS_TEST_PHYS_RIG_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "snd/mocks/memory_file_system.hpp"
#include "snd/physics/physics.h"
#include "snd/physics/physics.hpp"
#include "snd/sonus.h"
#include "snd/sonus.hpp"
#include "snd/test/test.hpp"

namespace phystest {

inline constexpr std::uint32_t kBlock = 256;

inline snd_config SmallConfig() {
  snd_config c;
  snd_config_init(&c);
  c.max_real_voices = 32;
  c.max_virtual_voices = 64;
  c.fade_reserve_voices = 8;
  c.max_playbacks = 256;
  c.max_emitters = 128;
  c.max_transports = 4;
  c.max_reservations = 32;
  c.max_streams = 2;
  c.max_banks = 4;
  c.max_user_buffers = 2;
  c.max_global_params = 32;
  c.param_overflow_blocks = 64;
  c.event_ring_capacity = 1024;
  c.max_registered_threads = 4;
  c.commands.arena_bytes = 256u << 10;
  c.commands.reserve_bytes = 16u << 10;
  c.commands.overflow_param_slots = 64;
  c.memory.stream_pool_bytes = 256u << 10;
  c.memory.pcm_cache_bytes = 256u << 10;
  c.memory.max_decoders = 4;
  c.memory.frame_arena_bytes = 128u << 10;
  c.memory.thread_arena_bytes = 128u << 10;
  c.jobs.realtime_workers = 0;
  c.jobs.normal_workers = 1;
  c.max_voice_channels = 2;
  c.max_buses = 8;
  c.max_categories = 4;
  c.max_snapshots = 4;
  c.max_limit_groups = 4;
  c.max_param_defs = 32;
  c.max_duck_rules = 4;
  c.tuning.lod_down_blocks = 0;
  return c;
}

// A system on the manual device and the game's in-memory file system.
struct Sys {
  explicit Sys(snd_config cfg = SmallConfig()) {
    fsTable = snd::ToC(fs);
    cfg.output.device = nullptr;
    cfg.output_kind = SND_OUTPUT_MANUAL;
    created = snd_create(&cfg, nullptr, &fsTable, &sys);
  }
  ~Sys() { snd_destroy(sys); }
  Sys(const Sys&) = delete;
  Sys& operator=(const Sys&) = delete;

  // snd_update, then `periods` device periods; captured frames are appended to `out`.
  void Advance(std::uint32_t periods = 1) {
    std::vector<float> buf(std::size_t{kBlock} * 2u);
    for (std::uint32_t i = 0; i < periods; ++i) {
      snd_update(sys);
      if (snd_advance_output(sys, 1, buf.data(), kBlock) != SND_OK) ++advanceErrors;
      out.insert(out.end(), buf.begin(), buf.end());
    }
    snd_update(sys);
  }
  // Advance(1) until snd_get_stats().block_index has moved (one period does not always move
  // it: the block can be published a period later). false when it did not move at all.
  bool AdvanceToNewBlock(int maxPeriods = 50) {
    snd_stats before{};
    snd_get_stats(sys, &before);
    for (int i = 0; i < maxPeriods; ++i) {
      Advance(1);
      snd_stats now{};
      snd_get_stats(sys, &now);
      if (now.block_index != before.block_index) return true;
    }
    return false;
  }
  std::uint64_t Frames() const { return out.size() / 2u; }
  float L(std::uint64_t f) const { return out[static_cast<std::size_t>(f) * 2u]; }
  float R(std::uint64_t f) const { return out[static_cast<std::size_t>(f) * 2u + 1u]; }
  // RMS of a channel over [from, from + n).
  double Rms(int ch, std::uint64_t from, std::uint64_t n) const {
    double s = 0.0;
    for (std::uint64_t f = from; f < from + n; ++f) {
      const double v = ch == 0 ? L(f) : R(f);
      s += v * v;
    }
    return n == 0 ? 0.0 : std::sqrt(s / static_cast<double>(n));
  }

  bool LoadBank(const std::string& path, const std::vector<std::uint8_t>& bytes, snd_bank_handle* outBank) {
    fs.AddFile(path, bytes);
    const snd_bank_handle h = snd_load_bank_async(sys, path.c_str());
    *outBank = h;
    for (int i = 0; i < 4000 && snd_get_bank_state(sys, h) != SND_BANK_STATE_LOADED; ++i) {
      Advance(1);
      if (snd_get_bank_state(sys, h) == SND_BANK_STATE_FAILED) break;
    }
    snd_update(sys);
    return snd_get_bank_state(sys, h) == SND_BANK_STATE_LOADED;
  }

  snd::mock::MemoryFileSystem fs;
  snd_file_system fsTable{};
  snd_system* sys = nullptr;
  snd_result created = SND_ERR_INVALID_ARGUMENT;
  std::vector<float> out;
  std::uint32_t advanceErrors = 0;
};

// A copy of one observer record (the params pointer is only valid during the call).
struct Rec {
  snd_physics_record r;
  std::vector<snd_physics_param> params;
  float Param(snd_id id, float missing = -12345.0f) const {
    for (const snd_physics_param& p : params) {
      if (p.id == id) return p.value;
    }
    return missing;
  }
  bool Has(snd_id id) const { return Param(id, -12345.0f) != -12345.0f; }
};

struct Recorder {
  std::vector<Rec> recs;
  static void SND_CALL Fn(void* user, const snd_physics_record* r) {
    Recorder* self = static_cast<Recorder*>(user);
    Rec c;
    c.r = *r;
    c.r.params = nullptr;
    if (r->params != nullptr) c.params.assign(r->params, r->params + r->param_count);
    self->recs.push_back(c);
  }
  void Clear() { recs.clear(); }
  std::vector<Rec> Of(snd_physics_record_kind kind) const {
    std::vector<Rec> v;
    for (const Rec& r : recs) {
      if (r.r.kind == kind) v.push_back(r);
    }
    return v;
  }
  std::size_t Count(snd_physics_record_kind kind) const { return Of(kind).size(); }
};

// Counting allocator (snd_allocator).
struct CountingAllocator {
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t bytes = 0;
  std::uint64_t failAfter = ~std::uint64_t{0};  // fail every allocation from this index on
  static void* SND_CALL Alloc(void* u, std::size_t n, std::size_t align, snd_mem_tag) {
    CountingAllocator* a = static_cast<CountingAllocator*>(u);
    if (a->allocs >= a->failAfter) return nullptr;
    ++a->allocs;
    a->bytes += n;
    const std::size_t al = align < 16u ? 16u : align;
    void* p = nullptr;
#if defined(_WIN32)
    p = _aligned_malloc(n, al);
#else
    if (posix_memalign(&p, al, n) != 0) p = nullptr;
#endif
    return p;
  }
  static void SND_CALL Free(void* u, void* p, snd_mem_tag) {
    if (p == nullptr) return;
    ++static_cast<CountingAllocator*>(u)->frees;
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
  }
  snd_allocator Table() { return snd_allocator{this, &Alloc, &Free}; }
};

inline snd_contact Contact(snd_contact_kind kind, std::uint64_t a, std::uint64_t b, std::uint16_t ma, std::uint16_t mb,
                           float x, float y, float z, float impulse, float speed = 1.0f, float massA = 1.0f,
                           float massB = 1.0f) {
  snd_contact c{};
  c.object_a = a;
  c.object_b = b;
  c.material_a = ma;
  c.material_b = mb;
  c.kind = kind;
  c.position[0] = x;
  c.position[1] = y;
  c.position[2] = z;
  c.normal[1] = 1.0f;
  c.impulse = impulse;
  c.relative_speed = speed;
  c.mass_a = massA;
  c.mass_b = massB;
  return c;
}
inline snd_contact Impact(std::uint64_t a, std::uint64_t b, float x, float impulse, std::uint16_t ma = 0,
                          std::uint16_t mb = 0) {
  return Contact(SND_CONTACT_IMPACT, a, b, ma, mb, x, 0.0f, 0.0f, impulse);
}

inline snd_physics_stats Stats(const snd_phys* p) {
  snd_physics_stats s{};
  s.struct_size = sizeof(s);
  snd_physics_get_stats(p, &s);
  return s;
}

// A config with the culling switched off where a test does not look at it.
inline snd_physics_config OpenConfig(Recorder* rec) {
  snd_physics_config c;
  snd_physics_config_init(&c);
  c.budget_per_second = 1.0e6f;
  c.cooldown_seconds = 0.0f;
  c.observer = rec != nullptr ? &Recorder::Fn : nullptr;
  c.observer_user = rec;
  return c;
}

// A table of `n` materials where material m's single event for kind k is Ev(m, k).
inline snd_id Ev(std::uint32_t m, std::uint32_t k) { return 0x1000u + m * 16u + k; }
inline snd_physics_table_desc SinglesTable(std::uint32_t n, std::vector<snd_id>* single) {
  single->assign(std::size_t{n} * 4u, SND_ID_NONE);
  for (std::uint32_t m = 0; m < n; ++m) {
    for (std::uint32_t k = 0; k < 4; ++k) (*single)[std::size_t{m} * 4u + k] = Ev(m, k);
  }
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  d.material_count = n;
  d.single = single->data();
  return d;
}

inline snd_physics_curve Linear(std::initializer_list<snd_physics_curve_point> pts) {
  snd_physics_curve c{};
  for (const snd_physics_curve_point& p : pts) c.points[c.count++] = p;
  return c;
}

// The documented piecewise-linear evaluation (end values outside).
inline float EvalCurve(const snd_physics_curve& c, float x) {
  if (c.count == 0) return 1.0f;
  if (!(x > c.points[0].x)) return c.points[0].y;
  if (x >= c.points[c.count - 1].x) return c.points[c.count - 1].y;
  for (std::uint32_t i = 0; i + 1 < c.count; ++i) {
    if (x < c.points[i + 1].x) {
      const float dx = c.points[i + 1].x - c.points[i].x;
      return c.points[i].y + (c.points[i + 1].y - c.points[i].y) * ((x - c.points[i].x) / dx);
    }
  }
  return c.points[c.count - 1].y;
}

inline snd_id P(const char* name) { return snd_hash_id(name); }

}  // namespace phystest

#endif  // SND_PHYSICS_TEST_PHYS_RIG_HPP
