// System 5: Compute3D (4.5, basic form). Per Emitter x Listener pair:
// distance, direction in the listener's frame (x right, y up, z forward),
// cone cosine (emitter forward vs the direction to the listener), validity.
// The emitter position is extrapolated with its velocity since its last
// update, up to Config.tuning.emitterExtrapolateMs (2.1). Doppler is stage 3
// (dopplerRatio = 1).
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
}  // namespace

void Compute3D(Impl& im) noexcept {
  schema::Tables& t = im.tables;
  const std::uint32_t L = im.cfg.maxListeners;
  const schema::TableRows& er = t.Emitter.rows;
  const float rate = static_cast<float>(im.cfg.sampleRate);
  for (std::uint32_t d = 0; d < er.count; ++d) {
    const SlotIndex e = base::rows::SlotAt(er, d);
    const BlockIndex last = SND_R(Emitter_lastUpdateBlock)[d];
    std::uint64_t frames = (im.block > last ? im.block - last : 0u) * im.cfg.blockFrames;
    if (frames > im.cfg.extrapolateFrames) frames = im.cfg.extrapolateFrames;
    const Vec3 pos = Add(SND_R(Emitter_position)[d], Mul(SND_R(Emitter_velocity)[d], static_cast<float>(frames) / rate));
    SND_W(Emitter_renderPosition)[d] = pos;
    const Vec3 efwd = Normalize(SND_R(Emitter_forward)[d], Vec3{0, 0, 1});
    schema::TableRows& sr = SND_ROWS(Spatial);
    for (std::uint32_t l = 0; l < L; ++l) {
      const SlotIndex s = e * L + l;
      if (!base::rows::IsLive(sr, s)) base::rows::InsertNext(sr, s);
      const bool live = base::rows::IsLive(t.Listener.rows, l) && SND_R(Listener_enabled)[l] != 0u;
      if (!live) {
        SND_W(Spatial_valid)[s] = 0u;
        continue;
      }
      const Vec3 rel = Sub(pos, SND_R(Listener_position)[l]);
      const float dist = std::sqrt(Dot(rel, rel));
      const Vec3 fwd = Normalize(SND_R(Listener_forward)[l], Vec3{0, 0, 1});
      const Vec3 up = Normalize(SND_R(Listener_up)[l], Vec3{0, 1, 0});
      const Vec3 right = Normalize(Cross(up, fwd), Vec3{1, 0, 0});
      const Vec3 dir = dist > 1e-6f ? Mul(rel, 1.0f / dist) : Vec3{0, 0, 1};
      SND_W(Spatial_distance)[s] = dist;
      SND_W(Spatial_direction)[s] = Vec3{Dot(dir, right), Dot(dir, up), Dot(dir, fwd)};
      SND_W(Spatial_dopplerRatio)[s] = 1.0f;
      SND_W(Spatial_coneCos)[s] = dist > 1e-6f ? -Dot(efwd, dir) : 1.0f;
      SND_W(Spatial_valid)[s] = 1u;
    }
  }
}

}  // namespace snd::core
