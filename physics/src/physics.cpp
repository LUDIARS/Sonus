// Physics-material audio (design.md 9): the snd_phys object, built on the public C API
// only (design.md 12.1). The rules are documented in snd/physics/physics.h.
#include "snd/physics/physics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include "snd/ext.h"
#include "snd/physics.h"
#include "snd/sonus.h"

namespace {

// ---------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------
constexpr std::uint16_t kUnknown = 0xFFFFu;
constexpr std::uint32_t kKinds = SND_PHYSICS_CONTACT_KIND_COUNT;
constexpr std::uint32_t kMotions = SND_PHYSICS_MOTION_COUNT;
constexpr std::uint32_t kMaxPoints = SND_PHYSICS_MAX_CURVE_POINTS;
constexpr std::uint32_t kMaxSurface = SND_PHYSICS_MAX_SURFACE_PARAMS;
constexpr std::uint32_t kEmptyBucket = 0xFFFFFFFFu;
// Named parameters (at most 5 per request) + surface parameters of two materials.
constexpr std::uint32_t kMaxRequestParams = 8u + 2u * kMaxSurface;
constexpr std::uint64_t kMaxMicros = std::uint64_t{1} << 62;
constexpr std::uint32_t kFourccPhys = SND_FOURCC('P', 'H', 'Y', 'S');

enum CurveIndex : std::uint32_t {
  kImpulseVolume = 0,
  kImpulseVelocity = 1,
  kMassPitch = 2,
  kSpeedVolume = 3,
  kSpeedPitch = 4,
};

// Which side of a contact a resolved event takes its curves, mass and surface parameters from.
enum Who : std::uint8_t {
  kWhoA = 0,
  kWhoB = 1,
  kWhoLighter = 2,  // pair override / default: the lighter object's material and mass, both sides' surface params
};

// ---------------------------------------------------------------------------------------
// PHYS chunk (spec/interface/bank-format.md §6.2), mirrored here because physics cannot see
// the contract's internal layer (snd/format/bank_format.hpp). Read with memcpy only.
// ---------------------------------------------------------------------------------------
struct PhysChunkHeader {
  std::uint32_t tableCount;
  std::uint32_t reserved;
  std::uint64_t tablesOffset;
};
static_assert(sizeof(PhysChunkHeader) == 16);
struct PhysTable {
  std::uint32_t id;
  std::uint32_t materialCount;
  std::uint64_t singleOffset;
  std::uint64_t pairOffset;
  std::uint64_t footstepOffset;
  std::uint32_t defaultEvent[4];
  std::uint64_t curveOffset;
  std::uint64_t reserved;
};
static_assert(sizeof(PhysTable) == 64);
static_assert(offsetof(PhysTable, materialCount) == 4 && offsetof(PhysTable, singleOffset) == 8 &&
              offsetof(PhysTable, pairOffset) == 16 && offsetof(PhysTable, footstepOffset) == 24 &&
              offsetof(PhysTable, defaultEvent) == 32 && offsetof(PhysTable, curveOffset) == 48 &&
              offsetof(PhysTable, reserved) == 56);
struct PhysCurves {
  float impulseToVelocity[4];
  float massToPitchCents[2];
  float speedToVolumeDb[2];
};
static_assert(sizeof(PhysCurves) == 32);

// ---------------------------------------------------------------------------------------
// Internal records
// ---------------------------------------------------------------------------------------
struct Key {
  std::uint64_t a, b, c, d;
};
inline bool SameKey(const Key& x, const Key& y) noexcept {
  return x.a == y.a && x.b == y.b && x.c == y.c && x.d == y.d;
}
inline std::uint64_t Mix(std::uint64_t h) noexcept {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}
inline std::uint64_t HashKey(const Key& k) noexcept {
  std::uint64_t h = Mix(k.a + 0x9e3779b97f4a7c15ull);
  h = Mix(h ^ (k.b + 0x632be59bd9b4e019ull));
  h = Mix(h ^ (k.c + 0x8cb92ba72f3d8dd7ull));
  return Mix(h ^ (k.d + 0x2545f4914f6cdd1dull));
}

using Curve = snd_physics_curve;
using Curves = snd_physics_curves;
using Entry = snd_physics_material_entry;

inline const Curve& CurveAt(const Curves& c, std::uint32_t i) noexcept {
  switch (i) {
    case kImpulseVolume: return c.impulse_to_volume;
    case kImpulseVelocity: return c.impulse_to_velocity;
    case kMassPitch: return c.mass_to_pitch;
    case kSpeedVolume: return c.speed_to_volume;
    default: return c.speed_to_pitch;
  }
}

struct Resolution {
  snd_physics_stage stage = SND_PHYSICS_STAGE_NOTHING;
  std::uint32_t n = 0;  // events to play (0..2)
  snd_id event[2] = {SND_ID_NONE, SND_ID_NONE};
  std::uint8_t who[2] = {kWhoA, kWhoA};
  bool unknown = false;
};

// One group (Impact / Break) or one slide report (Slide / Roll) of the current call.
struct Group {
  Key key;
  std::uint32_t rep;    // representative contact (index into the call's array)
  std::uint32_t count;  // contacts merged
  std::uint32_t kind;
  float value;          // max impulse (Impact / Break) or max relative speed (Slide / Roll)
  std::uint16_t matA, matB;    // sound materials of the representative (kUnknown = unknown)
  std::uint16_t gameA, gameB;  // game materials of the representative
  Resolution res;
};

struct CooldownEntry {
  Key key;
  std::uint64_t last;   // clock (us) of the last request
  std::uint64_t order;  // insertion order of the record (eviction ties: the first recorded goes)
  std::uint32_t stamp;  // call that recorded it
};

// A one-shot emitter kept until the audio side has applied its playbacks.
struct Held {
  snd_emitter_handle emitter;
  snd_playback_handle playback[2];
  float volume[2];      // the volume recorded on each playback (1 = none recorded)
};

struct Voice {
  snd_id event;
  snd_playback_handle handle;
  std::uint8_t who;
  float lastVolume, lastPitch;
  bool seen;  // its state was seen other than NONE (applied); NONE afterwards = stale
  std::uint32_t sentCount;
  snd_physics_param sent[kMaxRequestParams];  // parameters last recorded on the playback
};

struct Track {
  Key key;
  bool used;
  std::uint32_t kind;
  std::uint64_t start, last, seq;
  std::uint32_t reportStamp;  // call of the latest report
  std::uint32_t reportIdx;    // representative contact of this call's report (largest speed)
  float reportSpeed;
  snd_emitter_handle emitter;
  std::uint32_t voiceCount;
  Voice v[2];
  snd_physics_stage stage;
  bool unknown;
  std::uint16_t matA, matB, gameA, gameB;  // latest report (orientation of its representative)
  std::uint64_t objA, objB;
  float massA, massB, speed;
  snd_vec3 position;
  snd_vec3 sentPosition;  // the position last recorded on the emitter
};

// One play request (record source of the observer).
struct Request {
  snd_physics_source source;
  std::uint32_t kind;
  snd_physics_stage stage;
  std::uint32_t flags;
  snd_id event;
  std::uint32_t count;
  std::uint16_t matA, matB, gameA, gameB;
  float impulse, velocity, speed, mass, volume, pitch;
  snd_vec3 position;
  std::uint64_t objA, objB;
  std::uint32_t paramCount;
  snd_physics_param params[kMaxRequestParams];
};

}  // namespace

// ---------------------------------------------------------------------------------------
// The object
// ---------------------------------------------------------------------------------------
struct snd_phys {
  snd_system* sys;
  snd_allocator allocator;  // copy; allocate == NULL: malloc / free
  void* block;
  std::thread::id thread;
  bool inCallback;
  snd_physics_config cfg;
  std::uint64_t cooldownUs;
  std::uint64_t timeoutUs;

  // ---- table ----
  std::uint32_t nMax;
  std::uint32_t n;
  snd_id* single;            // [nMax * 4]
  snd_id* pair[kKinds];      // [nMax * nMax] each, index lo * n + hi
  snd_id* footMotion;        // [nMax * 6]
  snd_id* footAny;           // [nMax]
  snd_id footDefault;
  snd_id defaults[kKinds];
  Curves* curves;            // [nMax]
  Curves defaultCurves;
  Curves builtin;
  Curve weightCurve;
  Curve builtinWeight;
  float tableThreshold;

  // ---- game material map ----
  std::uint32_t mapCap;
  std::uint32_t mapCount;
  bool mapActive;
  Entry* map;  // [mapCap]

  // ---- time and budget ----
  std::uint64_t now;
  double carry;
  // ---- audio-side stall (D-125) ----
  bool haveBlock;              // lastBlock is valid
  std::uint64_t lastBlock;     // block_index seen by the previous call
  std::uint32_t sameBlockCalls;  // calls in a row that saw the same block_index
  bool stalled;
  bool haveProbeBlock;
  std::uint64_t probeBlock;    // block_index of the last release check that recorded
  std::uint32_t stamp;
  std::uint64_t seq;

  // ---- cooldown table ----
  std::uint32_t cdCap;
  std::uint32_t cdCount;
  std::uint32_t cdMask;
  CooldownEntry* cd;     // [cdCap]
  std::uint32_t* cdBucket;  // [cdMask + 1], entry index or kEmptyBucket

  std::uint64_t cdOrder;

  // ---- one-shot emitters held until their playbacks are applied (oldest first) ----
  std::uint32_t heldCap, heldCount;
  Held* held;  // [heldCap]

  // ---- tracking table ----
  std::uint32_t trCap;
  Track* tracks;  // [trCap]
  Group* fresh;   // [trCap] reports of keys that are not tracked yet (this call)
  std::uint32_t freshCount;

  // ---- work area of one call ----
  std::uint32_t gCap;
  std::uint32_t gMask;
  Group* groups;            // [gCap]
  std::uint32_t* gStamp;    // [gMask + 1]
  std::uint32_t* gIdx;      // [gMask + 1]
  std::uint32_t* order;     // [gCap]

  snd_physics_stats stats;
};

namespace {

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------
inline bool Finite(float v) noexcept { return std::isfinite(v); }
inline bool FiniteVec(const float* p) noexcept { return Finite(p[0]) && Finite(p[1]) && Finite(p[2]); }

// Seconds -> whole microseconds (round half away from zero), saturated.
std::uint64_t Micros(double seconds) noexcept {
  if (!(seconds > 0.0)) return 0;
  const double us = std::floor(seconds * 1e6 + 0.5);
  if (!(us < static_cast<double>(kMaxMicros))) return kMaxMicros;
  return static_cast<std::uint64_t>(us);
}

inline float SafeMass(float m) noexcept { return (Finite(m) && m > 0.0f) ? m : 0.0f; }

std::uint32_t NextPow2(std::uint64_t v) noexcept {
  std::uint64_t p = 1;
  while (p < v) p <<= 1;
  return static_cast<std::uint32_t>(p);
}

float Eval(const Curve& c, float x) noexcept {
  const std::uint32_t n = c.count;
  if (n == 0u) return 1.0f;
  if (!(x > c.points[0].x)) return c.points[0].y;  // NaN too
  if (x >= c.points[n - 1u].x) return c.points[n - 1u].y;
  for (std::uint32_t i = 0; i + 1u < n; ++i) {
    const snd_physics_curve_point& a = c.points[i];
    const snd_physics_curve_point& b = c.points[i + 1u];
    if (x < b.x) {
      const float dx = b.x - a.x;
      if (!(dx > 0.0f)) return b.y;
      return a.y + (b.y - a.y) * ((x - a.x) / dx);
    }
  }
  return c.points[n - 1u].y;
}

// What a curve's y means: a linear volume [0, 1000], a pitch ratio [1/16, 16], or a parameter
// value (velocity) |y| <= 1e6.
enum class YRange { Volume, Pitch, Value };
bool ValidCurve(const Curve& c, YRange range) noexcept {
  if (c.count > kMaxPoints) return false;
  for (std::uint32_t i = 0; i < c.count; ++i) {
    const float x = c.points[i].x;
    const float y = c.points[i].y;
    if (!Finite(x) || !Finite(y)) return false;
    if (i > 0u && x < c.points[i - 1u].x) return false;
    switch (range) {
      case YRange::Volume:
        if (y < 0.0f || y > 1000.0f) return false;
        break;
      case YRange::Pitch:
        if (y < 1.0f / 16.0f || y > 16.0f) return false;
        break;
      case YRange::Value:
        if (y < -1.0e6f || y > 1.0e6f) return false;
        break;
    }
  }
  return true;
}
bool ValidCurves(const Curves& c) noexcept {
  return ValidCurve(c.impulse_to_volume, YRange::Volume) && ValidCurve(c.impulse_to_velocity, YRange::Value) &&
         ValidCurve(c.mass_to_pitch, YRange::Pitch) && ValidCurve(c.speed_to_volume, YRange::Volume) &&
         ValidCurve(c.speed_to_pitch, YRange::Pitch);
}

inline float ClampVolume(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;
  return v > 1000.0f ? 1000.0f : v;
}
inline float ClampPitch(float p) noexcept {
  if (!(p >= 1.0f / 16.0f)) return 1.0f / 16.0f;
  return p > 16.0f ? 16.0f : p;
}

inline bool ReservedId(snd_id id) noexcept { return id >= SND_ID_RESERVED_FIRST && id <= SND_ID_RESERVED_LAST; }

// The PHYS mapping of one PhysCurves (documented in physics.h).
void MapPhysCurves(const PhysCurves& pc, Curves* out) noexcept {
  std::memset(out, 0, sizeof(*out));
  static const float kVol[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  static const float kVel[4] = {0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
  out->impulse_to_volume.count = 4u;
  out->impulse_to_velocity.count = 4u;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    out->impulse_to_volume.points[i] = {pc.impulseToVelocity[i], kVol[i]};
    out->impulse_to_velocity.points[i] = {pc.impulseToVelocity[i], kVel[i]};
  }
  out->mass_to_pitch.count = 8u;
  out->speed_to_volume.count = 8u;
  for (std::uint32_t i = 0; i < 8u; ++i) {
    const double f = static_cast<double>(i) / 7.0;
    const double cents = pc.massToPitchCents[0] + (static_cast<double>(pc.massToPitchCents[1]) - pc.massToPitchCents[0]) * f;
    const double db = pc.speedToVolumeDb[0] + (static_cast<double>(pc.speedToVolumeDb[1]) - pc.speedToVolumeDb[0]) * f;
    out->mass_to_pitch.points[i] = {static_cast<float>(std::pow(100.0, f)), static_cast<float>(std::pow(2.0, cents / 1200.0))};
    out->speed_to_volume.points[i] = {static_cast<float>(0.1 * std::pow(100.0, f)),
                                      static_cast<float>(std::pow(10.0, db / 20.0))};
  }
  out->speed_to_pitch.count = 0u;
}

// ---------------------------------------------------------------------------------------
// Observer and statistics
// ---------------------------------------------------------------------------------------
void Notify(snd_phys& p, const snd_physics_record& r) noexcept {
  if (p.cfg.observer == nullptr) return;
  p.inCallback = true;
  p.cfg.observer(p.cfg.observer_user, &r);
  p.inCallback = false;
}

snd_physics_record BaseRecord(const Request& q) noexcept {
  snd_physics_record r{};
  r.source = q.source;
  r.contact_kind = q.kind;
  r.stage = q.stage;
  r.flags = q.flags;
  r.event = q.event;
  r.count = q.count;
  r.material_a = q.matA;
  r.material_b = q.matB;
  r.game_material_a = q.gameA;
  r.game_material_b = q.gameB;
  r.impulse = q.impulse;
  r.velocity = q.velocity;
  r.speed = q.speed;
  r.mass = q.mass;
  r.volume = q.volume;
  r.pitch = q.pitch;
  r.position = q.position;
  r.object_a = q.objA;
  r.object_b = q.objB;
  r.result = SND_OK;
  r.param_count = q.paramCount;
  r.params = q.paramCount > 0u ? q.params : nullptr;
  return r;
}

void NotifyFallback(snd_phys& p, const Request& q) noexcept {
  snd_physics_record r = BaseRecord(q);
  r.kind = SND_PHYSICS_RECORD_FALLBACK;
  r.param_count = 0;
  r.params = nullptr;
  r.volume = 1.0f;
  r.pitch = 1.0f;
  Notify(p, r);
}

// Counts the stage of one contact resolution (pair / single / default / nothing (+ unknown)).
void CountStage(snd_phys& p, snd_physics_stage stage, bool unknown) noexcept {
  switch (stage) {
    case SND_PHYSICS_STAGE_PAIR: ++p.stats.resolved_pair; break;
    case SND_PHYSICS_STAGE_SINGLE: ++p.stats.fallback_pair_to_single; break;
    case SND_PHYSICS_STAGE_DEFAULT: ++p.stats.fallback_single_to_default; break;
    default: ++p.stats.fallback_none; break;
  }
  if (unknown) ++p.stats.fallback_unknown_material;
}

// ---------------------------------------------------------------------------------------
// Table and map lookups
// ---------------------------------------------------------------------------------------
// Game material -> sound material (kUnknown when unknown); *entry = the map entry or NULL.
std::uint16_t MapMaterial(const snd_phys& p, std::uint16_t game, const Entry** entry) noexcept {
  *entry = nullptr;
  if (!p.mapActive) return game < p.n ? game : kUnknown;
  if (game >= p.mapCount) return kUnknown;
  const Entry& e = p.map[game];
  *entry = &e;
  return e.sound_material < p.n ? e.sound_material : kUnknown;
}
const Entry* EntryOf(const snd_phys& p, std::uint16_t game) noexcept {
  const Entry* e = nullptr;
  (void)MapMaterial(p, game, &e);
  return e;
}

const Curve& EffectiveCurve(const snd_phys& p, std::uint16_t mat, std::uint32_t which) noexcept {
  if (mat != kUnknown && mat < p.n) {
    const Curve& c = CurveAt(p.curves[mat], which);
    if (c.count > 0u) return c;
  }
  const Curve& d = CurveAt(p.defaultCurves, which);
  if (d.count > 0u) return d;
  return CurveAt(p.builtin, which);
}
inline float CurveValue(const snd_phys& p, std::uint16_t mat, std::uint32_t which, float x) noexcept {
  return Eval(EffectiveCurve(p, mat, which), x);
}

Resolution Resolve(const snd_phys& p, std::uint32_t kind, std::uint16_t a, std::uint16_t b) noexcept {
  Resolution r;
  const bool ka = a != kUnknown;
  const bool kb = b != kUnknown;
  r.unknown = !ka || !kb;
  if (ka && kb) {
    const std::uint16_t lo = std::min(a, b);
    const std::uint16_t hi = std::max(a, b);
    const snd_id pe = p.pair[kind][static_cast<std::size_t>(lo) * p.n + hi];
    if (pe != SND_ID_NONE) {
      r.stage = SND_PHYSICS_STAGE_PAIR;
      r.n = 1;
      r.event[0] = pe;
      r.who[0] = kWhoLighter;
      return r;
    }
  }
  // Singles of the known materials (one unknown: the known side's only; no pair override).
  const snd_id sa = ka ? p.single[static_cast<std::size_t>(a) * kKinds + kind] : SND_ID_NONE;
  const snd_id sb = (kb && !(ka && a == b)) ? p.single[static_cast<std::size_t>(b) * kKinds + kind] : SND_ID_NONE;
  if (sa != SND_ID_NONE || sb != SND_ID_NONE) {
    r.stage = SND_PHYSICS_STAGE_SINGLE;
    if (sa != SND_ID_NONE) {
      r.event[r.n] = sa;
      r.who[r.n] = kWhoA;
      ++r.n;
    }
    if (sb != SND_ID_NONE) {
      r.event[r.n] = sb;
      r.who[r.n] = kWhoB;
      ++r.n;
    }
    return r;
  }
  const snd_id d = p.defaults[kind];
  if (d != SND_ID_NONE) {
    r.stage = SND_PHYSICS_STAGE_DEFAULT;
    r.n = 1;
    r.event[0] = d;
    r.who[0] = kWhoLighter;
  }
  return r;
}

// Appends a parameter (skipped for SND_ID_NONE); a repeated id keeps the larger value.
void AddParam(Request& q, snd_id id, float value) noexcept {
  if (id == SND_ID_NONE) return;
  for (std::uint32_t i = 0; i < q.paramCount; ++i) {
    if (q.params[i].id == id) {
      if (value > q.params[i].value) q.params[i].value = value;
      return;
    }
  }
  if (q.paramCount < kMaxRequestParams) q.params[q.paramCount++] = snd_physics_param{id, value};
}
void AddSurface(Request& q, const Entry* e) noexcept {
  if (e == nullptr) return;
  const std::uint32_t n = std::min<std::uint32_t>(e->param_count, kMaxSurface);
  for (std::uint32_t i = 0; i < n; ++i) AddParam(q, e->params[i].id, e->params[i].value);
}

// Material, mass and surface parameters of one resolved event of a contact.
struct Side {
  std::uint16_t mat;
  float mass;
};
Side SideOf(std::uint8_t who, std::uint16_t matA, std::uint16_t matB, float massA, float massB) noexcept {
  const float ma = SafeMass(massA);
  const float mb = SafeMass(massB);
  if (who == kWhoA) return Side{matA, ma};
  if (who == kWhoB) return Side{matB, mb};
  return mb < ma ? Side{matB, mb} : Side{matA, ma};
}
void AddSideSurface(const snd_phys& p, Request& q, std::uint8_t who, std::uint16_t gameA, std::uint16_t gameB) noexcept {
  if (who == kWhoA || who == kWhoLighter) AddSurface(q, EntryOf(p, gameA));
  if (who == kWhoB || who == kWhoLighter) AddSurface(q, EntryOf(p, gameB));
}

// ---------------------------------------------------------------------------------------
// Recording through the sonus C API
// ---------------------------------------------------------------------------------------
// Records the parameters, volume and pitch of a request on a playback. Returns the first failure.
snd_result ApplyPlayback(snd_phys& p, snd_playback_handle h, const Request& q) noexcept {
  snd_result first = SND_OK;
  auto note = [&](snd_result r) {
    if (r != SND_OK) {
      ++p.stats.api_failures;
      if (first == SND_OK) first = r;
    }
  };
  for (std::uint32_t i = 0; i < q.paramCount; ++i) note(snd_set_param_playback(p.sys, h, q.params[i].id, q.params[i].value));
  if (q.volume != 1.0f) note(snd_set_playback_volume(p.sys, h, q.volume));
  if (q.pitch != 1.0f) note(snd_set_playback_pitch(p.sys, h, q.pitch));
  return first;
}

// Positioned one-shots of one group (1 or 2 events at one position): emitter -> transform ->
// play -> parameters -> destroy the emitter (the playbacks keep the last transform);
// snd_play_at without parameters when no emitter can be used. Emits one PLAY record per event.
// Returns true when at least one play was recorded.
// Destroys held emitter i (keeps the order of the rest).
void DestroyHeld(snd_phys& p, std::uint32_t i) noexcept {
  const snd_result r = snd_destroy_emitter(p.sys, p.held[i].emitter);
  if (r != SND_OK && r != SND_ERR_INVALID_HANDLE) ++p.stats.api_failures;
  for (std::uint32_t j = i + 1u; j < p.heldCount; ++j) p.held[j - 1u] = p.held[j];
  --p.heldCount;
}

// True once the audio side has applied the play recorded on the emitter (or the playback is
// gone). Why destroying then keeps the position of the played voices: the play was recorded
// after the emitter's create and transform in the same arena, so the block that applied it
// created the emitter, and that block's Compute3D ran with the emitter alive (core computes 3D
// values for live emitters only, A-004 §1). A playback state other than NONE is read from a
// snapshot published at the end of such a block; the destroy recorded now is applied at a
// later block, so the voices already have their 3D values and keep them (sonus.h). The play
// and the destroy are never in one command batch, which loses the position even for an
// emitter alive since an earlier block (A-004 §4). Not covered (A-004 §5, core): child
// playbacks an event action starts later (`play` at timeMs > 0) begin after the destroy and
// play at the listener; physics does not hold the emitter for the length of the sound.
// NONE means "not in a published snapshot yet" or "ended and its slot returned": a recording
// call tells them apart without changing anything (the volume the playback already has):
// SND_ERR_INVALID_HANDLE = stale (D-29), the playback is over (it ended - played, not loaded,
// rejected - so its position no longer matters).
// The recording check runs only when `probe` (block_index moved since the last one, D-125);
// *probed tells that it ran.
bool Applied(snd_phys& p, snd_playback_handle h, float volume, bool probe, bool* probed) noexcept {
  if (h.value == SND_HANDLE_INVALID_VALUE) return true;
  if (snd_get_playback_state(p.sys, h) != SND_PLAYBACK_STATE_NONE) return true;
  if (!probe) return false;
  *probed = true;
  return snd_set_playback_volume(p.sys, h, volume) == SND_ERR_INVALID_HANDLE;
}

// Destroys the held emitters whose playbacks have all been applied (any order).
// Returns true when a recording check ran.
bool ReleaseHeld(snd_phys& p, bool probe) noexcept {
  bool probed = false;
  for (std::uint32_t i = 0; i < p.heldCount;) {
    const Held& h = p.held[i];
    if (Applied(p, h.playback[0], h.volume[0], probe, &probed) &&
        Applied(p, h.playback[1], h.volume[1], probe, &probed)) {
      DestroyHeld(p, i);
    } else {
      ++i;
    }
  }
  return probed;
}

// An emitter at `position` for one group; false when none can be had (held ring full, no
// emitter slot, transform refused). A full ring never destroys a held emitter early: that
// sound would lose its position (it would play at the listener at full volume); the new group
// is dropped instead.
bool AcquireEmitter(snd_phys& p, const snd_vec3& position, snd_emitter_handle* out) noexcept {
  *out = snd_emitter_handle{0};
  if (p.heldCount == p.heldCap) return false;
  const snd_emitter_handle e = snd_create_emitter(p.sys);
  if (e.value == SND_HANDLE_INVALID_VALUE) return false;
  if (snd_set_transform(p.sys, e, &position, nullptr, nullptr) != SND_OK) {
    ++p.stats.api_failures;
    (void)snd_destroy_emitter(p.sys, e);
    return false;
  }
  *out = e;
  return true;
}

// Plays the events of one group on the acquired emitter (one PLAY record each) and holds the
// emitter. Returns true when at least one play was recorded.
bool PlayPositioned(snd_phys& p, snd_emitter_handle e, Request* qs, std::uint32_t n) noexcept {
  Held h{};
  h.emitter = e;
  bool any = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    Request& q = qs[i];
    const snd_playback_handle ph = snd_play(p.sys, q.event, e, nullptr);
    snd_result r = SND_OK;
    if (ph.value == SND_HANDLE_INVALID_VALUE) {
      ++p.stats.api_failures;
      r = SND_ERR_LIMIT_REACHED;
    } else {
      any = true;
      (void)ApplyPlayback(p, ph, q);
    }
    h.playback[i] = ph;
    h.volume[i] = q.volume;
    snd_physics_record rec = BaseRecord(q);
    rec.kind = SND_PHYSICS_RECORD_PLAY;
    rec.playback = ph;
    rec.emitter = e;
    rec.result = r;
    Notify(p, rec);
  }
  if (n < 2u) h.volume[1] = 1.0f;
  p.held[p.heldCount++] = h;  // AcquireEmitter made room
  return any;
}

// ---------------------------------------------------------------------------------------
// Cooldown table (open addressing, linear probing, backward-shift deletion)
// ---------------------------------------------------------------------------------------
std::uint32_t CdFindBucket(const snd_phys& p, const Key& k) noexcept {
  std::uint32_t i = static_cast<std::uint32_t>(HashKey(k)) & p.cdMask;
  for (;;) {
    const std::uint32_t e = p.cdBucket[i];
    if (e == kEmptyBucket) return kEmptyBucket;
    if (SameKey(p.cd[e].key, k)) return i;
    i = (i + 1u) & p.cdMask;
  }
}
std::uint32_t CdFind(const snd_phys& p, const Key& k) noexcept {
  const std::uint32_t b = CdFindBucket(p, k);
  return b == kEmptyBucket ? kEmptyBucket : p.cdBucket[b];
}
void CdRemoveBucket(snd_phys& p, std::uint32_t i) noexcept {
  std::uint32_t j = i;
  for (;;) {
    p.cdBucket[i] = kEmptyBucket;
    for (;;) {
      j = (j + 1u) & p.cdMask;
      if (p.cdBucket[j] == kEmptyBucket) return;
      const std::uint32_t k = static_cast<std::uint32_t>(HashKey(p.cd[p.cdBucket[j]].key)) & p.cdMask;
      const bool inRange = i <= j ? (i < k && k <= j) : (i < k || k <= j);
      if (!inRange) break;
    }
    p.cdBucket[i] = p.cdBucket[j];
    i = j;
  }
}
void CdInsertBucket(snd_phys& p, std::uint32_t entry) noexcept {
  std::uint32_t i = static_cast<std::uint32_t>(HashKey(p.cd[entry].key)) & p.cdMask;
  while (p.cdBucket[i] != kEmptyBucket) i = (i + 1u) & p.cdMask;
  p.cdBucket[i] = entry;
}
// Records a request at `now`. A full table pushes out the oldest record (lowest slot on ties).
void CdRecord(snd_phys& p, const Key& k) noexcept {
  const std::uint32_t found = CdFind(p, k);
  if (found != kEmptyBucket) {
    p.cd[found].last = p.now;
    p.cd[found].stamp = p.stamp;
    return;
  }
  std::uint32_t slot;
  if (p.cdCount < p.cdCap) {
    slot = p.cdCount++;
  } else {
    slot = 0;
    for (std::uint32_t i = 1; i < p.cdCap; ++i) {
      const CooldownEntry& x = p.cd[i];
      const CooldownEntry& y = p.cd[slot];
      if (x.last < y.last || (x.last == y.last && x.order < y.order)) slot = i;
    }
    if (p.now - p.cd[slot].last < p.cooldownUs) ++p.stats.cooldown_evictions;
    CdRemoveBucket(p, CdFindBucket(p, p.cd[slot].key));
  }
  p.cd[slot].key = k;
  p.cd[slot].last = p.now;
  p.cd[slot].order = ++p.cdOrder;
  p.cd[slot].stamp = p.stamp;
  CdInsertBucket(p, slot);
}

// ---------------------------------------------------------------------------------------
// Tracking (Slide / Roll)
// ---------------------------------------------------------------------------------------
Request TrackRequest(const snd_phys& p, const Track& t, std::uint32_t vi) noexcept {
  Request q{};
  q.source = SND_PHYSICS_SOURCE_CONTACT;
  q.kind = t.kind;
  q.stage = t.stage;
  q.flags = t.unknown ? SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL : 0u;
  q.event = t.v[vi].event;
  q.count = 1;
  q.matA = t.matA;
  q.matB = t.matB;
  q.gameA = t.gameA;
  q.gameB = t.gameB;
  const Side s = SideOf(t.v[vi].who, t.matA, t.matB, t.massA, t.massB);
  q.speed = t.speed;
  q.mass = s.mass;
  q.volume = ClampVolume(CurveValue(p, s.mat, kSpeedVolume, t.speed));
  q.pitch = ClampPitch(CurveValue(p, s.mat, kSpeedPitch, t.speed));
  q.position = t.position;
  q.objA = t.objA;
  q.objB = t.objB;
  AddParam(q, p.cfg.params.speed, t.speed);
  AddParam(q, p.cfg.params.mass, s.mass);
  AddSideSurface(p, q, t.v[vi].who, t.gameA, t.gameB);
  return q;
}

// Starts voice vi of a track on its emitter. Emits START.
void StartVoice(snd_phys& p, Track& t, std::uint32_t vi, bool restart) noexcept {
  Voice& v = t.v[vi];
  Request q = TrackRequest(p, t, vi);
  if (restart) q.flags |= SND_PHYSICS_RECORD_FLAG_RESTART;
  v.seen = false;
  v.handle = snd_play(p.sys, v.event, t.emitter, nullptr);
  snd_result r = SND_OK;
  if (v.handle.value == SND_HANDLE_INVALID_VALUE) {
    ++p.stats.api_failures;
    r = SND_ERR_LIMIT_REACHED;
  } else {
    (void)ApplyPlayback(p, v.handle, q);
  }
  v.lastVolume = q.volume;
  v.lastPitch = q.pitch;
  v.sentCount = q.paramCount;
  for (std::uint32_t i = 0; i < q.paramCount; ++i) v.sent[i] = q.params[i];
  snd_physics_record rec = BaseRecord(q);
  rec.kind = SND_PHYSICS_RECORD_START;
  rec.playback = v.handle;
  rec.emitter = t.emitter;
  rec.result = r;
  Notify(p, rec);
}

// Stops every voice of a track with the fade and destroys its emitter. Emits STOP per voice.
void StopTrack(snd_phys& p, Track& t, snd_physics_stop_reason reason) noexcept {
  for (std::uint32_t vi = 0; vi < t.voiceCount; ++vi) {
    Voice& v = t.v[vi];
    snd_result r = SND_OK;
    if (v.handle.value != SND_HANDLE_INVALID_VALUE) {
      r = snd_stop(p.sys, v.handle, p.cfg.tracking_fade_seconds, nullptr);
      // A playback that already ended is not a failure of the stop.
      if (r != SND_OK && r != SND_ERR_INVALID_HANDLE) ++p.stats.api_failures;
    }
    Request q = TrackRequest(p, t, vi);
    q.paramCount = 0;
    snd_physics_record rec = BaseRecord(q);
    rec.kind = SND_PHYSICS_RECORD_STOP;
    rec.stop_reason = reason;
    rec.playback = v.handle;
    rec.emitter = t.emitter;
    rec.volume = v.lastVolume;
    rec.pitch = v.lastPitch;
    rec.result = r;
    Notify(p, rec);
    v.handle = snd_playback_handle{0};
  }
  if (t.emitter.value != SND_HANDLE_INVALID_VALUE) {
    const snd_result r = snd_destroy_emitter(p.sys, t.emitter);
    if (r != SND_OK && r != SND_ERR_INVALID_HANDLE) ++p.stats.api_failures;
  }
  t.emitter = snd_emitter_handle{0};
  t.used = false;
}

// Updates voice vi with the latest report. Returns false when the playback turned out to be stale.
bool UpdateVoice(snd_phys& p, Track& t, std::uint32_t vi) noexcept {
  Voice& v = t.v[vi];
  if (v.handle.value == SND_HANDLE_INVALID_VALUE) return false;
  // Stale without recording anything: ENDED, or NONE after it was once seen applied.
  const snd_playback_state st = snd_get_playback_state(p.sys, v.handle);
  if (st == SND_PLAYBACK_STATE_ENDED) return false;
  if (st != SND_PLAYBACK_STATE_NONE) {
    v.seen = true;
  } else if (v.seen) {
    return false;
  }
  const Request q = TrackRequest(p, t, vi);
  // Only the values that changed since they were last recorded (D-125); a record refused as
  // stale (D-29) also tells an ended playback.
  bool stale = false;
  snd_result first = SND_OK;
  auto note = [&](snd_result r) {
    if (r == SND_ERR_INVALID_HANDLE) {
      stale = true;
    } else if (r != SND_OK) {
      ++p.stats.api_failures;
      if (first == SND_OK) first = r;
    }
  };
  if (q.volume != v.lastVolume) note(snd_set_playback_volume(p.sys, v.handle, q.volume));
  if (!stale && q.pitch != v.lastPitch) note(snd_set_playback_pitch(p.sys, v.handle, q.pitch));
  // Parameters whose value changed (or that are new).
  for (std::uint32_t i = 0; i < q.paramCount && !stale; ++i) {
    bool same = false;
    for (std::uint32_t j = 0; j < v.sentCount; ++j) {
      if (v.sent[j].id == q.params[i].id) {
        same = v.sent[j].value == q.params[i].value;
        break;
      }
    }
    if (!same) note(snd_set_param_playback(p.sys, v.handle, q.params[i].id, q.params[i].value));
  }
  if (stale) return false;
  v.lastVolume = q.volume;
  v.lastPitch = q.pitch;
  v.sentCount = q.paramCount;
  for (std::uint32_t i = 0; i < q.paramCount; ++i) v.sent[i] = q.params[i];
  snd_physics_record rec = BaseRecord(q);
  rec.kind = SND_PHYSICS_RECORD_UPDATE;
  rec.playback = v.handle;
  rec.emitter = t.emitter;
  rec.result = first;
  Notify(p, rec);
  return true;
}

// ---------------------------------------------------------------------------------------
// Entry checks shared by the functions
// ---------------------------------------------------------------------------------------
snd_result Enter(const snd_phys* p) noexcept {
  if (p == nullptr) return SND_ERR_INVALID_ARGUMENT;
  if (std::this_thread::get_id() != p->thread) return SND_ERR_WRONG_THREAD;
  if (p->inCallback) return SND_ERR_UNSUPPORTED;
  return SND_OK;
}

// ---------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------
bool ReadConfig(const snd_physics_config* in, snd_physics_config* out) noexcept {
  snd_physics_config_init(out);
  if (in == nullptr) return false;
  // A larger struct_size (a newer header) is accepted and only the known part is read, like
  // snd_create (NormalizeConfig); a shorter one gets defaults for the rest.
  if (in->struct_size < 8u || (in->struct_size % 4u) != 0u) return false;
  // version changes only with an incompatible change (additions grow struct_size): it must match.
  if (in->version != SND_PHYSICS_CONFIG_VERSION) return false;
  std::memcpy(out, in, std::min<std::size_t>(in->struct_size, sizeof(snd_physics_config)));
  out->struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_config));
  out->version = SND_PHYSICS_CONFIG_VERSION;
  const snd_physics_config& c = *out;
  auto nonneg = [](float v) { return Finite(v) && v >= 0.0f; };
  if (!nonneg(c.budget_per_second) || !(Finite(c.grid_size) && c.grid_size > 0.0f) || !nonneg(c.cooldown_seconds) ||
      !nonneg(c.impulse_threshold) || !nonneg(c.min_slide_speed) || !nonneg(c.tracking_timeout_seconds) ||
      !nonneg(c.tracking_fade_seconds) || !(Finite(c.max_dt_seconds) && c.max_dt_seconds > 0.0f)) {
    return false;
  }
  auto cap = [](std::uint32_t v) { return v >= 1u && v <= SND_PHYSICS_MAX_CAPACITY; };
  if (!cap(c.max_tracked_pairs) || !cap(c.max_cooldowns) || !cap(c.max_groups_per_call) || !cap(c.max_game_materials) ||
      !cap(c.max_held_emitters) ||
      c.max_materials < 1u || c.max_materials > SND_PHYSICS_MAX_MATERIALS_LIMIT || c.max_game_materials > 0x10000u) {
    return false;
  }
  const snd_id ids[] = {c.params.impulse, c.params.velocity, c.params.count, c.params.speed,
                        c.params.mass, c.params.motion, c.params.foot_material, c.params.surface_weight};
  for (snd_id id : ids) {
    if (ReservedId(id)) return false;
  }
  return true;
}

// Memory layout of the block (all offsets 64-aligned).
struct Layout {
  std::size_t bytes = 0;
  std::size_t Take(std::size_t n) noexcept {
    const std::size_t at = (bytes + 63u) & ~static_cast<std::size_t>(63u);
    bytes = at + n;
    return at;
  }
};

void ClearTable(snd_phys& p) noexcept {
  const std::size_t nm = p.nMax;
  std::memset(p.single, 0, nm * kKinds * sizeof(snd_id));
  for (std::uint32_t k = 0; k < kKinds; ++k) std::memset(p.pair[k], 0, nm * nm * sizeof(snd_id));
  std::memset(p.footMotion, 0, nm * kMotions * sizeof(snd_id));
  std::memset(p.footAny, 0, nm * sizeof(snd_id));
  std::memset(p.curves, 0, nm * sizeof(Curves));
  std::memset(&p.defaultCurves, 0, sizeof(Curves));
  std::memset(&p.weightCurve, 0, sizeof(Curve));
  p.footDefault = SND_ID_NONE;
  for (std::uint32_t k = 0; k < kKinds; ++k) p.defaults[k] = SND_ID_NONE;
  p.tableThreshold = 0.0f;
  p.n = 0;
}

bool ValidEntry(const Entry& e) noexcept {
  if (e.param_count > kMaxSurface) return false;
  for (std::uint32_t i = 0; i < e.param_count; ++i) {
    const snd_id id = e.params[i].id;
    if (id == SND_ID_NONE || ReservedId(id) || !Finite(e.params[i].value)) return false;
    for (std::uint32_t j = 0; j < i; ++j) {
      if (e.params[j].id == id) return false;
    }
  }
  return true;
}
inline Entry IdentityEntry(std::uint32_t g) noexcept {
  Entry e{};
  e.sound_material = static_cast<std::uint16_t>(g);
  return e;
}

// PHYS: validates everything, then replaces the table.
snd_result LoadPhys(snd_phys& p, const void* data, std::uint64_t bytes, snd_id table) noexcept {
  if (data == nullptr || bytes < sizeof(PhysChunkHeader)) return SND_ERR_CORRUPT;
  const std::uint8_t* base = static_cast<const std::uint8_t*>(data);
  auto inRange = [&](std::uint64_t off, std::uint64_t len, std::uint64_t align) {
    return off % align == 0u && off <= bytes && len <= bytes - off;
  };
  PhysChunkHeader h;
  std::memcpy(&h, base, sizeof(h));
  if (h.tablesOffset % 8u != 0u || h.tablesOffset > bytes ||
      std::uint64_t{h.tableCount} > (bytes - h.tablesOffset) / sizeof(PhysTable)) {
    return SND_ERR_CORRUPT;
  }
  PhysTable t{};
  bool found = false;
  for (std::uint32_t i = 0; i < h.tableCount && !found; ++i) {
    std::memcpy(&t, base + h.tablesOffset + std::uint64_t{i} * sizeof(PhysTable), sizeof(t));
    found = t.id == table;
  }
  if (!found) return SND_ERR_NOT_FOUND;
  const std::uint64_t n = t.materialCount;
  if (n >= kUnknown) return SND_ERR_CORRUPT;  // material numbers are u16, 0xFFFF is "unknown"
  if (!inRange(t.singleOffset, n * kKinds * 4u, 4u) || !inRange(t.pairOffset, n * n * 4u, 4u) ||
      !inRange(t.footstepOffset, n * 4u, 4u) || !inRange(t.curveOffset, n * sizeof(PhysCurves), 4u)) {
    return SND_ERR_CORRUPT;
  }
  float threshold = 0.0f;
  for (std::uint64_t m = 0; m < n; ++m) {
    PhysCurves pc;
    std::memcpy(&pc, base + t.curveOffset + m * sizeof(PhysCurves), sizeof(pc));
    const float* all = pc.impulseToVelocity;
    for (std::uint32_t i = 0; i < 4u; ++i) {
      if (!Finite(all[i]) || (i > 0u && all[i] < all[i - 1u])) return SND_ERR_CORRUPT;
    }
    // Ranges keep the mapped values finite in float: 2^(4800/1200) = 16, 10^(200/20) = 1e10.
    for (float v : pc.massToPitchCents) {
      if (!Finite(v) || v < -4800.0f || v > 4800.0f) return SND_ERR_CORRUPT;
    }
    for (float v : pc.speedToVolumeDb) {
      if (!Finite(v) || v < -200.0f || v > 200.0f) return SND_ERR_CORRUPT;
    }
    threshold = m == 0u ? pc.impulseToVelocity[0] : std::min(threshold, pc.impulseToVelocity[0]);
  }
  if (n > p.nMax) return SND_ERR_LIMIT_REACHED;
  // Commit.
  ClearTable(p);
  p.n = static_cast<std::uint32_t>(n);
  std::memcpy(p.single, base + t.singleOffset, static_cast<std::size_t>(n * kKinds * 4u));
  for (std::uint32_t k = 0; k < kKinds; ++k) std::memcpy(p.pair[k], base + t.pairOffset, static_cast<std::size_t>(n * n * 4u));
  std::memcpy(p.footAny, base + t.footstepOffset, static_cast<std::size_t>(n * 4u));  // PHYS has one footstep column
  for (std::uint32_t k = 0; k < kKinds; ++k) p.defaults[k] = t.defaultEvent[k];
  for (std::uint64_t m = 0; m < n; ++m) {
    PhysCurves pc;
    std::memcpy(&pc, base + t.curveOffset + m * sizeof(PhysCurves), sizeof(pc));
    MapPhysCurves(pc, &p.curves[m]);
  }
  p.tableThreshold = threshold > 0.0f ? threshold : 0.0f;
  return SND_OK;
}

snd_result LoadBankTable(snd_phys& p, snd_bank_handle bank, snd_id table) noexcept {
  if (table == SND_ID_NONE) return SND_ERR_INVALID_ARGUMENT;
  const void* data = nullptr;
  std::uint64_t bytes = 0;
  const snd_result r = snd_get_bank_chunk(p.sys, bank, kFourccPhys, &data, &bytes);
  if (r != SND_OK) return r;
  return LoadPhys(p, data, bytes, table);
}

// ---------------------------------------------------------------------------------------
// Create / destroy
// ---------------------------------------------------------------------------------------
snd_result Create(snd_system* sys, const snd_physics_config& cfg, const snd_allocator* allocator, snd_phys** out) noexcept {
  const std::size_t nm = cfg.max_materials;
  const std::uint32_t cdBuckets = NextPow2(std::uint64_t{cfg.max_cooldowns} * 2u);
  const std::uint32_t gBuckets = NextPow2(std::uint64_t{cfg.max_groups_per_call} * 2u);
  Layout L;
  const std::size_t oPhys = L.Take(sizeof(snd_phys));
  const std::size_t oSingle = L.Take(nm * kKinds * sizeof(snd_id));
  std::size_t oPair[kKinds];
  for (std::uint32_t k = 0; k < kKinds; ++k) oPair[k] = L.Take(nm * nm * sizeof(snd_id));
  const std::size_t oFootMotion = L.Take(nm * kMotions * sizeof(snd_id));
  const std::size_t oFootAny = L.Take(nm * sizeof(snd_id));
  const std::size_t oCurves = L.Take(nm * sizeof(Curves));
  const std::size_t oMap = L.Take(std::size_t{cfg.max_game_materials} * sizeof(Entry));
  const std::size_t oCd = L.Take(std::size_t{cfg.max_cooldowns} * sizeof(CooldownEntry));
  const std::size_t oCdBucket = L.Take(std::size_t{cdBuckets} * sizeof(std::uint32_t));
  const std::size_t oTracks = L.Take(std::size_t{cfg.max_tracked_pairs} * sizeof(Track));
  const std::size_t oFresh = L.Take(std::size_t{cfg.max_tracked_pairs} * sizeof(Group));
  const std::size_t oHeld = L.Take(std::size_t{cfg.max_held_emitters} * sizeof(Held));
  const std::size_t oGroups = L.Take(std::size_t{cfg.max_groups_per_call} * sizeof(Group));
  const std::size_t oGStamp = L.Take(std::size_t{gBuckets} * sizeof(std::uint32_t));
  const std::size_t oGIdx = L.Take(std::size_t{gBuckets} * sizeof(std::uint32_t));
  const std::size_t oOrder = L.Take(std::size_t{cfg.max_groups_per_call} * sizeof(std::uint32_t));
  const std::size_t total = L.Take(0);

  void* block = nullptr;
  snd_allocator a{};
  if (allocator != nullptr) {
    if (allocator->allocate == nullptr || allocator->deallocate == nullptr) return SND_ERR_INVALID_ARGUMENT;
    a = *allocator;
    block = a.allocate(a.user, total, 64u, SND_MEM_MISC);
  } else {
    block = std::malloc(total + 64u);  // aligned by hand below
  }
  if (block == nullptr) return SND_ERR_OUT_OF_BUDGET;
  std::uint8_t* b = static_cast<std::uint8_t*>(block);
  if (allocator == nullptr) {
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(b);
    b += (64u - (at & 63u)) & 63u;
  }
  std::memset(b, 0, total);
  snd_phys* p = ::new (static_cast<void*>(b + oPhys)) snd_phys();  // placement: no allocation
  p->sys = sys;
  p->allocator = a;
  p->block = block;
  p->thread = std::this_thread::get_id();
  p->inCallback = false;
  p->cfg = cfg;
  p->cooldownUs = Micros(cfg.cooldown_seconds);
  p->timeoutUs = Micros(cfg.tracking_timeout_seconds);
  p->nMax = cfg.max_materials;
  p->single = reinterpret_cast<snd_id*>(b + oSingle);
  for (std::uint32_t k = 0; k < kKinds; ++k) p->pair[k] = reinterpret_cast<snd_id*>(b + oPair[k]);
  p->footMotion = reinterpret_cast<snd_id*>(b + oFootMotion);
  p->footAny = reinterpret_cast<snd_id*>(b + oFootAny);
  p->curves = reinterpret_cast<Curves*>(b + oCurves);
  p->mapCap = cfg.max_game_materials;
  p->map = reinterpret_cast<Entry*>(b + oMap);
  p->cdCap = cfg.max_cooldowns;
  p->cdMask = cdBuckets - 1u;
  p->cd = reinterpret_cast<CooldownEntry*>(b + oCd);
  p->cdBucket = reinterpret_cast<std::uint32_t*>(b + oCdBucket);
  for (std::uint32_t i = 0; i < cdBuckets; ++i) p->cdBucket[i] = kEmptyBucket;
  p->trCap = cfg.max_tracked_pairs;
  p->tracks = reinterpret_cast<Track*>(b + oTracks);
  p->fresh = reinterpret_cast<Group*>(b + oFresh);
  p->heldCap = cfg.max_held_emitters;
  p->held = reinterpret_cast<Held*>(b + oHeld);
  p->gCap = cfg.max_groups_per_call;
  p->gMask = gBuckets - 1u;
  p->groups = reinterpret_cast<Group*>(b + oGroups);
  p->gStamp = reinterpret_cast<std::uint32_t*>(b + oGStamp);
  p->gIdx = reinterpret_cast<std::uint32_t*>(b + oGIdx);
  p->order = reinterpret_cast<std::uint32_t*>(b + oOrder);
  ClearTable(*p);
  // Built-in curves: the PHYS mapping of the bank format's default PhysCurves.
  MapPhysCurves(PhysCurves{{0.1f, 1.0f, 10.0f, 100.0f}, {0.0f, 0.0f}, {-30.0f, 0.0f}}, &p->builtin);
  p->builtin.speed_to_pitch.count = 1u;
  p->builtin.speed_to_pitch.points[0] = {0.0f, 1.0f};
  p->builtinWeight.count = 2u;
  p->builtinWeight.points[0] = {0.0f, 0.0f};
  p->builtinWeight.points[1] = {1.0f, 1.0f};
  p->stats.struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_stats));
  *out = p;
  return SND_OK;
}

void Destroy(snd_phys* p) noexcept {
  // NULL, another thread, or a call from inside the observer (the object is in use): nothing
  // (the object stays alive and can be destroyed again on its thread).
  if (p == nullptr || p->inCallback || std::this_thread::get_id() != p->thread) return;
  for (std::uint32_t i = 0; i < p->trCap; ++i) {
    if (p->tracks[i].used) StopTrack(*p, p->tracks[i], SND_PHYSICS_STOP_DESTROYED);
  }
  while (p->heldCount > 0u) DestroyHeld(*p, p->heldCount - 1u);
  const snd_allocator a = p->allocator;
  void* block = p->block;
  if (a.deallocate != nullptr) {
    a.deallocate(a.user, block, SND_MEM_MISC);
  } else {
    std::free(block);
  }
}

// ---------------------------------------------------------------------------------------
// SubmitContacts (9.4, 9.5)
// ---------------------------------------------------------------------------------------
inline std::int64_t Cell(float pos, float grid) noexcept {
  const float q = pos / grid;  // in float: k * grid is cell k (physics.h)
  const double f = std::floor(static_cast<double>(q));
  constexpr double kLim = 4.0e18;
  if (!(f > -kLim)) return static_cast<std::int64_t>(-kLim);
  if (!(f < kLim)) return static_cast<std::int64_t>(kLim);
  return static_cast<std::int64_t>(f);
}

inline std::uint64_t MatKind(std::uint16_t a, std::uint16_t b, std::uint32_t kind, std::uint64_t ns) noexcept {
  const std::uint16_t lo = std::min(a, b);
  const std::uint16_t hi = std::max(a, b);
  return std::uint64_t{lo} | (std::uint64_t{hi} << 16) | (std::uint64_t{kind} << 32) | (ns << 40);
}

// Group lookup in the call's hash: the group index, or the empty bucket | 0x80000000.
std::uint32_t FindGroup(const snd_phys& p, const Key& k) noexcept {
  std::uint32_t i = static_cast<std::uint32_t>(HashKey(k)) & p.gMask;
  while (p.gStamp[i] == p.stamp) {
    if (SameKey(p.groups[p.gIdx[i]].key, k)) return p.gIdx[i];
    i = (i + 1u) & p.gMask;
  }
  return i | 0x80000000u;  // empty bucket (high bit marks "not found")
}

void PlayGroups(snd_phys& p, const snd_contact* contacts, std::uint32_t groupCount, std::uint64_t frameBudget) noexcept {
  std::uint32_t candidates = 0;
  for (std::uint32_t gi = 0; gi < groupCount; ++gi) {
    Group& g = p.groups[gi];
    if (g.kind != SND_CONTACT_IMPACT && g.kind != SND_CONTACT_BREAK) continue;
    g.res = Resolve(p, g.kind, g.matA, g.matB);
    if (g.res.stage == SND_PHYSICS_STAGE_NOTHING) {
      ++p.stats.dropped_no_event;
      CountStage(p, SND_PHYSICS_STAGE_NOTHING, g.res.unknown);
      const snd_contact& c = contacts[g.rep];
      Request q{};
      q.source = SND_PHYSICS_SOURCE_CONTACT;
      q.kind = g.kind;
      q.stage = SND_PHYSICS_STAGE_NOTHING;
      q.flags = g.res.unknown ? SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL : 0u;
      q.count = g.count;
      q.matA = g.matA;
      q.matB = g.matB;
      q.gameA = g.gameA;
      q.gameB = g.gameB;
      q.impulse = g.value;
      q.speed = c.relative_speed;
      q.position = snd_vec3{c.position[0], c.position[1], c.position[2]};
      q.objA = c.object_a;
      q.objB = c.object_b;
      NotifyFallback(p, q);
      continue;
    }
    // Step 3: cooldown of the representative's objects.
    const snd_contact& c = contacts[g.rep];
    Key ck{std::min(c.object_a, c.object_b), std::max(c.object_a, c.object_b), 0, MatKind(g.matA, g.matB, g.kind, 3u)};
    const std::uint32_t e = CdFind(p, ck);
    if (e != kEmptyBucket && p.now - p.cd[e].last < p.cooldownUs) {
      ++p.stats.dropped_cooldown;
      continue;
    }
    p.order[candidates++] = gi;
  }
  // Step 4: Break first, then impulse descending, then the representative's array order.
  const Group* groups = p.groups;
  std::sort(p.order, p.order + candidates, [groups](std::uint32_t x, std::uint32_t y) {
    const Group& a = groups[x];
    const Group& b = groups[y];
    const bool ab = a.kind == SND_CONTACT_BREAK;
    const bool bb = b.kind == SND_CONTACT_BREAK;
    if (ab != bb) return ab;
    if (a.value != b.value) return a.value > b.value;
    return a.rep < b.rep;
  });
  std::uint64_t used = 0;
  for (std::uint32_t oi = 0; oi < candidates; ++oi) {
    Group& g = p.groups[p.order[oi]];
    const snd_contact& c = contacts[g.rep];
    Key ck{std::min(c.object_a, c.object_b), std::max(c.object_a, c.object_b), 0, MatKind(g.matA, g.matB, g.kind, 3u)};
    // A cooldown key requested earlier in this same call (another cell of the same objects).
    if (p.cooldownUs > 0u) {
      const std::uint32_t e = CdFind(p, ck);
      if (e != kEmptyBucket && p.cd[e].stamp == p.stamp) {
        ++p.stats.dropped_cooldown;
        continue;
      }
    }
    if (used >= frameBudget) {
      ++p.stats.dropped_budget;
      continue;
    }
    ++used;  // a group without an emitter still uses its place in the budget
    snd_emitter_handle emitter{0};
    const snd_vec3 position{c.position[0], c.position[1], c.position[2]};
    // Audio side stalled (D-125): no emitter is made, nothing is recorded.
    const bool haveEmitter = !p.stalled && AcquireEmitter(p, position, &emitter);
    Request base{};
    base.source = SND_PHYSICS_SOURCE_CONTACT;
    base.kind = g.kind;
    base.stage = g.res.stage;
    base.flags = g.res.unknown ? SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL : 0u;
    base.count = g.count;
    base.matA = g.matA;
    base.matB = g.matB;
    base.gameA = g.gameA;
    base.gameB = g.gameB;
    base.impulse = g.value;
    base.speed = c.relative_speed;
    base.position = snd_vec3{c.position[0], c.position[1], c.position[2]};
    base.objA = c.object_a;
    base.objB = c.object_b;
    if (!haveEmitter) {
      // Not played: no position-less fallback (a distant hit must not sound at the listener).
      if (p.stalled) {
        ++p.stats.dropped_audio_stalled;
      } else {
        ++p.stats.dropped_no_emitter;
      }
      snd_physics_record rec = BaseRecord(base);
      rec.kind = SND_PHYSICS_RECORD_DROP;
      rec.event = g.res.event[0];
      rec.volume = 1.0f;
      rec.pitch = 1.0f;
      rec.result = SND_ERR_LIMIT_REACHED;
      Notify(p, rec);
      continue;
    }
    ++p.stats.group_requests;
    CountStage(p, g.res.stage, g.res.unknown);
    if (g.res.stage != SND_PHYSICS_STAGE_PAIR) {
      Request fb = base;
      fb.event = g.res.event[0];
      NotifyFallback(p, fb);
    }
    Request qs[2];
    for (std::uint32_t k = 0; k < g.res.n; ++k) {
      Request& q = qs[k];
      q = base;
      q.event = g.res.event[k];
      const Side s = SideOf(g.res.who[k], g.matA, g.matB, c.mass_a, c.mass_b);
      q.mass = s.mass;
      q.velocity = CurveValue(p, s.mat, kImpulseVelocity, g.value);
      q.volume = ClampVolume(CurveValue(p, s.mat, kImpulseVolume, g.value));
      q.pitch = ClampPitch(CurveValue(p, s.mat, kMassPitch, s.mass));
      AddParam(q, p.cfg.params.impulse, g.value);
      AddParam(q, p.cfg.params.velocity, q.velocity);
      AddParam(q, p.cfg.params.count, static_cast<float>(g.count));
      AddParam(q, p.cfg.params.speed, Finite(c.relative_speed) ? c.relative_speed : 0.0f);
      AddParam(q, p.cfg.params.mass, s.mass);
      AddSideSurface(p, q, g.res.who[k], g.gameA, g.gameB);
      if (g.kind == SND_CONTACT_BREAK) {
        ++p.stats.requested_break;
      } else {
        ++p.stats.requested_impact;
      }
    }
    const bool any = PlayPositioned(p, emitter, qs, g.res.n);
    if (any) CdRecord(p, ck);
  }
}

Track* FindTrack(snd_phys& p, const Key& k) noexcept {
  for (std::uint32_t i = 0; i < p.trCap; ++i) {
    if (p.tracks[i].used && SameKey(p.tracks[i].key, k)) return &p.tracks[i];
  }
  return nullptr;
}

// Step (1): the reports of this call to tracked keys (no work area: kept in the entries).
void UpdateTracked(snd_phys& p, const snd_contact* contacts) noexcept {
  for (std::uint32_t ti = 0; ti < p.trCap; ++ti) {
    Track* t = &p.tracks[ti];
    if (!t->used || t->reportStamp != p.stamp) continue;
    const snd_contact& c = contacts[t->reportIdx];
    const Entry* ea = nullptr;
    const Entry* eb = nullptr;
    const std::uint16_t ma = MapMaterial(p, c.material_a, &ea);
    const std::uint16_t mb = MapMaterial(p, c.material_b, &eb);
    t->last = p.now;
    // Keep the orientation of the start (voice A stays the material / object A).
    const bool swap = (c.object_a != c.object_b) ? (c.object_a == t->objB && c.object_b == t->objA)
                                                 : (ma != mb && ma == t->matB && mb == t->matA);
    t->matA = swap ? mb : ma;
    t->matB = swap ? ma : mb;
    t->gameA = swap ? c.material_b : c.material_a;
    t->gameB = swap ? c.material_a : c.material_b;
    t->objA = swap ? c.object_b : c.object_a;
    t->objB = swap ? c.object_a : c.object_b;
    t->massA = swap ? c.mass_b : c.mass_a;
    t->massB = swap ? c.mass_a : c.mass_b;
    t->speed = t->reportSpeed;
    t->position = snd_vec3{c.position[0], c.position[1], c.position[2]};
    if (p.stalled) {
      // Audio side stalled (D-125): the entry keeps the latest values, nothing is recorded;
      // the first report after the stall records them.
      ++p.stats.tracking_updated;
      continue;
    }
    if (t->position.x != t->sentPosition.x || t->position.y != t->sentPosition.y ||
        t->position.z != t->sentPosition.z) {
      if (snd_set_transform(p.sys, t->emitter, &t->position, nullptr, nullptr) != SND_OK) {
        ++p.stats.api_failures;
      } else {
        t->sentPosition = t->position;
      }
    }
    bool restarted = false;
    for (std::uint32_t vi = 0; vi < t->voiceCount; ++vi) {
      if (UpdateVoice(p, *t, vi)) continue;
      // Ended by itself: STOP (ENDED) then START again on the same emitter.
      Request q = TrackRequest(p, *t, vi);
      snd_physics_record rec = BaseRecord(q);
      rec.kind = SND_PHYSICS_RECORD_STOP;
      rec.stop_reason = SND_PHYSICS_STOP_ENDED;
      rec.param_count = 0;
      rec.params = nullptr;
      rec.playback = t->v[vi].handle;
      rec.emitter = t->emitter;
      rec.volume = t->v[vi].lastVolume;
      rec.pitch = t->v[vi].lastPitch;
      Notify(p, rec);
      StartVoice(p, *t, vi, true);
      restarted = true;
    }
    if (restarted) {
      ++p.stats.tracking_restarted;
    } else {
      ++p.stats.tracking_updated;
    }
  }
}

// Step (2): entries not reported in this call whose last report is >= timeout ago.
void Timeouts(snd_phys& p) noexcept {
  if (p.stalled) return;  // D-125: at the first call after the stall
  for (std::uint32_t i = 0; i < p.trCap; ++i) {
    Track& t = p.tracks[i];
    if (!t.used || t.reportStamp == p.stamp) continue;
    if (p.now - t.last >= p.timeoutUs) {
      ++p.stats.tracking_timeouts;
      StopTrack(p, t, SND_PHYSICS_STOP_TIMEOUT);
    }
  }
}

// Step (3): keys that are not tracked yet. Eviction only when the table is full of entries
// that have not timed out (step 2 ran first) and were not reported in this call.
void StartFresh(snd_phys& p, const snd_contact* contacts) noexcept {
  if (p.stalled) {  // D-125: new keys are not started while the audio side is stalled
    p.stats.tracking_dropped += p.freshCount;
    return;
  }
  for (std::uint32_t fi = 0; fi < p.freshCount; ++fi) {
    const Group& g = p.fresh[fi];
    const snd_contact& c = contacts[g.rep];
    const Resolution res = Resolve(p, g.kind, g.matA, g.matB);
    if (res.stage == SND_PHYSICS_STAGE_NOTHING) {
      CountStage(p, SND_PHYSICS_STAGE_NOTHING, res.unknown);
      Request q{};
      q.source = SND_PHYSICS_SOURCE_CONTACT;
      q.kind = g.kind;
      q.stage = SND_PHYSICS_STAGE_NOTHING;
      q.flags = res.unknown ? SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL : 0u;
      q.count = 1;
      q.matA = g.matA;
      q.matB = g.matB;
      q.gameA = g.gameA;
      q.gameB = g.gameB;
      q.speed = g.value;
      q.position = snd_vec3{c.position[0], c.position[1], c.position[2]};
      q.objA = c.object_a;
      q.objB = c.object_b;
      NotifyFallback(p, q);
      continue;
    }
    std::uint32_t slot = kEmptyBucket;
    for (std::uint32_t i = 0; i < p.trCap; ++i) {
      if (!p.tracks[i].used) {
        slot = i;
        break;
      }
    }
    if (slot == kEmptyBucket) {
      // Evict the oldest last report (then the oldest start), never one reported in this call.
      for (std::uint32_t i = 0; i < p.trCap; ++i) {
        const Track& x = p.tracks[i];
        if (x.reportStamp == p.stamp) continue;
        if (slot == kEmptyBucket) {
          slot = i;
          continue;
        }
        const Track& y = p.tracks[slot];
        if (x.last < y.last || (x.last == y.last && x.seq < y.seq)) slot = i;
      }
      if (slot == kEmptyBucket) {
        ++p.stats.tracking_dropped;
        continue;
      }
      ++p.stats.tracking_evictions;
      StopTrack(p, p.tracks[slot], SND_PHYSICS_STOP_EVICTED);
    }
    const snd_emitter_handle e = snd_create_emitter(p.sys);
    if (e.value == SND_HANDLE_INVALID_VALUE) {
      ++p.stats.api_failures;
      ++p.stats.tracking_dropped;
      continue;
    }
    Track& nt = p.tracks[slot];
    std::memset(&nt, 0, sizeof(nt));
    nt.used = true;
    nt.key = g.key;
    nt.kind = g.kind;
    nt.start = p.now;
    nt.last = p.now;
    nt.seq = ++p.seq;
    nt.reportStamp = p.stamp;
    nt.emitter = e;
    nt.stage = res.stage;
    nt.unknown = res.unknown;
    nt.matA = g.matA;
    nt.matB = g.matB;
    nt.gameA = g.gameA;
    nt.gameB = g.gameB;
    nt.objA = c.object_a;
    nt.objB = c.object_b;
    nt.massA = c.mass_a;
    nt.massB = c.mass_b;
    nt.speed = g.value;
    nt.position = snd_vec3{c.position[0], c.position[1], c.position[2]};
    if (snd_set_transform(p.sys, e, &nt.position, nullptr, nullptr) != SND_OK) {
      ++p.stats.api_failures;
    } else {
      nt.sentPosition = nt.position;
    }
    CountStage(p, res.stage, res.unknown);
    if (res.stage != SND_PHYSICS_STAGE_PAIR) {
      Request fb = TrackRequest(p, nt, 0);
      fb.event = res.event[0];
      NotifyFallback(p, fb);
    }
    nt.voiceCount = res.n;
    for (std::uint32_t vi = 0; vi < res.n; ++vi) {
      nt.v[vi].event = res.event[vi];
      nt.v[vi].who = res.who[vi];
      StartVoice(p, nt, vi, false);
    }
    ++p.stats.tracking_started;
  }
}

snd_result Submit(snd_phys& p, const snd_contact* contacts, std::uint32_t count, float dt) noexcept {
  if (!std::isfinite(dt) || dt < 0.0f || (count > 0u && contacts == nullptr)) {
    ++p.stats.calls_rejected;
    return SND_ERR_INVALID_ARGUMENT;
  }
  ++p.stats.calls_submit;
  const float dtc = std::min(dt, p.cfg.max_dt_seconds);
  const std::uint64_t us = Micros(dtc);
  p.now += us;
  // Budget of this frame: the whole part of carry + budget * dt; only the fraction is carried.
  const double avail = p.carry + static_cast<double>(p.cfg.budget_per_second) * static_cast<double>(us) / 1e6;
  const double whole = std::floor(avail);
  p.carry = avail - whole;
  const std::uint64_t frameBudget = whole > 1e18 ? std::uint64_t{1} << 60 : static_cast<std::uint64_t>(whole);
  // New stamp for the call's hash and the "reported in this call" marks.
  if (++p.stamp == 0u) {
    std::memset(p.gStamp, 0, (std::size_t{p.gMask} + 1u) * sizeof(std::uint32_t));
    for (std::uint32_t i = 0; i < p.trCap; ++i) p.tracks[i].reportStamp = 0;
    for (std::uint32_t i = 0; i < p.cdCount; ++i) p.cd[i].stamp = 0;
    p.stamp = 1;
  }
  // Audio side stalled (D-125): block_index unchanged for stall_calls calls in a row.
  snd_stats ss{};
  const bool known = snd_get_stats(p.sys, &ss) == SND_OK;
  if (known) {
    p.sameBlockCalls = (p.haveBlock && ss.block_index == p.lastBlock) ? p.sameBlockCalls + 1u : 0u;
    p.lastBlock = ss.block_index;
    p.haveBlock = true;
  }
  p.stalled = known && p.cfg.stall_calls > 0u && p.sameBlockCalls >= p.cfg.stall_calls;
  if (p.stalled) ++p.stats.audio_stalled_calls;
  // The release check that records runs only when block_index moved since the last one.
  const bool probe = !known || !p.haveProbeBlock || ss.block_index != p.probeBlock;
  if (ReleaseHeld(p, probe) && known) {
    p.haveProbeBlock = true;
    p.probeBlock = ss.block_index;
  }
  p.freshCount = 0;
  const float threshold = p.cfg.impulse_threshold > 0.0f ? p.cfg.impulse_threshold : p.tableThreshold;
  const float grid = p.cfg.grid_size;
  std::uint32_t groupCount = 0;
  // Steps 1 and 2 (and the slide reports) in one pass.
  for (std::uint32_t i = 0; i < count; ++i) {
    const snd_contact& c = contacts[i];
    ++p.stats.contacts_received;
    const std::uint32_t kind = c.kind;
    const bool oneShot = kind == SND_CONTACT_IMPACT || kind == SND_CONTACT_BREAK;
    const bool continuous = kind == SND_CONTACT_SLIDE || kind == SND_CONTACT_ROLL;
    if ((!oneShot && !continuous) || !FiniteVec(c.position)) {
      ++p.stats.dropped_invalid;
      continue;
    }
    float value;
    if (oneShot) {
      value = c.impulse;
      if (!Finite(value) || value < 0.0f) {
        ++p.stats.dropped_invalid;
        continue;
      }
      if (kind == SND_CONTACT_IMPACT && value < threshold) {
        ++p.stats.dropped_threshold;
        continue;
      }
    } else {
      value = c.relative_speed;
      if (!Finite(value) || value < 0.0f) {
        ++p.stats.dropped_invalid;
        continue;
      }
      if (value < p.cfg.min_slide_speed) {
        ++p.stats.dropped_threshold;
        continue;
      }
    }
    const Entry* ea = nullptr;
    const Entry* eb = nullptr;
    const std::uint16_t ma = MapMaterial(p, c.material_a, &ea);
    const std::uint16_t mb = MapMaterial(p, c.material_b, &eb);
    if (continuous) {
      const Key k{std::min(c.object_a, c.object_b), std::max(c.object_a, c.object_b), 0, MatKind(ma, mb, kind, 2u)};
      // A tracked key: kept in its entry (the largest speed of the call, the first on ties).
      if (Track* t = FindTrack(p, k)) {
        if (t->reportStamp == p.stamp) {
          ++p.stats.merged;
          if (value > t->reportSpeed) {
            t->reportSpeed = value;
            t->reportIdx = i;
          }
        } else {
          ++p.stats.slide_reports;
          t->reportStamp = p.stamp;
          t->reportSpeed = value;
          t->reportIdx = i;
        }
        continue;
      }
      // A new key: its own area (the tracking capacity), apart from the Impact / Break groups.
      Group* f = nullptr;
      for (std::uint32_t j = 0; j < p.freshCount; ++j) {
        if (SameKey(p.fresh[j].key, k)) {
          f = &p.fresh[j];
          break;
        }
      }
      if (f != nullptr) {
        ++f->count;
        ++p.stats.merged;
        if (value > f->value) {
          f->rep = i;
          f->value = value;
          f->matA = ma;
          f->matB = mb;
          f->gameA = c.material_a;
          f->gameB = c.material_b;
        }
        continue;
      }
      if (p.freshCount >= p.trCap) {
        ++p.stats.dropped_work_overflow;
        continue;
      }
      Group& ng = p.fresh[p.freshCount++];
      ng = Group{};
      ng.key = k;
      ng.rep = i;
      ng.count = 1;
      ng.kind = kind;
      ng.value = value;
      ng.matA = ma;
      ng.matB = mb;
      ng.gameA = c.material_a;
      ng.gameB = c.material_b;
      ++p.stats.slide_reports;
      continue;
    }
    const Key k{static_cast<std::uint64_t>(Cell(c.position[0], grid)), static_cast<std::uint64_t>(Cell(c.position[1], grid)),
                static_cast<std::uint64_t>(Cell(c.position[2], grid)), MatKind(ma, mb, kind, 1u)};
    const std::uint32_t found = FindGroup(p, k);
    if ((found & 0x80000000u) == 0u) {
      Group& g = p.groups[found];
      ++g.count;
      ++p.stats.merged;
      if (value > g.value) {
        g.rep = i;
        g.value = value;
        g.matA = ma;
        g.matB = mb;
        g.gameA = c.material_a;
        g.gameB = c.material_b;
      }
      continue;
    }
    if (groupCount >= p.gCap) {
      ++p.stats.dropped_work_overflow;
      continue;
    }
    const std::uint32_t bucket = found & 0x7FFFFFFFu;
    p.gStamp[bucket] = p.stamp;
    p.gIdx[bucket] = groupCount;
    Group& g = p.groups[groupCount++];
    g = Group{};
    g.key = k;
    g.rep = i;
    g.count = 1;
    g.kind = kind;
    g.value = value;
    g.matA = ma;
    g.matB = mb;
    g.gameA = c.material_a;
    g.gameB = c.material_b;
    ++p.stats.groups;
  }
  PlayGroups(p, contacts, groupCount, frameBudget);
  UpdateTracked(p, contacts);  // (1)
  Timeouts(p);                 // (2)
  StartFresh(p, contacts);     // (3)
  return SND_OK;
}

// ---------------------------------------------------------------------------------------
// Footsteps (9.2)
// ---------------------------------------------------------------------------------------
snd_result Footstep(snd_phys& p, snd_emitter_handle foot, std::uint16_t footMaterial, const snd_surface_weight* surfaces,
                    std::uint32_t count, snd_motion motion) noexcept {
  if (motion >= kMotions || (count > 0u && surfaces == nullptr)) {
    ++p.stats.calls_rejected;
    return SND_ERR_INVALID_ARGUMENT;
  }
  ++p.stats.footsteps;
  std::uint32_t best[2] = {kEmptyBucket, kEmptyBucket};
  for (std::uint32_t i = 0; i < count; ++i) {
    const float w = surfaces[i].weight;
    if (!Finite(w) || !(w > 0.0f)) continue;
    if (best[0] == kEmptyBucket || w > surfaces[best[0]].weight) {
      best[1] = best[0];
      best[0] = i;
    } else if (best[1] == kEmptyBucket || w > surfaces[best[1]].weight) {
      best[1] = i;
    }
  }
  const std::uint32_t chosen = best[0] == kEmptyBucket ? 0u : (best[1] == kEmptyBucket ? 1u : 2u);
  p.stats.footstep_surfaces_ignored += count - chosen;
  if (chosen == 0u) return SND_OK;
  // In double: two finite floats never overflow there (3e38 + 3e38).
  const double sum = chosen == 1u ? static_cast<double>(surfaces[best[0]].weight)
                                  : static_cast<double>(surfaces[best[0]].weight) + surfaces[best[1]].weight;
  const Entry* fe = nullptr;
  const std::uint16_t footSound = MapMaterial(p, footMaterial, &fe);
  const Curve& wc = p.weightCurve.count > 0u ? p.weightCurve : p.builtinWeight;
  snd_result first = SND_OK;
  for (std::uint32_t s = 0; s < chosen; ++s) {
    const snd_surface_weight& sw = surfaces[best[s]];
    const float wn = chosen == 1u ? 1.0f : static_cast<float>(static_cast<double>(sw.weight) / sum);
    const Entry* se = nullptr;
    const std::uint16_t m = MapMaterial(p, sw.material, &se);
    Request q{};
    q.source = SND_PHYSICS_SOURCE_FOOTSTEP;
    q.kind = motion;
    q.count = 1;
    q.matA = m;
    q.matB = footSound;
    q.gameA = sw.material;
    q.gameB = footMaterial;
    q.volume = ClampVolume(Eval(wc, wn));
    q.pitch = 1.0f;
    q.speed = 0.0f;
    snd_id ev = SND_ID_NONE;
    if (m == kUnknown) {
      q.flags |= SND_PHYSICS_RECORD_FLAG_UNKNOWN_MATERIAL;
      ++p.stats.fallback_unknown_material;
      ev = p.footDefault;
      q.stage = ev != SND_ID_NONE ? SND_PHYSICS_STAGE_FOOT_DEFAULT : SND_PHYSICS_STAGE_NOTHING;
    } else if ((ev = p.footMotion[static_cast<std::size_t>(m) * kMotions + motion]) != SND_ID_NONE) {
      q.stage = SND_PHYSICS_STAGE_FOOT_MOTION;
    } else if ((ev = p.footAny[m]) != SND_ID_NONE) {
      q.stage = SND_PHYSICS_STAGE_FOOT_ANY;
    } else if ((ev = p.footDefault) != SND_ID_NONE) {
      q.stage = SND_PHYSICS_STAGE_FOOT_DEFAULT;
    } else {
      q.stage = SND_PHYSICS_STAGE_NOTHING;
    }
    switch (q.stage) {
      case SND_PHYSICS_STAGE_FOOT_MOTION: ++p.stats.footstep_by_motion; break;
      case SND_PHYSICS_STAGE_FOOT_ANY: ++p.stats.footstep_fallback_any_motion; break;
      case SND_PHYSICS_STAGE_FOOT_DEFAULT: ++p.stats.footstep_fallback_default; break;
      default: ++p.stats.footstep_fallback_none; break;
    }
    q.event = ev;
    if (q.stage != SND_PHYSICS_STAGE_FOOT_MOTION) NotifyFallback(p, q);
    if (ev == SND_ID_NONE) continue;
    AddParam(q, p.cfg.params.motion, static_cast<float>(motion));
    if (footSound != kUnknown) AddParam(q, p.cfg.params.foot_material, static_cast<float>(footSound));
    AddParam(q, p.cfg.params.surface_weight, wn);
    AddSurface(q, se);
    ++p.stats.requested_footstep;
    const snd_playback_handle h = snd_play(p.sys, ev, foot, nullptr);
    snd_result r = SND_OK;
    if (h.value == SND_HANDLE_INVALID_VALUE) {
      ++p.stats.api_failures;
      r = SND_ERR_LIMIT_REACHED;
    } else {
      r = ApplyPlayback(p, h, q);
    }
    if (r != SND_OK && first == SND_OK) first = r;
    snd_physics_record rec = BaseRecord(q);
    rec.kind = SND_PHYSICS_RECORD_PLAY;
    rec.playback = h;
    rec.emitter = foot;
    rec.result = r;
    Notify(p, rec);
  }
  return first;
}

// ---------------------------------------------------------------------------------------
// Table setters
// ---------------------------------------------------------------------------------------
snd_result SetTable(snd_phys& p, const snd_physics_table_desc* in) noexcept {
  if (in == nullptr || in->struct_size < 8u || (in->struct_size % 4u) != 0u) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  snd_physics_table_desc d;
  snd_physics_table_desc_init(&d);
  std::memcpy(&d, in, std::min<std::size_t>(in->struct_size, sizeof(snd_physics_table_desc)));
  const std::uint32_t n = d.material_count;
  if (!Finite(d.impulse_threshold) || d.impulse_threshold < 0.0f || !ValidCurve(d.weight_to_volume, YRange::Volume)) {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (d.default_curves != nullptr && !ValidCurves(*d.default_curves)) return SND_ERR_INVALID_ARGUMENT;
  if (d.curves != nullptr) {
    for (std::uint32_t m = 0; m < n; ++m) {
      if (!ValidCurves(d.curves[m])) return SND_ERR_INVALID_ARGUMENT;
    }
  }
  if (n > p.nMax) return SND_ERR_LIMIT_REACHED;
  ClearTable(p);
  p.n = n;
  if (d.single != nullptr) std::memcpy(p.single, d.single, std::size_t{n} * kKinds * sizeof(snd_id));
  for (std::uint32_t k = 0; k < kKinds; ++k) {
    const snd_id* src = d.pair_by_kind[k] != nullptr ? d.pair_by_kind[k] : d.pair;
    if (src != nullptr) std::memcpy(p.pair[k], src, std::size_t{n} * n * sizeof(snd_id));
  }
  if (d.footstep != nullptr) std::memcpy(p.footMotion, d.footstep, std::size_t{n} * kMotions * sizeof(snd_id));
  if (d.footstep_any != nullptr) std::memcpy(p.footAny, d.footstep_any, std::size_t{n} * sizeof(snd_id));
  p.footDefault = d.footstep_default;
  for (std::uint32_t k = 0; k < kKinds; ++k) p.defaults[k] = d.default_event[k];
  if (d.curves != nullptr) std::memcpy(p.curves, d.curves, std::size_t{n} * sizeof(Curves));
  if (d.default_curves != nullptr) p.defaultCurves = *d.default_curves;
  p.weightCurve = d.weight_to_volume;
  p.tableThreshold = d.impulse_threshold;
  return SND_OK;
}

inline bool KindOk(snd_contact_kind kind) noexcept { return kind < kKinds || kind == SND_PHYSICS_ALL_KINDS; }

snd_result SetMap(snd_phys& p, const Entry* entries, std::uint32_t count) noexcept {
  if (count > 0u && entries == nullptr) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!ValidEntry(entries[i])) return SND_ERR_INVALID_ARGUMENT;
  }
  if (count > p.mapCap) return SND_ERR_LIMIT_REACHED;
  if (count == 0u) {
    p.mapActive = false;
    p.mapCount = 0;
    return SND_OK;
  }
  std::memcpy(p.map, entries, std::size_t{count} * sizeof(Entry));
  p.mapCount = count;
  p.mapActive = true;
  return SND_OK;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------------------
extern "C" {

snd_result snd_physics_create(snd_system* sys, const snd_physics_config* cfg, const snd_allocator* allocator,
                              snd_phys** out) {
  if (out != nullptr) *out = nullptr;
  if (sys == nullptr || cfg == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  snd_physics_config c;
  if (!ReadConfig(cfg, &c)) return SND_ERR_INVALID_ARGUMENT;
  return Create(sys, c, allocator, out);
}

void snd_physics_destroy(snd_phys* phys) { Destroy(phys); }

snd_result snd_phys_create(snd_system* sys, const snd_phys_desc* desc, const snd_allocator* allocator, snd_phys** out) {
  if (out != nullptr) *out = nullptr;
  if (sys == nullptr || desc == nullptr || out == nullptr) return SND_ERR_INVALID_ARGUMENT;
  const bool hasBank = desc->bank.value != SND_HANDLE_INVALID_VALUE;
  if (hasBank != (desc->table != SND_ID_NONE)) return SND_ERR_INVALID_ARGUMENT;
  snd_physics_config cfg;
  snd_physics_config_init(&cfg);
  cfg.max_tracked_pairs = desc->max_tracked_pairs;
  cfg.max_cooldowns = desc->max_cooldowns;
  cfg.impulse_threshold = desc->impulse_threshold;
  cfg.grid_size = desc->grid_size;
  cfg.cooldown_seconds = desc->cooldown_seconds;
  cfg.budget_per_second = desc->budget_per_second;
  cfg.tracking_timeout_seconds = desc->tracking_timeout_seconds;
  snd_physics_config checked;
  if (!ReadConfig(&cfg, &checked)) return SND_ERR_INVALID_ARGUMENT;
  snd_phys* p = nullptr;
  const snd_result r = Create(sys, checked, allocator, &p);
  if (r != SND_OK) return r;
  if (hasBank) {
    snd_result lr = LoadBankTable(*p, desc->bank, desc->table);
    if (lr != SND_OK) {
      Destroy(p);
      if (lr == SND_ERR_INVALID_HANDLE) lr = SND_ERR_INVALID_ARGUMENT;
      return lr;
    }
  }
  *out = p;
  return SND_OK;
}

void snd_phys_destroy(snd_phys* phys) { Destroy(phys); }

snd_result snd_phys_submit_contacts(snd_phys* phys, const snd_contact* contacts, uint32_t count, float dt_seconds) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  return Submit(*phys, contacts, count, dt_seconds);
}

snd_result snd_phys_play_footstep(snd_phys* phys, snd_emitter_handle foot, uint16_t foot_material,
                                  const snd_surface_weight* surfaces, uint32_t surface_count, snd_motion motion) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  return Footstep(*phys, foot, foot_material, surfaces, surface_count, motion);
}

snd_result snd_physics_play_at(snd_phys* phys, snd_id event, const snd_vec3* position) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (event == SND_ID_NONE || position == nullptr || !Finite(position->x) || !Finite(position->y) ||
      !Finite(position->z)) {
    ++p.stats.calls_rejected;
    return SND_ERR_INVALID_ARGUMENT;
  }
  ++p.stats.requested_play_at;
  const snd_result r = snd_play_at(p.sys, event, position, nullptr);
  if (r != SND_OK) ++p.stats.api_failures;
  Request q{};
  q.source = SND_PHYSICS_SOURCE_PLAY_AT;
  q.stage = SND_PHYSICS_STAGE_DIRECT;
  q.flags = SND_PHYSICS_RECORD_FLAG_POSITION_ONLY;
  q.event = event;
  q.count = 1;
  q.matA = kUnknown;
  q.matB = kUnknown;
  q.gameA = kUnknown;
  q.gameB = kUnknown;
  q.volume = 1.0f;
  q.pitch = 1.0f;
  q.position = *position;
  snd_physics_record rec = BaseRecord(q);
  rec.kind = SND_PHYSICS_RECORD_PLAY;
  rec.result = r;
  Notify(p, rec);
  return r;
}

snd_result snd_physics_set_observer(snd_phys* phys, snd_physics_observer observer, void* user) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  phys->cfg.observer = observer;
  phys->cfg.observer_user = user;
  return SND_OK;
}

snd_result snd_physics_set_table(snd_phys* phys, const snd_physics_table_desc* desc) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  return SetTable(*phys, desc);
}

snd_result snd_physics_load_bank_table(snd_phys* phys, snd_bank_handle bank, snd_id table) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  return LoadBankTable(*phys, bank, table);
}

uint32_t snd_physics_material_count(const snd_phys* phys) {
  if (Enter(phys) != SND_OK) return 0u;
  return phys->n;
}

snd_result snd_physics_set_single_event(snd_phys* phys, uint16_t material, snd_contact_kind kind, snd_id event) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (material >= p.n || !KindOk(kind)) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t k = 0; k < kKinds; ++k) {
    if (kind == SND_PHYSICS_ALL_KINDS || kind == k) p.single[std::size_t{material} * kKinds + k] = event;
  }
  return SND_OK;
}

snd_result snd_physics_set_pair_event(snd_phys* phys, uint16_t material_a, uint16_t material_b, snd_contact_kind kind,
                                      snd_id event) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (material_a >= p.n || material_b >= p.n || !KindOk(kind)) return SND_ERR_INVALID_ARGUMENT;
  const std::size_t at = std::size_t{std::min(material_a, material_b)} * p.n + std::max(material_a, material_b);
  for (std::uint32_t k = 0; k < kKinds; ++k) {
    if (kind == SND_PHYSICS_ALL_KINDS || kind == k) p.pair[k][at] = event;
  }
  return SND_OK;
}

snd_result snd_physics_set_default_event(snd_phys* phys, snd_contact_kind kind, snd_id event) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  if (!KindOk(kind)) return SND_ERR_INVALID_ARGUMENT;
  for (std::uint32_t k = 0; k < kKinds; ++k) {
    if (kind == SND_PHYSICS_ALL_KINDS || kind == k) phys->defaults[k] = event;
  }
  return SND_OK;
}

snd_result snd_physics_set_footstep_event(snd_phys* phys, uint16_t material, snd_motion motion, snd_id event) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (material == SND_PHYSICS_DEFAULT_MATERIAL) {
    if (motion != SND_PHYSICS_ANY_MOTION) return SND_ERR_INVALID_ARGUMENT;
    p.footDefault = event;
    return SND_OK;
  }
  if (material >= p.n) return SND_ERR_INVALID_ARGUMENT;
  if (motion == SND_PHYSICS_ANY_MOTION) {
    p.footAny[material] = event;
  } else if (motion < kMotions) {
    p.footMotion[std::size_t{material} * kMotions + motion] = event;
  } else {
    return SND_ERR_INVALID_ARGUMENT;
  }
  return SND_OK;
}

snd_result snd_physics_set_curves(snd_phys* phys, uint16_t material, const snd_physics_curves* curves) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (curves != nullptr && !ValidCurves(*curves)) return SND_ERR_INVALID_ARGUMENT;
  Curves* dst = nullptr;
  if (material == SND_PHYSICS_DEFAULT_MATERIAL) {
    dst = &p.defaultCurves;
  } else if (material < p.n) {
    dst = &p.curves[material];
  } else {
    return SND_ERR_INVALID_ARGUMENT;
  }
  if (curves != nullptr) {
    *dst = *curves;
  } else {
    std::memset(dst, 0, sizeof(*dst));
  }
  return SND_OK;
}

snd_result snd_physics_set_material_map(snd_phys* phys, const snd_physics_material_entry* entries, uint32_t count) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  return SetMap(*phys, entries, count);
}

snd_result snd_physics_set_material_entry(snd_phys* phys, uint16_t game_material, const snd_physics_material_entry* entry) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (entry == nullptr || !ValidEntry(*entry)) return SND_ERR_INVALID_ARGUMENT;
  if (game_material >= p.mapCap) return SND_ERR_LIMIT_REACHED;
  if (!p.mapActive) {
    p.mapActive = true;
    p.mapCount = 0;
  }
  for (std::uint32_t g = p.mapCount; g < game_material; ++g) p.map[g] = IdentityEntry(g);
  if (game_material >= p.mapCount) p.mapCount = std::uint32_t{game_material} + 1u;
  p.map[game_material] = *entry;
  return SND_OK;
}

snd_result snd_physics_set_surface_param(snd_phys* phys, uint16_t game_material, snd_id param, float value) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  snd_phys& p = *phys;
  if (param == SND_ID_NONE || ReservedId(param) || !Finite(value)) return SND_ERR_INVALID_ARGUMENT;
  if (!p.mapActive || game_material >= p.mapCount) return SND_ERR_NOT_FOUND;
  Entry& en = p.map[game_material];
  for (std::uint32_t i = 0; i < en.param_count; ++i) {
    if (en.params[i].id == param) {
      en.params[i].value = value;
      return SND_OK;
    }
  }
  if (en.param_count >= kMaxSurface) return SND_ERR_LIMIT_REACHED;
  en.params[en.param_count++] = snd_physics_param{param, value};
  return SND_OK;
}

snd_result snd_physics_get_stats(const snd_phys* phys, snd_physics_stats* out) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  if (out == nullptr || out->struct_size < 8u) return SND_ERR_INVALID_ARGUMENT;
  const std::uint32_t want = out->struct_size;
  const std::size_t n = std::min<std::size_t>(want, sizeof(snd_physics_stats));
  std::memcpy(out, &phys->stats, n);
  out->struct_size = want;
  return SND_OK;
}

snd_result snd_physics_reset_stats(snd_phys* phys) {
  const snd_result e = Enter(phys);
  if (e != SND_OK) return e;
  std::memset(&phys->stats, 0, sizeof(phys->stats));
  phys->stats.struct_size = static_cast<std::uint32_t>(sizeof(snd_physics_stats));
  return SND_OK;
}

}  // extern "C"
