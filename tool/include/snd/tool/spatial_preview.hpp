// 3D attenuation preview model (design.md 4.5, 11.1 3D減衰プレビュー).
//
// The state of the top-down view: a listener (position, facing) and an emitter (position,
// facing, velocity), the visible area (centre and metres per pixel), and the selected
// event's maxDistance circle and cone. Compute() gives what the runtime computes for that
// arrangement, with the same operations in float as core (sys_3d.cpp Compute3D,
// sys_dsp.cpp EvaluateDspParams, sys_render.cpp PanFor):
//   - distance and the direction in the listener's frame (x right, y up, z forward; the
//     emitter extrapolated with its velocity for `extrapolateSeconds`), cone cosine;
//   - volume: the event's distance volume curve (dB) or, without one, inverse distance
//     1 / max(d, 1) cut to 0 beyond maxDistance (if set);
//   - lowpass: the distance lowpass curve and lowpass parameter curves (minimum);
//   - the event's parameter curves (ComposeEvent, curve_editor.hpp) with the preview's
//     parameter values (a curve with input "distance" reads the distance);
//   - cone: full cone angle 2 acos(cos) in degrees; beyond innerDeg the gain falls linearly
//     to outerDb at outerDeg;
//   - spread / 3D blend: their distance curves (clamped 0..1; no curve: spread 0, blend 1);
//   - pan: the 2D balance (pan curves) blended with the 3D pan at the azimuth
//     atan2(x, z) (dsp::ComputePanMatrix with the spread) by the blend.
// Clip values (volumeDb, random ranges) are not part of the preview (they differ per clip).
// Listening is group P's: Audition() is the request (event, emitter position, listener) a
// live link Audition message is made from.
#ifndef SND_TOOL_SPATIAL_PREVIEW_HPP
#define SND_TOOL_SPATIAL_PREVIEW_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "snd/id.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/document.hpp"

namespace snd::tool {

struct Vec3f {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct ListenerPose {
  Vec3f position{0.0f, 0.0f, 0.0f};
  Vec3f forward{0.0f, 0.0f, 1.0f};
  Vec3f up{0.0f, 1.0f, 0.0f};
};

struct EmitterPose {
  Vec3f position{0.0f, 0.0f, 5.0f};
  Vec3f forward{0.0f, 0.0f, 1.0f};
  Vec3f velocity{0.0f, 0.0f, 0.0f};
};

struct SpatialResult {
  bool is3D = false;            // the event's is3D flag (a 2D event is not attenuated)
  float distance = 0.0f;
  Vec3f direction;              // in the listener's frame
  float azimuthRad = 0.0f;      // atan2(direction.x, direction.z)
  float coneCos = 1.0f;
  float coneAngleDeg = 0.0f;    // full angle between the emitter's forward and the listener
  float coneDb = 0.0f;
  bool volumeCurve = false;     // distance volume curve used (else inverse distance)
  float distanceDb = 0.0f;      // the volume curve's value
  float distanceGain = 1.0f;    // inverse distance factor (1 with a curve)
  float curveDb = 0.0f;         // parameter curves (volume target)
  float gainDb = 0.0f;          // distanceDb + curveDb + coneDb
  float linearGain = 1.0f;      // DbToGain(gainDb) * distanceGain
  float pitchCents = 0.0f;
  float lowpassHz = 0.0f;       // 0 = open
  float highpassHz = 0.0f;
  float pan = 0.0f;             // 2D balance (-1 .. 1)
  float spread = 0.0f;
  float blend3D = 1.0f;
  float reverbSend = 0.0f;      // its distance curve (the runtime applies it from stage 3)
  bool beyondMaxDistance = false;
  // Gains of the pan matrix for each source channel to each output channel (no gain applied).
  std::uint32_t sourceChannels = 1;
  std::uint32_t outputChannels = 2;
  float pan3[8][8] = {};
  // Output gain per output channel for a mono source: linearGain * pan row 0.
  float outputGain[8] = {};
};

struct AuditionRequest {
  Id eventId = 0;
  std::string eventGuid;
  bool atPosition = true;
  float position[3] = {0.0f, 0.0f, 0.0f};  // the emitter, in the listener's world
  ListenerPose listener;
};

class SpatialPreview {
 public:
  SpatialPreview(const ProjectDocument& doc, std::string eventGuid);
  bool valid() const;

  ListenerPose& listener() { return listener_; }
  const ListenerPose& listener() const { return listener_; }
  EmitterPose& emitter() { return emitter_; }
  const EmitterPose& emitter() const { return emitter_; }
  // Top-down: x right, z up the screen. Yaw 0 faces +z, positive yaw turns toward +x.
  void MoveListener(float x, float z) { listener_.position = Vec3f{x, listener_.position.y, z}; }
  void SetListenerYaw(float radians);
  void MoveEmitter(float x, float z) { emitter_.position = Vec3f{x, emitter_.position.y, z}; }
  void SetEmitterYaw(float radians);
  void SetEmitterVelocity(float x, float z) { emitter_.velocity = Vec3f{x, 0.0f, z}; }
  // D-189: positions and facings as typed (doubles). Refused (false, nothing changes) when not
  // finite or beyond kMaxCoordinate metres / kMaxAngle radians: the float computation stays finite.
  static constexpr double kMaxCoordinate = 1.0e6;
  static constexpr double kMaxAngle = 1.0e6;
  bool PlaceListener(double x, double z);
  bool PlaceEmitter(double x, double z);
  bool TurnListener(double radians);
  bool TurnEmitter(double radians);
  // Parameter values for the event's parameter curves.
  std::map<std::string, ParamScopeValues>& parameters() { return params_; }

  // --- view ------------------------------------------------------------------------------
  void SetView(float centerX, float centerZ, float metersPerPixel);
  void Zoom(float factor, float aboutX, float aboutZ);  // keeps (aboutX, aboutZ) on screen
  // Screen pixels (origin at the view's top-left, `width` x `height`) <-> world x / z.
  void WorldToScreen(float x, float z, float width, float height, float* px, float* py) const;
  void ScreenToWorld(float px, float py, float width, float height, float* x, float* z) const;
  // Shows the listener, the emitter and the maxDistance circle with a margin.
  void FitView(float width, float height);
  float metersPerPixel() const { return metersPerPixel_; }

  // --- event data ------------------------------------------------------------------------
  float MaxDistance() const;            // 0 = none
  void Cone(float* innerDeg, float* outerDeg, float* outerDb) const;
  std::vector<CurvePoint> DistanceCurve(CurveTarget target) const;  // empty if none

  SpatialResult Compute(std::uint32_t outputChannels = 2, std::uint32_t sourceChannels = 1,
                        float extrapolateSeconds = 0.0f) const;
  AuditionRequest Audition() const;

 private:
  const ProjectDocument& doc_;
  std::string guid_;
  ListenerPose listener_;
  EmitterPose emitter_;
  std::map<std::string, ParamScopeValues> params_;
  float centerX_ = 0.0f, centerZ_ = 0.0f, metersPerPixel_ = 0.1f;
};

}  // namespace snd::tool

#endif  // SND_TOOL_SPATIAL_PREVIEW_HPP
