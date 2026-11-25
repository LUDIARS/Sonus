// System 5: Compute3D (4.5, basic form). Per Emitter x Listener pair:
// distance, direction in the listener's frame (x right, y up, z forward),
// cone cosine (emitter forward vs the direction to the listener), validity.
// The emitter position is extrapolated with its velocity since its last
// update, up to Config.tuning.emitterExtrapolateMs (2.1).
#include <algorithm>
#include <cmath>

#include "access.hpp"
#include "core_impl.hpp"

namespace snd::core {

namespace {
constexpr schema::SystemId kSys = schema::SystemId::Compute3D;

Vec3 Add(Vec3 a, Vec3 b) noexcept { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) noexcept { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 a, float s) noexcept { return Vec3{a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) noexcept { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Normalize(Vec3 a, Vec3 fallback) noexcept {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-9f ? Mul(a, 1.0f / l) : fallback;
}
// Keep the strongest zones, with stable emitter-slot order on equal weights.
void InsertZone(ZoneSend (&zones)[kMaxZones], ZoneSend zone) noexcept {
  for (ZoneSend& current : zones) {
    if (zone.amount > current.amount || (zone.amount == current.amount && zone.emitter < current.emitter)) {
      std::swap(current, zone);
      if (zone.amount == 0) break;
    }
  }
}

float Doppler(const snd_config& cfg, Vec3 listenerVelocity, Vec3 emitterVelocity, Vec3 n) noexcept {
  // n points from listener to source: positive listener velocity approaches,
  // positive emitter velocity recedes. Limit radial speeds near the singularity.
  const float c = cfg.speed_of_sound;
  const float factor = cfg.doppler_factor;
  if (factor == 0) return 1;
  const auto radial = [n](Vec3 v) {
    return static_cast<double>(n.x) * v.x + static_cast<double>(n.y) * v.y + static_cast<double>(n.z) * v.z;
  };
  const double numerator = std::max(0.01 * c, c + factor * radial(listenerVelocity));
  const double denominator = std::max(0.01 * c, c + factor * radial(emitterVelocity));
  return static_cast<float>(std::clamp(numerator / denominator, 0.25, 4.0));
}
struct EmitterSpatialFrame {
  Vec3 position, velocity, forward, extent;
  std::uint32_t shape;
  Vec3 right, up;

  EmitterSpatialFrame(Vec3 pos, Vec3 vel, Vec3 fwd, Vec3 halfExtents, std::uint32_t kind) noexcept
      : position(pos), velocity(vel), forward(Normalize(fwd, Vec3{0, 0, 1})), extent(halfExtents), shape(kind),
        right(Normalize(Cross(std::fabs(forward.y) < 0.9999f ? Vec3{0, 1, 0} : Vec3{0, 0, 1}, forward),
                        Vec3{1, 0, 0})), up(Cross(forward, right)) {}
};

SpatialSample Sample(const Impl& im, const EmitterSpatialFrame& e, std::uint32_t l,
                     Vec3& local, float& coneCos) noexcept {
  const schema::Tables& t = im.tables;
  Vec3 rel = Sub(e.position, SND_R(Listener_position)[l]);
  float radius = 0;
  if (e.shape != SND_EMITTER_SHAPE_POINT) {
    const Vec3 towardListener = Mul(rel, -1);
    rel = Add(rel, Mul(e.right, std::clamp(Dot(towardListener, e.right), -e.extent.x, e.extent.x)));
    radius = e.extent.x;
    if (e.shape >= SND_EMITTER_SHAPE_RECTANGLE) {
      rel = Add(rel, Mul(e.forward, std::clamp(Dot(towardListener, e.forward), -e.extent.z, e.extent.z)));
      radius = std::max(radius, e.extent.z);
    }
    if (e.shape == SND_EMITTER_SHAPE_BOX) {
      rel = Add(rel, Mul(e.up, std::clamp(Dot(towardListener, e.up), -e.extent.y, e.extent.y)));
      radius = std::max(radius, e.extent.y);
    }
  }
  const float dist = std::sqrt(Dot(rel, rel));
  const Vec3 fwd = Normalize(SND_R(Listener_forward)[l], Vec3{0, 0, 1});
  const Vec3 up = Normalize(SND_R(Listener_up)[l], Vec3{0, 1, 0});
  const Vec3 right = Normalize(Cross(up, fwd), Vec3{1, 0, 0});
  const Vec3 dir = dist > 1e-6f ? Mul(rel, 1.0f / dist) : Vec3{0, 0, 1};
  local = Vec3{Dot(dir, right), Dot(dir, up), Dot(dir, fwd)};
  coneCos = dist > 1e-6f ? -Dot(e.forward, dir) : 1.0f;
  constexpr float kPi = 3.14159265358979f;
  return {dist, radius > 0 ? radius / (radius + dist) : 0,
          dist > 1e-6f ? Doppler(im.config, SND_R(Listener_velocity)[l], e.velocity, dir) : 1.0f,
          2.0f * std::acos(std::clamp(coneCos, -1.0f, 1.0f)) * 180.0f / kPi, std::atan2(local.x, local.z)};
}
}  // namespace

bool SampleFrozenEmitter(const Impl& im, const PlaybackEmitter& emitter, std::uint32_t l,
                         SpatialSample& sample) noexcept {
  const schema::Tables& t = im.tables;
  if (!base::rows::IsLive(t.Listener.rows, l) || SND_R(Listener_enabled)[l] == 0u) return false;
  Vec3 direction{};
  float coneCos = 0;
  sample = Sample(im, EmitterSpatialFrame{emitter.position, Vec3{}, emitter.forward, emitter.halfExtents, emitter.shape},
                  l, direction, coneCos);
  return true;
}

void Compute3D(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  const std::uint32_t L = im.cfg.maxListeners;
  const schema::TableRows& er = t.Emitter.rows;
  const float rate = static_cast<float>(im.cfg.sampleRate);
  for (auto& listener : im.dp.zones) for (auto& zone : listener) zone = ZoneSend{};
  for (std::uint32_t d = 0; d < er.count; ++d) {
    const SlotIndex e = base::rows::SlotAt(er, d);
    const BlockIndex last = SND_R(Emitter_lastUpdateBlock)[d];
    std::uint64_t frames = (im.block > last ? im.block - last : 0u) * im.cfg.blockFrames;
    if (frames > im.cfg.extrapolateFrames) frames = im.cfg.extrapolateFrames;
    const Vec3 pos = Add(SND_R(Emitter_position)[d], Mul(SND_R(Emitter_velocity)[d], static_cast<float>(frames) / rate));
    SND_W(Emitter_renderPosition)[d] = pos;
    const EmitterSpatialFrame emitter{pos, SND_R(Emitter_velocity)[d], SND_R(Emitter_forward)[d],
                                      SND_R(Emitter_halfExtents)[d], SND_R(Emitter_shapeKind)[d]};
    schema::TableRows& sr = SND_ROWS(Spatial);
    for (std::uint32_t l = 0; l < L; ++l) {
      const SlotIndex s = e * L + l;
      if (!base::rows::IsLive(sr, s)) base::rows::InsertNext(sr, s);
      const bool live = base::rows::IsLive(t.Listener.rows, l) && SND_R(Listener_enabled)[l] != 0u;
      if (!live) {
        SND_W(Spatial_valid)[s] = 0u;
        continue;
      }
      Vec3 direction{};
      float coneCos = 0;
      const auto sample = Sample(im, emitter, l, direction, coneCos);
      const float dist = sample.distance;
      SND_W(Spatial_distance)[s] = dist;
      SND_W(Spatial_direction)[s] = direction;
      SND_W(Spatial_azimuth)[s] = sample.azimuth;
      SND_W(Spatial_shapeSpread)[s] = sample.spread;
      SND_W(Spatial_dopplerRatio)[s] = sample.ratio;
      const Id zoneBus = SND_R(Emitter_zoneBus)[d];
      if (zoneBus != SND_ID_NONE) {
        const float fade = SND_R(Emitter_zoneFade)[d];
        const float amount = SND_R(Emitter_zoneAmount)[d] *
                             (dist < 1e-6f ? 1 : fade > 0 ? std::max(0.0f, 1 - dist / fade) : 0);
        if (amount > 0) InsertZone(im.dp.zones[l], ZoneSend{zoneBus, e, amount});
      }
      SND_W(Spatial_coneCos)[s] = coneCos;
      SND_W(Spatial_coneAngle)[s] = sample.coneAngle;
      SND_W(Spatial_valid)[s] = 1u;
    }
  }
}

}  // namespace snd::core
