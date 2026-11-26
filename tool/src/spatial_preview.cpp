// 3D attenuation preview (spatial_preview.hpp).
#include "snd/tool/spatial_preview.hpp"

#include <algorithm>
#include <cmath>

#include "snd/dsp/kernels.hpp"
#include "snd/dsp/pan.hpp"

namespace snd::tool {
namespace {

using json::Value;
constexpr float kPi = 3.14159265358979f;

Vec3f Add(Vec3f a, Vec3f b) { return Vec3f{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3f Sub(Vec3f a, Vec3f b) { return Vec3f{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3f Mul(Vec3f a, float s) { return Vec3f{a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3f Cross(Vec3f a, Vec3f b) { return Vec3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3f Normalize(Vec3f a, Vec3f fallback) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-9f ? Mul(a, 1.0f / l) : fallback;
}

std::vector<CurvePoint> CurveOf(const Value& c) {
  std::vector<CurvePoint> out;
  const Value* pts = c.Find("points");
  if (pts == nullptr || !pts->IsArray()) return out;
  for (const Value& p : pts->items) out.push_back(CurvePoint{NumberAt(p, "[0]", 0), NumberAt(p, "[1]", 0), NumberAt(p, "[2]", 0)});
  return out;
}

}  // namespace

SpatialPreview::SpatialPreview(const ProjectDocument& doc, std::string eventGuid)
    : doc_(doc), guid_(NormalizeGuid(eventGuid)) {}

bool SpatialPreview::valid() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr && o->kind() == "event";
}

void SpatialPreview::SetListenerYaw(float r) { listener_.forward = Vec3f{std::sin(r), 0.0f, std::cos(r)}; }
void SpatialPreview::SetEmitterYaw(float r) { emitter_.forward = Vec3f{std::sin(r), 0.0f, std::cos(r)}; }

namespace {
bool Coordinate(double v) { return std::isfinite(v) && std::fabs(v) <= SpatialPreview::kMaxCoordinate; }
bool Angle(double v) { return std::isfinite(v) && std::fabs(v) <= SpatialPreview::kMaxAngle; }
}  // namespace

bool SpatialPreview::PlaceListener(double x, double z) {
  if (!Coordinate(x) || !Coordinate(z)) return false;
  MoveListener(static_cast<float>(x), static_cast<float>(z));
  return true;
}
bool SpatialPreview::PlaceEmitter(double x, double z) {
  if (!Coordinate(x) || !Coordinate(z)) return false;
  MoveEmitter(static_cast<float>(x), static_cast<float>(z));
  return true;
}
bool SpatialPreview::TurnListener(double radians) {
  if (!Angle(radians)) return false;
  SetListenerYaw(static_cast<float>(radians));
  return true;
}
bool SpatialPreview::TurnEmitter(double radians) {
  if (!Angle(radians)) return false;
  SetEmitterYaw(static_cast<float>(radians));
  return true;
}

void SpatialPreview::SetView(float cx, float cz, float mpp) {
  centerX_ = cx;
  centerZ_ = cz;
  if (std::isfinite(mpp) && mpp > 1e-6f) metersPerPixel_ = mpp;
}
void SpatialPreview::Zoom(float factor, float ax, float az) {
  if (!(factor > 0.0f) || !std::isfinite(factor)) return;
  const float mpp = std::clamp(metersPerPixel_ / factor, 1e-4f, 1e4f);
  const float k = mpp / metersPerPixel_;
  centerX_ = ax + (centerX_ - ax) * k;
  centerZ_ = az + (centerZ_ - az) * k;
  metersPerPixel_ = mpp;
}
void SpatialPreview::WorldToScreen(float x, float z, float w, float h, float* px, float* py) const {
  *px = w * 0.5f + (x - centerX_) / metersPerPixel_;
  *py = h * 0.5f - (z - centerZ_) / metersPerPixel_;
}
void SpatialPreview::ScreenToWorld(float px, float py, float w, float h, float* x, float* z) const {
  *x = centerX_ + (px - w * 0.5f) * metersPerPixel_;
  *z = centerZ_ - (py - h * 0.5f) * metersPerPixel_;
}
void SpatialPreview::FitView(float w, float h) {
  const Vec3f l = listener_.position, e = emitter_.position;
  float r = std::max(std::fabs(e.x - l.x), std::fabs(e.z - l.z));
  r = std::max(r, MaxDistance());
  if (r <= 0.0f) r = 10.0f;
  centerX_ = l.x;
  centerZ_ = l.z;
  const float side = std::max(1.0f, std::min(w, h));
  metersPerPixel_ = 2.2f * r / side;
}

float SpatialPreview::MaxDistance() const {
  const DocObject* o = doc_.Find(guid_);
  return o != nullptr ? static_cast<float>(NumberAt(o->doc, "maxDistance", 0)) : 0.0f;
}
void SpatialPreview::Cone(float* inner, float* outer, float* outerDb) const {
  const DocObject* o = doc_.Find(guid_);
  *inner = o != nullptr ? static_cast<float>(NumberAt(o->doc, "cone.innerDeg", 0)) : 0.0f;
  *outer = o != nullptr ? static_cast<float>(NumberAt(o->doc, "cone.outerDeg", 0)) : 0.0f;
  *outerDb = o != nullptr ? static_cast<float>(NumberAt(o->doc, "cone.outerDb", 0)) : 0.0f;
}
std::vector<CurvePoint> SpatialPreview::DistanceCurve(CurveTarget t) const {
  const DocObject* o = doc_.Find(guid_);
  const Value* c = o != nullptr ? FindPath(o->doc, std::string("distanceCurves.") + CurveTargetName(t)) : nullptr;
  return c != nullptr && c->IsObject() ? CurveOf(*c) : std::vector<CurvePoint>{};
}

SpatialResult SpatialPreview::Compute(std::uint32_t outCh, std::uint32_t srcCh, float extrapolateSeconds) const {
  SpatialResult r;
  r.outputChannels = outCh;
  r.sourceChannels = srcCh;
  const DocObject* o = doc_.Find(guid_);
  if (o == nullptr) return r;
  r.is3D = BoolAt(o->doc, "is3D", false);
  // Compute3D.
  const Vec3f pos = Add(emitter_.position, Mul(emitter_.velocity, std::max(0.0f, extrapolateSeconds)));
  const Vec3f efwd = Normalize(emitter_.forward, Vec3f{0, 0, 1});
  const Vec3f rel = Sub(pos, listener_.position);
  const float dist = std::sqrt(Dot(rel, rel));
  const Vec3f fwd = Normalize(listener_.forward, Vec3f{0, 0, 1});
  const Vec3f up = Normalize(listener_.up, Vec3f{0, 1, 0});
  const Vec3f right = Normalize(Cross(up, fwd), Vec3f{1, 0, 0});
  const Vec3f dir = dist > 1e-6f ? Mul(rel, 1.0f / dist) : Vec3f{0, 0, 1};
  r.distance = dist;
  r.direction = Vec3f{Dot(dir, right), Dot(dir, up), Dot(dir, fwd)};
  r.coneCos = dist > 1e-6f ? -Dot(efwd, dir) : 1.0f;
  r.azimuthRad = std::atan2(r.direction.x, r.direction.z);
  // Parameter curves (distance-input curves read the distance).
  const ComposeResult cr = ComposeEvent(doc_, guid_, params_, r.is3D ? dist : 0.0);
  float gainDb = static_cast<float>(cr.volumeDb);
  float linear = 1.0f;
  float lowpass = cr.lowpassHz > 0.0 ? static_cast<float>(cr.lowpassHz) : 1.0e9f;
  r.curveDb = gainDb;
  r.pitchCents = static_cast<float>(cr.pitchCents);
  r.highpassHz = static_cast<float>(cr.highpassHz);
  r.pan = std::clamp(static_cast<float>(cr.pan), -1.0f, 1.0f);
  // Basic 3D (EvaluateDspParams).
  if (r.is3D) {
    const std::vector<CurvePoint> vol = DistanceCurve(CurveTarget::Volume);
    if (!vol.empty()) {
      r.volumeCurve = true;
      r.distanceDb = EvaluateCurve(vol, dist);
      gainDb += r.distanceDb;
    } else {
      const float maxD = MaxDistance();
      r.beyondMaxDistance = maxD > 0.0f && dist > maxD;
      r.distanceGain = r.beyondMaxDistance ? 0.0f : 1.0f / (dist > 1.0f ? dist : 1.0f);
      linear *= r.distanceGain;
    }
    const std::vector<CurvePoint> lp = DistanceCurve(CurveTarget::Lowpass);
    if (!lp.empty()) {
      const float y = EvaluateCurve(lp, dist);
      lowpass = y < lowpass ? y : lowpass;
    }
    float inner = 0, outer = 0, outerDb = 0;
    Cone(&inner, &outer, &outerDb);
    float cc2 = std::clamp(r.coneCos, -1.0f, 1.0f);
    r.coneAngleDeg = 2.0f * std::acos(cc2) * 180.0f / kPi;
    if (outer > 0.0f && outer >= inner && r.coneAngleDeg > inner) {
      const float tt = r.coneAngleDeg >= outer ? 1.0f : (r.coneAngleDeg - inner) / (outer - inner > 1e-6f ? outer - inner : 1.0f);
      r.coneDb = outerDb * tt;
      gainDb += r.coneDb;
    }
    const std::vector<CurvePoint> sp = DistanceCurve(CurveTarget::Spread);
    const std::vector<CurvePoint> bl = DistanceCurve(CurveTarget::Blend3D);
    const std::vector<CurvePoint> rv = DistanceCurve(CurveTarget::ReverbSend);
    if (!sp.empty()) r.spread = std::clamp(EvaluateCurve(sp, dist), 0.0f, 1.0f);
    if (!bl.empty()) r.blend3D = std::clamp(EvaluateCurve(bl, dist), 0.0f, 1.0f);
    if (!rv.empty()) r.reverbSend = EvaluateCurve(rv, dist);
  }
  r.gainDb = gainDb;
  r.distanceGain = r.is3D ? (r.volumeCurve ? 1.0f : r.distanceGain) : 1.0f;
  if (r.is3D && r.blend3D < 1.0f) {
    // The unspatialized portion keeps its level. Event volume applies to both portions.
    const float spatialGain = dsp::DbToGain(r.distanceDb + r.coneDb) * linear;
    r.linearGain = dsp::DbToGain(r.curveDb) * ((1.0f - r.blend3D) + r.blend3D * spatialGain);
  } else {
    r.linearGain = dsp::DbToGain(gainDb) * linear;
  }
  r.lowpassHz = lowpass < 1.0e9f ? lowpass : 0.0f;
  // PanFor (sys_render.cpp).
  PanMatrix m{};
  dsp::SpeakerLayout outLayout = dsp::SpeakerLayout::Stereo, inLayout = dsp::SpeakerLayout::Stereo;
  if (dsp::LayoutForChannels(outCh, &outLayout) && dsp::LayoutForChannels(srcCh, &inLayout)) {
    PanMatrix p2{};
    dsp::ComputeStereoPanMatrix(srcCh, outLayout, r.pan, dsp::PanLaw::ConstantPower, &p2);
    if (!r.is3D || r.blend3D <= 0.0f) {
      m = p2;
    } else {
      PanMatrix p3{};
      dsp::ComputePanMatrix(srcCh, outLayout, r.azimuthRad, r.spread, &p3);
      if (r.blend3D >= 1.0f) {
        m = p3;
      } else {
        dsp::LerpPanMatrix(p2, p3, r.blend3D, &m);
      }
    }
  }
  for (std::uint32_t i = 0; i < 8; ++i) {
    for (std::uint32_t j = 0; j < 8; ++j) r.pan3[i][j] = (i < kMaxChannels && j < kMaxChannels) ? m.gain[i][j] : 0.0f;
  }
  for (std::uint32_t j = 0; j < 8; ++j) r.outputGain[j] = r.linearGain * r.pan3[0][j];
  return r;
}

AuditionRequest SpatialPreview::Audition() const {
  AuditionRequest a;
  a.eventGuid = guid_;
  const DocObject* o = doc_.Find(guid_);
  a.eventId = o != nullptr ? HashId(o->name()) : 0u;
  a.position[0] = emitter_.position.x;
  a.position[1] = emitter_.position.y;
  a.position[2] = emitter_.position.z;
  a.listener = listener_;
  return a;
}

}  // namespace snd::tool
