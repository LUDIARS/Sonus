// 3D preview model: analytic values (inverse distance, maxDistance, distance curves, cone,
// azimuth and pan, extrapolation), the view, the audition request. The comparison with
// the running core is in runtime_compare_test.cpp.
#include <cmath>

#include "doc_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace snd::tool;
using namespace doctest;

SND_TEST(SpatialPreview, InverseDistanceMaxDistanceAndCone) {
  ProjectDocument doc;
  LoadBase(doc);
  doc.Erase(G(8), "distanceCurves");  // inverse distance
  SpatialPreview p(doc, G(8));
  SND_REQUIRE(p.valid());
  p.MoveEmitter(0, 10);
  SpatialResult r = p.Compute();
  SND_CHECK_NEAR(r.distance, 10.0, 1e-5);
  SND_CHECK_NEAR(r.linearGain, 0.1, 1e-6);
  SND_CHECK(!r.volumeCurve);
  p.MoveEmitter(0, 0.5f);  // inside 1 m: gain 1
  SND_CHECK_NEAR(p.Compute().linearGain, 1.0, 1e-6);
  p.MoveEmitter(30, 40);  // 50 m = maxDistance: not beyond
  SND_CHECK_NEAR(p.Compute().linearGain, 0.02, 1e-6);
  p.MoveEmitter(30, 41);
  r = p.Compute();
  SND_CHECK(r.beyondMaxDistance && r.linearGain == 0.0f);
  // Cone: inner 60, outer 180, -12 dB. The emitter faces +z; listener behind it (angle 360 -> full).
  EventEditor e(doc, G(8));
  SND_REQUIRE(e.SetCone(60, 180, -12));
  p.MoveEmitter(0, 10);
  p.SetEmitterYaw(0.0f);  // facing away from the listener at the origin
  r = p.Compute();
  SND_CHECK_NEAR(r.coneAngleDeg, 360.0, 1e-3);
  SND_CHECK_NEAR(r.coneDb, -12.0, 1e-5);
  p.SetEmitterYaw(3.14159265f);  // facing the listener: angle 0
  r = p.Compute();
  SND_CHECK_NEAR(r.coneAngleDeg, 0.0, 1e-2);
  SND_CHECK_EQ(r.coneDb, 0.0f);
  p.SetEmitterYaw(3.14159265f - 3.14159265f / 3.0f);  // 60 deg off axis: full angle 120 -> half way
  r = p.Compute();
  SND_CHECK_NEAR(r.coneAngleDeg, 120.0, 1e-2);
  SND_CHECK_NEAR(r.coneDb, -6.0, 1e-3);
  SND_CHECK_NEAR(r.linearGain, 0.1 * std::pow(10.0, -6.0 / 20.0), 1e-4);
}

SND_TEST(SpatialPreview, DistanceCurvesAzimuthAndPan) {
  ProjectDocument doc;
  LoadBase(doc);
  SND_REQUIRE(SetDistanceCurve(doc, G(8), CurveTarget::Lowpass, {{0, 20000}, {50, 1000}}));
  SND_REQUIRE(SetDistanceCurve(doc, G(8), CurveTarget::Spread, {{0, 1}, {10, 0}}));
  SND_REQUIRE(SetDistanceCurve(doc, G(8), CurveTarget::ReverbSend, {{0, 0}, {50, 1}}));
  SpatialPreview p(doc, G(8));
  // Emitter 25 m to the right: azimuth +90 deg; volume curve 1..50 -> -40 dB.
  p.MoveEmitter(25, 0);
  SpatialResult r = p.Compute();
  SND_CHECK_NEAR(r.azimuthRad, 3.14159265 / 2, 1e-5);
  SND_CHECK_NEAR(r.direction.x, 1.0, 1e-6);
  SND_CHECK(r.volumeCurve);
  SND_CHECK_NEAR(r.distanceDb, -40.0 * 24.0 / 49.0, 1e-4);
  SND_CHECK_NEAR(r.lowpassHz, 20000 - 19000 * 0.5, 1e-2);
  SND_CHECK_EQ(r.spread, 0.0f);
  SND_CHECK_NEAR(r.reverbSend, 0.5, 1e-6);
  // Point source hard right: stereo gains L 0, R 1 (pan = sin(azimuth)).
  SND_CHECK_NEAR(r.pan3[0][0], 0.0, 1e-5);
  SND_CHECK_NEAR(r.pan3[0][1], 1.0, 1e-5);
  SND_CHECK_NEAR(r.outputGain[1], r.linearGain, 1e-6);
  // Listener turned to face the emitter: straight ahead, equal gains.
  p.SetListenerYaw(3.14159265f / 2);
  r = p.Compute();
  SND_CHECK_NEAR(r.azimuthRad, 0.0, 1e-5);
  SND_CHECK_NEAR(r.pan3[0][0], r.pan3[0][1], 1e-5);
  // Pitch curve on Speed (default 1): 120 cents.
  SND_CHECK_NEAR(r.pitchCents, 120.0, 1e-3);
  // Extrapolation: 2 m/s toward -x for 5 s -> 15 m.
  p.SetListenerYaw(0);
  p.SetEmitterVelocity(-2, 0);
  SND_CHECK_NEAR(p.Compute(2, 1, 5.0f).distance, 15.0, 1e-4);
  // A 2D event: no attenuation, centre pan.
  SND_REQUIRE(doc.Erase(G(8), "is3D"));
  r = p.Compute();
  SND_CHECK(!r.is3D);
  SND_CHECK_NEAR(r.linearGain, 1.0, 1e-6);
  SND_CHECK_NEAR(r.pan3[0][0], r.pan3[0][1], 1e-6);
}

SND_TEST(SpatialPreview, ViewAndAudition) {
  ProjectDocument doc;
  LoadBase(doc);
  SpatialPreview p(doc, G(8));
  p.SetView(10, 20, 0.5f);
  float px = 0, py = 0, x = 0, z = 0;
  p.WorldToScreen(12, 21, 400, 300, &px, &py);
  SND_CHECK_NEAR(px, 204.0, 1e-4);
  SND_CHECK_NEAR(py, 148.0, 1e-4);
  p.ScreenToWorld(px, py, 400, 300, &x, &z);
  SND_CHECK_NEAR(x, 12.0, 1e-4);
  SND_CHECK_NEAR(z, 21.0, 1e-4);
  p.Zoom(2.0f, 12, 21);  // the point stays where it is on screen
  p.WorldToScreen(12, 21, 400, 300, &x, &z);
  SND_CHECK_NEAR(x, 204.0, 1e-3);
  SND_CHECK_NEAR(z, 148.0, 1e-3);
  SND_CHECK_NEAR(p.metersPerPixel(), 0.25, 1e-6);
  p.FitView(200, 200);  // maxDistance 50 fits: 110 m over 200 px
  SND_CHECK_NEAR(p.metersPerPixel(), 0.55, 1e-5);
  p.MoveEmitter(3, 4);
  const AuditionRequest a = p.Audition();
  SND_CHECK_EQ(a.eventId, snd::HashId("Shot"));
  SND_CHECK(a.atPosition && a.position[0] == 3.0f && a.position[2] == 4.0f);
}
