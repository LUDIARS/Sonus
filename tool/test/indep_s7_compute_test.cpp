// Independent tests of group M: what the editors compute (properties F* of
// tool/test/indep_s7_model_properties.md), with analytical expectations.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "indep_s7_fixture.hpp"
#include "indep_s7_ops.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace indep_s7;
using namespace snd::tool;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

std::string ParamWithAllScopes(ProjectDocument& doc) {
  json::Value init = json::MakeObject();
  json::Add(init, "kind", String("continuous"));
  json::Add(init, "min", Number(0));
  json::Add(init, "max", Number(10));
  json::Add(init, "default", Number(1));
  return doc.Create("parameter", "All", &init);
}
}  // namespace

// --- F1: curve evaluation ------------------------------------------------------------------------------

SND_TEST(IndepS7Calc, F1_CurveEvaluation) {
  const std::vector<CurvePoint> lin = {{0, 0, 0}, {10, 10, 0}};
  SND_CHECK_EQ(EvaluateCurve(lin, 5.0f), 5.0f);
  SND_CHECK_EQ(EvaluateCurve(lin, -100.0f), 0.0f);
  SND_CHECK_EQ(EvaluateCurve(lin, 0.0f), 0.0f);
  SND_CHECK_EQ(EvaluateCurve(lin, 10.0f), 10.0f);
  SND_CHECK_EQ(EvaluateCurve(lin, 1e30f), 10.0f);
  const std::vector<CurvePoint> sq = {{0, 0, 2}, {10, 10, 0}, {20, 0, 0.5}};
  SND_CHECK_NEAR(EvaluateCurve(sq, 5.0f), 2.5f, 1e-6f);   // 10 * 0.5^2
  SND_CHECK_NEAR(EvaluateCurve(sq, 15.0f), 5.0f, 1e-6f);  // the left point's shape (0) applies
  const std::vector<CurvePoint> down = {{0, 0, 0}, {10, -10, 3}, {20, -20, 0}};
  SND_CHECK_NEAR(EvaluateCurve(down, 15.0f), -10.0f - 10.0f * 0.125f, 1e-5f);
  // Equal x: t = 1 (the right point's y) between them; after: the last segment.
  const std::vector<CurvePoint> step = {{0, 0, 0}, {5, 1, 0}, {5, 3, 0}, {10, 3, 0}};
  SND_CHECK_NEAR(EvaluateCurve(step, 2.5f), 0.5f, 1e-6f);
  SND_CHECK_NEAR(EvaluateCurve(step, 7.5f), 3.0f, 1e-6f);
  // Degenerate inputs do not crash.
  (void)EvaluateCurve({}, 1.0f);
  SND_CHECK_EQ(EvaluateCurve({{3, 7, 0}}, -5.0f), 7.0f);
  SND_CHECK_EQ(EvaluateCurve({{3, 7, 0}}, 5.0f), 7.0f);
  (void)EvaluateCurve(lin, kNaNf);
  (void)EvaluateCurve(lin, std::numeric_limits<float>::infinity());
}

// --- F2..F4: composition and scopes ------------------------------------------------------------------------

SND_TEST(IndepS7Calc, F2_ComposeRulesOf43) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string all = ParamWithAllScopes(doc);
  SND_REQUIRE(!all.empty());
  // Replace SE/Shot's curves: two per target, input "All".
  SND_REQUIRE(doc.Erase(kShot, "curves"));
  struct C {
    CurveTarget t;
    float y0, y1;
  };
  const C curves[] = {{CurveTarget::Volume, 0, -12}, {CurveTarget::Volume, -3, -3}, {CurveTarget::Pitch, 0, 200},
                      {CurveTarget::Pitch, 100, 100}, {CurveTarget::Lowpass, 20000, 2000}, {CurveTarget::Lowpass, 5000, 5000},
                      {CurveTarget::Highpass, 20, 220}, {CurveTarget::Highpass, 100, 100}, {CurveTarget::Spread, 1, 0},
                      {CurveTarget::Spread, 0.5, 0.5}};
  for (const auto& c : curves) SND_REQUIRE(AddEventCurve(doc, kShot, all, c.t, {{0, c.y0, 0}, {10, c.y1, 0}}) >= 0);
  std::map<std::string, ParamScopeValues> v;
  v[all].hasGlobal = true;
  v[all].global = 5;
  const ComposeResult r = ComposeEvent(doc, kShot, v, 0);
  SND_CHECK_NEAR(r.volumeDb, -6.0 + -3.0, 1e-5);      // dB added
  SND_CHECK_NEAR(r.pitchCents, 100.0 + 100.0, 1e-4);  // cents added
  SND_CHECK_NEAR(r.lowpassHz, 5000.0, 1e-3);          // min(11000, 5000)
  SND_CHECK_NEAR(r.highpassHz, 120.0, 1e-3);          // max(120, 100)
  SND_CHECK_NEAR(r.spread, 0.5 * 0.5, 1e-6);          // others: multiplied
  SND_CHECK_EQ(r.curves.size(), 10u);
  for (const auto& c : r.curves) {
    SND_CHECK_NEAR(c.input, 5.0, 1e-9);
    SND_CHECK_EQ(std::string(c.inputFrom), std::string("global"));
    SND_CHECK(c.compose == DefaultCompose(c.target));
  }
  SND_CHECK(DefaultCompose(CurveTarget::Volume) == CurveCompose::AddDb);
  SND_CHECK(DefaultCompose(CurveTarget::Pitch) == CurveCompose::AddCents);
  SND_CHECK(DefaultCompose(CurveTarget::Lowpass) == CurveCompose::Min);
  SND_CHECK(DefaultCompose(CurveTarget::Highpass) == CurveCompose::Max);
  for (CurveTarget t : {CurveTarget::Send, CurveTarget::EffectParam, CurveTarget::Spread, CurveTarget::ReverbSend, CurveTarget::Blend3D})
    SND_CHECK(DefaultCompose(t) == CurveCompose::Multiply);
  // A written compose is used for the targets the runtime leaves to it.
  CurveEditor c9(doc, kShot, "curves[9]");
  SND_REQUIRE(c9.SetCompose(CurveCompose::Min));
  const ComposeResult r2 = ComposeEvent(doc, kShot, v, 0);
  SND_CHECK_NEAR(r2.spread, 0.5, 1e-6);  // min(0.5, 0.5)
  // No curve for a target: neutral values.
  SND_CHECK_EQ(r.send, 1.0);
  SND_CHECK_EQ(r.reverbSend, 1.0);
  SND_CHECK_EQ(r.blend3D, 1.0);
  SND_CHECK_EQ(r.pan, 0.0);
}

// D-179a: the tool computes what the runtime does: pan curves are added (4.3's "multiply" for
// other targets is the gap C-13 takes to B).
SND_TEST(IndepS7Calc, F2_PanCompositionIsAdded_D179a) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string all = ParamWithAllScopes(doc);
  SND_REQUIRE(doc.Erase(kShot, "curves"));
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Pan, {{0, 0.5, 0}, {10, 0.5, 0}}) >= 0);
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Pan, {{0, 0.5, 0}, {10, 0.5, 0}}) >= 0);
  std::map<std::string, ParamScopeValues> v;
  const ComposeResult r = ComposeEvent(doc, kShot, v, 0);
  SND_CHECK_NEAR(r.pan, 1.0, 1e-6);
}

SND_TEST(IndepS7Calc, F3_F4_ScopePriority) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string all = ParamWithAllScopes(doc);
  SND_REQUIRE(doc.Erase(kShot, "curves"));
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Volume, {{0, 0, 0}, {10, -10, 0}}) >= 0);   // dB = -input
  SND_REQUIRE(AddEventCurve(doc, kShot, kSpeed, CurveTarget::Pitch, {{0, 0, 0}, {10, 10, 0}}) >= 0);  // cents = input
  for (int mask = 0; mask < 8; ++mask) {
    std::map<std::string, ParamScopeValues> v;
    ParamScopeValues& p = v[all];
    p.hasGlobal = mask & 1;
    p.global = 3;
    p.hasEmitter = mask & 2;
    p.emitter = 5;
    p.hasPlayback = mask & 4;
    p.playback = 7;
    const double expect = (mask & 4) ? 7 : (mask & 2) ? 5 : (mask & 1) ? 3 : 1;  // default 1
    const char* from = (mask & 4) ? "playback" : (mask & 2) ? "emitter" : (mask & 1) ? "global" : "default";
    const ComposeResult r = ComposeEvent(doc, kShot, v, 0);
    SND_CHECK_NEAR(r.volumeDb, -expect, 1e-5);
    SND_REQUIRE(!r.curves.empty());
    SND_CHECK_EQ(std::string(r.curves[0].inputFrom), std::string(from));
    // Speed has no values: its default 2.
    SND_CHECK_NEAR(r.pitchCents, 2.0, 1e-5);
    SND_CHECK(r.outsideScope.empty());
  }
  // Speed declares global and emitter: a playback value is used but reported.
  std::map<std::string, ParamScopeValues> v;
  v[kSpeed].hasPlayback = true;
  v[kSpeed].playback = 9;
  const ComposeResult r = ComposeEvent(doc, kShot, v, 0);
  SND_CHECK_NEAR(r.pitchCents, 9.0, 1e-5);
  SND_REQUIRE(r.outsideScope.size() == 1u);
  SND_CHECK_EQ(r.outsideScope[0], std::string(kSpeed));
  // A distance curve input reads the distance.
  SND_REQUIRE(AddEventCurve(doc, kShot, "distance", CurveTarget::Lowpass, {{0, 20000, 0}, {100, 1000, 0}}) >= 0);
  const ComposeResult rd = ComposeEvent(doc, kShot, {}, 50);
  SND_CHECK_NEAR(rd.lowpassHz, 10500.0, 1e-2);
}

// --- F5: ramp ------------------------------------------------------------------------------------------------

SND_TEST(IndepS7Calc, F5_RampTrajectory) {
  // 5 units / s, 480-frame blocks at 48 kHz: 0.05 per block.
  const auto up = RampTrajectory(0.0f, 1.0f, 5.0f, false, 480, 48000, 40);
  SND_REQUIRE(up.size() == 40u);
  for (std::size_t k = 1; k < up.size(); ++k) {
    SND_CHECK(up[k] >= up[k - 1]);
    SND_CHECK(up[k] <= 1.0f);
    if (up[k] < 1.0f && up[k - 1] > 0.0f) SND_CHECK_NEAR(up[k] - up[k - 1], 0.05f, 1e-5f);
  }
  SND_CHECK_NEAR(up[0], 0.05f, 0.05f + 1e-6f);  // either the start or after one block
  SND_CHECK_EQ(up.back(), 1.0f);
  // Reaches the target exactly, never overshoots (step does not divide the distance).
  const auto odd = RampTrajectory(0.0f, 0.12f, 5.0f, false, 480, 48000, 10);
  for (float x : odd) SND_CHECK(x <= 0.12f);
  SND_CHECK_EQ(odd.back(), 0.12f);
  // Downwards.
  const auto down = RampTrajectory(10.0f, 9.0f, 50.0f, false, 480, 48000, 5);
  for (std::size_t k = 1; k < down.size(); ++k) SND_CHECK(down[k] <= down[k - 1] && down[k] >= 9.0f);
  SND_CHECK_EQ(down.back(), 9.0f);
  // 0 = at once; switches at once.
  for (float x : RampTrajectory(0.0f, 3.0f, 0.0f, false, 256, 48000, 3)) SND_CHECK_EQ(x, 3.0f);
  for (float x : RampTrajectory(0.0f, 3.0f, 0.001f, true, 256, 48000, 3)) SND_CHECK_EQ(x, 3.0f);
  // Degenerate: no crash, finite.
  for (float x : RampTrajectory(0.0f, 3.0f, 1.0f, false, 0, 0, 3)) SND_CHECK(std::isfinite(x));
  for (float x : RampTrajectory(0.0f, kNaNf, 1.0f, false, 256, 48000, 3)) (void)x;
  SND_CHECK(RampTrajectory(0.0f, 3.0f, 1.0f, false, 256, 48000, 0).empty());
}

// --- F6, F7: 3D ----------------------------------------------------------------------------------------------

SND_TEST(IndepS7Calc, F6_DistanceAndInverseDistance) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SpatialPreview sp(doc, kEngine);
  SND_REQUIRE(sp.valid());
  SND_CHECK_EQ(sp.MaxDistance(), 50.0f);
  float in, out, db;
  sp.Cone(&in, &out, &db);
  SND_CHECK(in == 90.0f && out == 180.0f && db == -12.0f);
  sp.SetEmitterYaw(static_cast<float>(kPi));  // faces the listener
  sp.MoveEmitter(0, 5);
  SpatialResult r = sp.Compute();
  SND_CHECK(r.is3D);
  SND_CHECK_NEAR(r.distance, 5.0f, 1e-5f);
  SND_CHECK_NEAR(r.distanceGain, 0.2f, 1e-6f);
  SND_CHECK(!r.volumeCurve);
  SND_CHECK_NEAR(r.coneDb, 0.0f, 1e-4f);
  SND_CHECK_NEAR(r.coneAngleDeg, 0.0f, 0.05f);
  SND_CHECK_NEAR(r.linearGain, 0.2f, 1e-5f);
  SND_CHECK_NEAR(r.azimuthRad, 0.0f, 1e-5f);
  // Closer than 1 m: gain 1.
  sp.MoveEmitter(0, 0.5f);
  r = sp.Compute();
  SND_CHECK_NEAR(r.distanceGain, 1.0f, 1e-6f);
  // Beyond maxDistance: 0.
  sp.MoveEmitter(0, 60);
  r = sp.Compute();
  SND_CHECK(r.beyondMaxDistance);
  SND_CHECK_EQ(r.distanceGain, 0.0f);
  SND_CHECK_EQ(r.linearGain, 0.0f);
  // Exactly at maxDistance: not beyond, 1/50.
  sp.MoveEmitter(0, 50);
  r = sp.Compute();
  SND_CHECK(!r.beyondMaxDistance);
  SND_CHECK_NEAR(r.distanceGain, 0.02f, 1e-6f);
  // Velocity extrapolation.
  sp.MoveEmitter(0, 5);
  sp.SetEmitterVelocity(0, 10);
  r = sp.Compute(2, 1, 0.5f);
  SND_CHECK_NEAR(r.distance, 10.0f, 1e-4f);
  // A volume distance curve replaces inverse distance.
  sp.SetEmitterVelocity(0, 0);
  SND_REQUIRE(SetDistanceCurve(doc, kEngine, CurveTarget::Volume, {{0, 0, 0}, {10, -20, 0}}));
  SpatialPreview sp2(doc, kEngine);
  sp2.SetEmitterYaw(static_cast<float>(kPi));
  sp2.MoveEmitter(0, 5);
  r = sp2.Compute();
  SND_CHECK(r.volumeCurve);
  SND_CHECK_NEAR(r.distanceDb, -10.0f, 1e-4f);
  SND_CHECK_NEAR(r.distanceGain, 1.0f, 1e-6f);
  SND_CHECK_NEAR(r.linearGain, std::pow(10.0f, -10.0f / 20.0f), 1e-5f);
  // A 2D event is not attenuated.
  SpatialPreview flat(doc, kHit);
  flat.MoveEmitter(0, 40);
  r = flat.Compute();
  SND_CHECK(!r.is3D);
  SND_CHECK_NEAR(r.linearGain, 1.0f, 1e-6f);
}

SND_TEST(IndepS7Calc, F6_ConeGain) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SpatialPreview sp(doc, kEngine);
  sp.MoveEmitter(0, 5);  // the listener is at -z from the emitter
  // Off-axis angle a (between the emitter's forward and the way to the listener):
  // full angle 2a; inner 90 / outer 180 / -12 dB.
  const struct {
    double offAxisDeg;
    double db;
  } cases[] = {{0, 0}, {30, 0}, {45, 0}, {67.5, -6}, {90, -12}, {135, -12}, {180, -12}};
  for (const auto& c : cases) {
    // Forward pointing at the listener is yaw pi; turn away by the off-axis angle.
    sp.SetEmitterYaw(static_cast<float>(kPi - c.offAxisDeg * kPi / 180.0));
    const SpatialResult r = sp.Compute();
    SND_CHECK_NEAR(r.coneAngleDeg, static_cast<float>(2 * c.offAxisDeg), 0.05f);
    SND_CHECK_NEAR(r.coneDb, static_cast<float>(c.db), 0.02f);
    SND_CHECK_NEAR(r.gainDb, static_cast<float>(c.db), 0.02f);
  }
}

SND_TEST(IndepS7Calc, F7_ListenerFrameAndPan) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SpatialPreview sp(doc, kEngine);
  sp.MoveEmitter(5, 0);  // to the right of a listener facing +z
  SpatialResult r = sp.Compute();
  SND_CHECK_NEAR(r.azimuthRad, static_cast<float>(kPi / 2), 1e-4f);
  SND_CHECK_NEAR(r.direction.x, 1.0f, 1e-4f);
  SND_CHECK_NEAR(r.direction.z, 0.0f, 1e-4f);
  // Mono to stereo: louder on the right.
  SND_CHECK(r.outputGain[1] > r.outputGain[0]);
  // Turn the listener toward +x: the emitter is ahead.
  sp.SetListenerYaw(static_cast<float>(kPi / 2));
  r = sp.Compute();
  SND_CHECK_NEAR(r.azimuthRad, 0.0f, 1e-4f);
  SND_CHECK_NEAR(r.outputGain[0], r.outputGain[1], 1e-4f);
  // Behind.
  sp.SetListenerYaw(0);
  sp.MoveEmitter(0, -5);
  r = sp.Compute();
  SND_CHECK_NEAR(std::fabs(r.azimuthRad), static_cast<float>(kPi), 1e-4f);
  // Left.
  sp.MoveEmitter(-5, 0);
  r = sp.Compute();
  SND_CHECK_NEAR(r.azimuthRad, static_cast<float>(-kPi / 2), 1e-4f);
  SND_CHECK(r.outputGain[0] > r.outputGain[1]);
  // The listener in another place: only the relative position matters.
  sp.MoveListener(100, 100);
  sp.MoveEmitter(95, 100);
  const SpatialResult r2 = sp.Compute();
  SND_CHECK_NEAR(r2.distance, r.distance, 1e-3f);
  SND_CHECK_NEAR(r2.azimuthRad, r.azimuthRad, 1e-4f);
  // Coincident emitter and listener: finite results.
  sp.MoveEmitter(100, 100);
  const SpatialResult r3 = sp.Compute();
  SND_CHECK(std::isfinite(r3.linearGain) && std::isfinite(r3.azimuthRad) && std::isfinite(r3.coneDb));
  for (int c = 0; c < 2; ++c) SND_CHECK(std::isfinite(r3.outputGain[c]));
  // Audition request carries the event and the poses.
  sp.MoveEmitter(3, 4);
  const AuditionRequest a = sp.Audition();
  SND_CHECK_EQ(a.eventId, snd::HashId("SE/Engine"));
  SND_CHECK_EQ(a.eventGuid, std::string(kEngine));
  SND_CHECK(a.position[0] == 3.0f && a.position[2] == 4.0f);
  SND_CHECK(a.listener.position.x == 100.0f);
}

SND_TEST(IndepS7Calc, F6_ViewTransformsRoundTrip) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  SpatialPreview sp(doc, kEngine);
  sp.SetView(10, -5, 0.25f);
  for (float x : {-20.0f, 0.0f, 10.0f, 33.3f}) {
    for (float z : {-40.0f, -5.0f, 7.0f}) {
      float px, py, bx, bz;
      sp.WorldToScreen(x, z, 800, 600, &px, &py);
      sp.ScreenToWorld(px, py, 800, 600, &bx, &bz);
      SND_CHECK_NEAR(bx, x, 1e-3f);
      SND_CHECK_NEAR(bz, z, 1e-3f);
    }
  }
  float px, py;
  sp.WorldToScreen(10, -5, 800, 600, &px, &py);
  SND_CHECK_NEAR(px, 400.0f, 1e-3f);
  SND_CHECK_NEAR(py, 300.0f, 1e-3f);
  // z up the screen.
  float px2, py2;
  sp.WorldToScreen(10, -4, 800, 600, &px2, &py2);
  SND_CHECK(py2 < py);
  // Zoom keeps the anchor.
  float ax, ay;
  sp.WorldToScreen(20, 0, 800, 600, &ax, &ay);
  sp.Zoom(2.0f, 20, 0);
  float ax2, ay2;
  sp.WorldToScreen(20, 0, 800, 600, &ax2, &ay2);
  SND_CHECK_NEAR(ax2, ax, 1e-2f);
  SND_CHECK_NEAR(ay2, ay, 1e-2f);
  // Fit: the listener, the emitter and the maxDistance circle are on screen.
  sp.MoveEmitter(30, 30);
  sp.FitView(800, 600);
  for (const auto& p : {std::pair<float, float>{0, 0}, {30, 30}, {50, 0}, {-50, 0}, {0, 50}, {0, -50}}) {
    float sx, sy;
    sp.WorldToScreen(p.first, p.second, 800, 600, &sx, &sy);
    SND_CHECK(sx >= 0 && sx <= 800 && sy >= 0 && sy <= 600);
  }
}

// --- F8..F11: tempo map, tap tempo --------------------------------------------------------------------

SND_TEST(IndepS7Calc, F8_F9_TempoMapAnalytic) {
  TempoMap m({{7680, 120, 3, 4}, {0, 120, 4, 4}});  // any order
  SND_CHECK_EQ(m.points()[0].tick, 0u);
  SND_CHECK_NEAR(m.BarPosition(7680), 2.0, 1e-12);
  SND_CHECK_NEAR(m.TickAtBar(3), 10560.0, 1e-9);
  SND_CHECK_NEAR(m.BarPosition(10560), 3.0, 1e-12);
  const MusicalPosition mp = m.Musical(10560 + 960 + 5);
  SND_CHECK_EQ(mp.bar, 3);
  SND_CHECK_EQ(mp.beat, 1);
  SND_CHECK_EQ(mp.tick, 5);
  SND_CHECK_NEAR(m.TickOfMusical(mp), 10560.0 + 965.0, 1e-9);
  SND_CHECK_NEAR(m.SecondsAtTick(960), 0.5, 1e-12);
  SND_CHECK_NEAR(m.FrameAtTick(3840, 48000), 96000.0, 1e-6);
  // >= : on a beat / bar it is that point (D-69).
  SND_CHECK_NEAR(m.NextBeat(960), 960.0, 1e-9);
  SND_CHECK_NEAR(m.NextBeat(961), 1920.0, 1e-9);
  SND_CHECK_NEAR(m.NextBar(3840), 3840.0, 1e-9);
  SND_CHECK_NEAR(m.NextBar(3841), 7680.0, 1e-9);
  SND_CHECK_NEAR(m.NextBar(7681), 10560.0, 1e-9);  // 3/4 after the change
  SND_CHECK_NEAR(m.NextGrid(100, 16), 240.0, 1e-9);
  SND_CHECK_NEAR(m.NextGrid(0, 16), 0.0, 1e-9);
  SND_CHECK_NEAR(m.NextGrid(7700, 4), 7680.0 + 960.0, 1e-9);  // grids count from the bar line
  // 6/8: a beat is an eighth (480 ticks), a bar 2880 ticks.
  TempoMap six({{0, 90, 6, 8}});
  SND_CHECK_NEAR(six.BeatTicks(0), 480.0, 1e-12);
  SND_CHECK_NEAR(six.BarTicks(0), 2880.0, 1e-12);
  SND_CHECK_NEAR(six.SecondsAtTick(960), 60.0 / 90.0, 1e-12);  // bpm counts quarters
  SND_CHECK_NEAR(six.NextBeat(481), 960.0, 1e-9);
  // Tempo change: 120 for 2 beats, then 60.
  TempoMap tc({{0, 120, 4, 4}, {1920, 60, 4, 4}});
  SND_CHECK_NEAR(tc.SecondsAtTick(1920), 1.0, 1e-12);
  SND_CHECK_NEAR(tc.SecondsAtTick(2880), 2.0, 1e-12);
  SND_CHECK_NEAR(tc.TickAtSeconds(2.0), 2880.0, 1e-9);
  SND_CHECK_NEAR(tc.BpmAt(1919), 120.0, 1e-12);
  SND_CHECK_NEAR(tc.BpmAt(1920), 60.0, 1e-12);
  SND_CHECK_EQ(tc.PointAt(1920), 1u);
  // Empty: 120 BPM 4/4. Ticks before the first point use the first point.
  TempoMap e;
  SND_CHECK_NEAR(e.SecondsAtTick(960), 0.5, 1e-12);
  SND_CHECK_NEAR(e.BarTicks(0), 3840.0, 1e-12);
  TempoMap late({{960, 60, 4, 4}});
  SND_CHECK_EQ(late.PointAt(0), 0u);
  SND_CHECK_NEAR(late.BpmAt(0), 60.0, 1e-12);
}

SND_TEST(IndepS7Calc, F10_TempoMapRoundTrips) {
  // Signature changes on bar lines: 2 bars of 4/4, 3 of 7/8, 2 of 3/8, then 5/16 (beats no longer
  // than a quarter: see F10_HalfNoteBeatsAreRecorded for 3/2).
  TempoMap m({{0, 97, 4, 4}, {7680, 143.5, 7, 8}, {17760, 61, 3, 8}, {20640, 200, 5, 16}});
  Rng r(9);
  for (int i = 0; i < 2000; ++i) {
    const double t = static_cast<double>(r.Below(40000));
    SND_CHECK_NEAR(m.TickAtSeconds(m.SecondsAtTick(t)), t, 1e-6);
    SND_CHECK_NEAR(m.TickAtFrame(m.FrameAtTick(t, 44100), 44100), t, 1e-6);
    const MusicalPosition mp = m.Musical(t);
    SND_CHECK_NEAR(m.TickOfMusical(mp), t, 1e-6);
    SND_CHECK(m.NextBeat(t) >= t && m.NextBar(t) >= t && m.NextGrid(t, 8) >= t);
    SND_CHECK(m.NextBeat(t) <= m.NextBar(t) + 1e-9);
    // Monotone time.
    SND_CHECK(m.SecondsAtTick(t + 1) > m.SecondsAtTick(t));
  }
}

// D-179c: a signature change inside a bar (loaded that way) is computed as the runtime's
// Timeline does: compared with music's Timeline in indep_s7_recheck_test.cpp
// (IndepS7Recheck.D179c_MidBarChangeIsTheRuntimes).

// D-179d: with a beat longer than a quarter the tick within the beat is capped at 959 as in the
// runtime (snd_musical_time), and MusicalIsApproximate says so.
SND_TEST(IndepS7Calc, F10_HalfNoteBeatsAreApproximate_D179d) {
  TempoMap m({{0, 120, 3, 2}});
  const MusicalPosition mp = m.Musical(1500);  // bar 0, beat 0, 1500 ticks into the half-note beat
  SND_CHECK_EQ(mp.bar, 0);
  SND_CHECK_EQ(mp.beat, 0);
  SND_CHECK_EQ(mp.tick, 959);
  SND_CHECK(m.MusicalIsApproximate(1500));
  TempoMap q({{0, 120, 4, 4}, {3840, 120, 3, 8}});
  for (double t : {0.0, 500.0, 3839.0, 3840.0, 9000.0}) SND_CHECK(!q.MusicalIsApproximate(t));
  TempoMap w({{0, 120, 4, 4}, {3840, 120, 2, 1}});
  SND_CHECK(!w.MusicalIsApproximate(100));
  SND_CHECK(w.MusicalIsApproximate(3840 + 100));
}

SND_TEST(IndepS7Calc, F11_TapTempoAndGrid) {
  TapTempo tap;
  SND_CHECK_EQ(tap.Bpm(), 0.0);
  tap.Tap(0.25);
  SND_CHECK_EQ(tap.Bpm(), 0.0);
  for (double t : {0.75, 1.25, 1.75, 2.25}) tap.Tap(t);
  SND_CHECK_NEAR(tap.Bpm(), 120.0, 1e-9);
  SND_CHECK_NEAR(tap.OffsetSeconds(), 0.25, 1e-9);
  // Late start: offset modulo one beat.
  TapTempo t2;
  for (int i = 0; i < 8; ++i) t2.Tap(10.1 + i * 0.6);
  SND_CHECK_NEAR(t2.Bpm(), 100.0, 1e-9);
  const double off = t2.OffsetSeconds();
  SND_CHECK(off >= 0.0 && off < 0.6);
  SND_CHECK_NEAR(std::fmod(10.1 - off, 0.6), 0.0, 1e-6);
  // Symmetric jitter: least squares gives the mean interval.
  TapTempo t3;
  const double times[] = {0.0, 0.52, 0.98, 1.52, 1.98};  // +-0.02 around 0.5 s
  for (double t : times) t3.Tap(t);
  SND_CHECK_NEAR(t3.Bpm(), 120.0, 1.0);
  t3.Reset();
  SND_CHECK_EQ(t3.count(), 0u);
  // Same time twice: no division by zero.
  TapTempo t4;
  t4.Tap(1.0);
  t4.Tap(1.0);
  SND_CHECK(std::isfinite(t4.Bpm()));
  // Grid.
  const auto grid = BeatGrid(120, 0.25, 4, 4, 2.0, 48000);
  SND_REQUIRE(grid.size() >= 4u);
  std::size_t first = 0;
  while (first < grid.size() && grid[first].seconds < 0.25 - 1e-9) ++first;
  SND_REQUIRE(first + 3 < grid.size());
  SND_CHECK_NEAR(grid[first].seconds, 0.25, 1e-9);
  SND_CHECK_NEAR(grid[first].frame, 12000.0, 1e-6);
  SND_CHECK(grid[first].barLine);
  SND_CHECK_NEAR(grid[first + 1].seconds, 0.75, 1e-9);
  SND_CHECK(!grid[first + 1].barLine);
  for (const auto& g : grid) SND_CHECK(g.seconds >= -1e-9 && g.seconds <= 2.0 + 1e-9);
  // 6/8 at 120 BPM: a beat is an eighth = 0.25 s.
  const auto g68 = BeatGrid(120, 0.0, 6, 8, 3.0, 1000);
  SND_REQUIRE(g68.size() >= 7u);
  SND_CHECK_NEAR(g68[1].seconds - g68[0].seconds, 0.25, 1e-9);
  SND_CHECK(g68[0].barLine && g68[6].barLine && !g68[5].barLine);
  SND_CHECK(BeatGrid(0, 0, 4, 4, 1, 48000).size() < 100000u);  // bpm 0 does not loop forever
}

// --- F12..F14: transition rules ----------------------------------------------------------------------------

namespace {
std::string AddMusicEvent(ProjectDocument& doc, const std::vector<std::string>& states) {
  json::Value init = json::MakeObject();
  json::Add(init, "bank", String(kBank));
  json::Value music = json::MakeObject();
  json::Value st = json::MakeArray();
  for (const auto& s : states) {
    json::Value o = json::MakeObject();
    json::Add(o, "name", String(s));
    json::Add(o, "playlist", String(kPlExplore));
    st.items.push_back(o);
  }
  json::Add(music, "initialState", String(states[0]));
  json::Add(music, "states", st);
  json::Add(music, "transitions", json::MakeArray());
  json::Add(init, "music", music);
  return doc.Create("event", "Music/Rules", &init);
}
TransitionRule Rule(const std::string& f, const std::string& t, Timing timing = Timing::NextBar) {
  TransitionRule r;
  r.from = f;
  r.to = t;
  r.timing = timing;
  return r;
}
std::string RuleKey(const TransitionRule& r) {
  return r.from + ">" + r.to + "/" + std::to_string(static_cast<int>(r.timing)) + "/" + std::to_string(r.fadeOutBeats);
}
}  // namespace

SND_TEST(IndepS7Calc, F12_F13_MatchAndUnreachable) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string ev = AddMusicEvent(doc, {"A", "B", "C"});
  SND_REQUIRE(!ev.empty());
  MusicEditor me(doc, ev);
  SND_CHECK_EQ(me.Match("A", "B"), -1);  // empty table: the built-in default
  SND_REQUIRE(me.AddRule(Rule("A", "B"), 0));
  SND_REQUIRE(me.AddRule(Rule("*", "B"), 1));
  SND_REQUIRE(me.AddRule(Rule("A", "*"), 2));
  SND_REQUIRE(me.AddRule(Rule("*", "*"), 3));
  SND_CHECK_EQ(me.Match("A", "B"), 0);
  SND_CHECK_EQ(me.Match("C", "B"), 1);
  SND_CHECK_EQ(me.Match("A", "C"), 2);
  SND_CHECK_EQ(me.Match("C", "A"), 3);
  SND_CHECK_EQ(me.Match("B", "B"), 1);
  SND_CHECK(me.Unreachable().empty());
  // Covered rows: A->B after A->*, anything after *->*.
  SND_REQUIRE(me.AddRule(Rule("A", "B", Timing::Immediate), 4));
  SND_REQUIRE(me.AddRule(Rule("C", "A"), 5));
  const auto un = me.Unreachable();
  SND_REQUIRE(un.size() == 2u);
  SND_CHECK_EQ(un[0].first, 4u);
  SND_CHECK(un[0].second <= 3u);
  SND_CHECK_EQ(un[1].first, 5u);
  SND_CHECK_EQ(un[1].second, 3u);
  // The same row twice: the second is covered by the first.
  ProjectDocument d2;
  SND_REQUIRE(LoadBase(d2));
  const std::string e2 = AddMusicEvent(d2, {"A", "B"});
  MusicEditor m2(d2, e2);
  SND_REQUIRE(m2.AddRule(Rule("A", "B"), 0));
  SND_REQUIRE(m2.AddRule(Rule("A", "B", Timing::Immediate), 1));
  const auto u2 = m2.Unreachable();
  SND_REQUIRE(u2.size() == 1u);
  SND_CHECK(u2[0].first == 1u && u2[0].second == 0u);
  SND_CHECK_EQ(m2.Match("A", "B"), 0);
  // Unknown state names do not match the exact rows.
  SND_CHECK_EQ(m2.Match("X", "B"), -1);
}

SND_TEST(IndepS7Calc, F14_AutoSortKeepsMatches) {
  Rng r(2024);
  const std::vector<std::string> names = {"A", "B", "C", "*"};
  std::uint32_t tables = 0;
  for (int iter = 0; iter < 300 && tables < 60; ++iter) {
    ProjectDocument doc;
    SND_REQUIRE(LoadBase(doc));
    const std::string ev = AddMusicEvent(doc, {"A", "B", "C"});
    MusicEditor me(doc, ev);
    const std::uint32_t rows = 1 + r.Below(7);
    for (std::uint32_t i = 0; i < rows; ++i) {
      TransitionRule rule = Rule(names[r.Below(4)], names[r.Below(4)], static_cast<Timing>(r.Below(7)));
      rule.fadeOutBeats = static_cast<double>(i);  // tells the rows apart
      me.AddRule(rule, i);
    }
    if (!me.Unreachable().empty()) continue;
    ++tables;
    std::map<std::string, std::string> before;
    const auto rulesBefore = me.Rules();
    for (const auto& f : {"A", "B", "C"})
      for (const auto& t : {"A", "B", "C"}) {
        const int m = me.Match(f, t);
        before[std::string(f) + t] = m < 0 ? "default" : RuleKey(rulesBefore[static_cast<std::size_t>(m)]);
      }
    SND_CHECK(me.AutoSort() || true);
    const auto rulesAfter = me.Rules();
    SND_CHECK_EQ(rulesAfter.size(), rulesBefore.size());
    // Specificity order: exact-exact, then one exact, then *->*.
    auto rank = [](const TransitionRule& x) { return (x.from == "*" ? 1 : 0) + (x.to == "*" ? 1 : 0); };
    for (std::size_t i = 1; i < rulesAfter.size(); ++i) SND_CHECK(rank(rulesAfter[i - 1]) <= rank(rulesAfter[i]));
    for (const auto& f : {"A", "B", "C"})
      for (const auto& t : {"A", "B", "C"}) {
        const int m = me.Match(f, t);
        const std::string got = m < 0 ? "default" : RuleKey(rulesAfter[static_cast<std::size_t>(m)]);
        if (got != before[std::string(f) + t]) {
          snd::test::Note("table %d: %s->%s used %s, after AutoSort %s", iter, f, t, before[std::string(f) + t].c_str(), got.c_str());
          SND_CHECK(false);
        }
      }
    SND_CHECK(me.Unreachable().empty());
  }
  SND_CHECK(tables >= 20u);
}

// --- F15, F16: lookahead and the transition preview -----------------------------------------------------------

SND_TEST(IndepS7Calc, F15_MinimumLookahead) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MusicEditor me(doc, kBgm);
  MusicSettings s;  // 48000, 256, 30 ms
  const double block = 256.0 * 1000.0 / 48000.0;
  auto find = [](const std::vector<LookaheadEntry>& v, const std::string& kind, const std::string& name) -> const LookaheadEntry* {
    for (const auto& e : v)
      if (e.kind == kind && e.name == name) return &e;
    return nullptr;
  };
  auto la = me.MinimumLookahead(s);
  const LookaheadEntry* explore = find(la, "state", "Explore");
  const LookaheadEntry* battle = find(la, "state", "Battle");
  const LookaheadEntry* bridge = find(la, "bridge", "Seg/Bridge");
  const LookaheadEntry* sting = find(la, "stinger", "Sting/Hit");
  SND_REQUIRE(explore && battle && bridge && sting);
  SND_CHECK_NEAR(explore->lookaheadMs, block, 1e-9);  // stream from its head with a preload
  SND_CHECK_EQ(explore->primeMs, 0.0);
  SND_CHECK_NEAR(battle->lookaheadMs, block + 30.0, 1e-9);  // a stream without preload
  SND_CHECK_EQ(battle->reason, std::string("stream"));
  SND_CHECK_NEAR(bridge->lookaheadMs, block, 1e-9);  // in memory
  SND_CHECK_EQ(bridge->reason, std::string("memory"));
  SND_CHECK_NEAR(sting->lookaheadMs, block, 1e-9);
  for (const auto& e : la) SND_CHECK_NEAR(e.lookaheadMs, block + e.primeMs, 1e-9);
  // Settings: other block and prime.
  MusicSettings s2;
  s2.sampleRate = 44100;
  s2.blockFrames = 1024;
  s2.streamPrimeMs = 80;
  la = me.MinimumLookahead(s2);
  SND_CHECK_NEAR(find(la, "state", "Battle")->lookaheadMs, 1024.0 * 1000.0 / 44100.0 + 80.0, 1e-9);
  // "prepare" on every rule into Battle: 0 prime.
  auto rules = me.Rules();
  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    rules[i].prepare = true;
    SND_REQUIRE(me.SetRule(i, rules[i]));
  }
  la = me.MinimumLookahead(s);
  SND_CHECK_NEAR(find(la, "state", "Battle")->lookaheadMs, block, 1e-9);
  // Only one of them prepared: the worst rule counts.
  rules[1].prepare = false;
  SND_REQUIRE(me.SetRule(1, rules[1]));
  la = me.MinimumLookahead(s);
  SND_CHECK_NEAR(find(la, "state", "Battle")->lookaheadMs, block + 30.0, 1e-9);
  // Same-bar entry into Explore without entryPreload: the stream's prime time; with it: 0.
  rules[1].start = StartPosition::SameBar;
  SND_REQUIRE(me.SetRule(1, rules[1]));
  la = me.MinimumLookahead(s);
  SND_CHECK_NEAR(find(la, "state", "Explore")->lookaheadMs, block + 30.0, 1e-9);
  SegmentEditor seg(doc, kSegA);
  SND_REQUIRE(seg.SetEntryPreload(true));
  la = me.MinimumLookahead(s);
  SND_CHECK_NEAR(find(la, "state", "Explore")->lookaheadMs, block, 1e-9);
}

SND_TEST(IndepS7Calc, F16_TransitionPreview) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MusicEditor me(doc, kBgm);
  MusicSettings s;
  const double block = 256.0 / 48000.0;
  TransitionPreview p = me.Preview("Explore", "Battle", 0.1, s);
  SND_REQUIRE(p.valid);
  SND_CHECK_EQ(p.rule, 0);
  SND_CHECK(!p.lateApplied && !p.cancelled);
  SND_CHECK_NEAR(p.requestTick, 192.0, 1e-6);
  SND_CHECK_NEAR(p.fireTick, 960.0, 1e-6);
  SND_CHECK_NEAR(p.fireSeconds, 0.5, 1e-9);
  SND_CHECK_EQ(p.fireAt.bar, 0);
  SND_CHECK_EQ(p.fireAt.beat, 1);
  SND_CHECK_NEAR(p.fadeOutSeconds, 0.25, 1e-9);
  SND_CHECK_NEAR(p.fadeInSeconds, 0.0, 1e-9);
  SND_CHECK_EQ(p.bridge, std::string(kSegBridge));
  SND_CHECK_NEAR(p.bridgeSeconds, 2.0, 1e-9);  // 1 bar at 120 BPM 4/4
  SND_CHECK_EQ(p.targetSegment, std::string(kSegB));
  SND_CHECK_NEAR(p.targetStartSeconds, 2.5, 1e-9);
  SND_CHECK_NEAR(p.targetStartTick, 0.0, 1e-9);
  // D-184 (the runtime's rules are the truth; music transition.cpp HandleSetState): the request is
  // taken at the first block start at or after it (here 256 / 48000 s blocks, at least block 1),
  // the rule's timing is evaluated from that block start (NextBeat / NextBar: a grid point at the
  // block start itself counts), and the late rule applies only when that point is earlier than the
  // block start + the prime of the first material (the bridge when the rule has one, else the
  // target's first segment). No extra block of lead.
  // 2 ms before a beat: taken at block 94 (0.50133 s), already past 0.5 s: the next beat 1.0 s, not late.
  p = me.Preview("Explore", "Battle", 0.5 - 0.002, s);
  SND_CHECK_NEAR(p.applySeconds, 94.0 * block, 1e-12);
  SND_CHECK(!p.lateApplied);
  SND_CHECK_NEAR(p.fireSeconds, 1.0, 1e-9);
  // The late rules (no bridge: the first material is Seg/B's stream without preload: prime =
  // streamPrimeMs 30 ms, D-179f). Request 0.47 s: taken at block 89 (0.474667 s); the next beat
  // 0.5 s is earlier than 0.474667 + 0.030 -> late.
  auto rules = me.Rules();
  rules[0].bridge.clear();
  SND_REQUIRE(me.SetRule(0, rules[0]));
  p = me.Preview("Explore", "Battle", 0.47, s);
  SND_CHECK_NEAR(p.applySeconds, 89.0 * block, 1e-12);
  SND_CHECK(p.lateApplied);
  SND_CHECK_NEAR(p.fireSeconds, 1.0, 1e-9);  // nextGrid: the following beat
  // startLate: the block start + the prime.
  rules[0].late = LateRule::StartLate;
  SND_REQUIRE(me.SetRule(0, rules[0]));
  p = me.Preview("Explore", "Battle", 0.47, s);
  SND_CHECK(p.lateApplied);
  SND_CHECK_NEAR(p.fireSeconds, 89.0 * block + 0.030, 1e-6);
  rules[0].late = LateRule::Cancel;
  SND_REQUIRE(me.SetRule(0, rules[0]));
  p = me.Preview("Explore", "Battle", 0.47, s);
  SND_CHECK(p.cancelled);
  // Far enough (0.4 s: block 75 = 0.4 s, 0.1 s before the beat): no late rule.
  p = me.Preview("Explore", "Battle", 0.4, s);
  SND_CHECK(!p.cancelled && !p.lateApplied);
  SND_CHECK_NEAR(p.fireSeconds, 0.5, 1e-9);
  // Battle -> Explore: the wildcard row, next bar, 1 beat fades at 120 BPM.
  p = me.Preview("Battle", "Explore", 0.1, s);
  SND_CHECK_EQ(p.rule, 1);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);
  SND_CHECK_NEAR(p.fadeOutSeconds, 0.5, 1e-9);
  SND_CHECK_NEAR(p.fadeInSeconds, 0.5, 1e-9);
  SND_CHECK_EQ(p.targetSegment, std::string(kSegA));
  SND_CHECK_NEAR(p.targetStartSeconds, 2.0, 1e-9);
  // Exactly on a bar (2.0 s = block 375's start): taken at 2.0 s, the bar at the block start counts
  // and Seg/A's stream is preloaded (prime 0): that bar, 2.0 s (D-184; before: 4.0 s).
  p = me.Preview("Battle", "Explore", 2.0, s);
  SND_CHECK_NEAR(p.applySeconds, 2.0, 1e-12);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);
  // Just after it (2.001 s: block 376): the next bar, 4.0 s.
  p = me.Preview("Battle", "Explore", 2.001, s);
  SND_CHECK_NEAR(p.fireSeconds, 4.0, 1e-9);
  // At 0: taken at block 1 (the music starts in the From state), the first bar line after: 2.0 s.
  p = me.Preview("Battle", "Explore", 0.0, s);
  SND_CHECK_NEAR(p.applySeconds, block, 1e-12);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);
  // Unknown states.
  SND_CHECK(!me.Preview("Nope", "Explore", 0, s).valid);
  // Fades follow the tempo: Battle's segment at 60 BPM -> 1 beat out = 1 s, in at Explore's 120 = 0.5 s.
  SegmentEditor b(doc, kSegB);
  TempoPointDesc t0 = b.Tempo()[0];
  t0.bpm = 60;
  SND_REQUIRE(b.SetTempo(0, t0));
  p = me.Preview("Battle", "Explore", 0.1, s);
  SND_CHECK_NEAR(p.fadeOutSeconds, 1.0, 1e-9);
  SND_CHECK_NEAR(p.fadeInSeconds, 0.5, 1e-9);
  SND_CHECK_NEAR(p.fireSeconds, 4.0, 1e-9);  // one 4/4 bar at 60 BPM
}

SND_TEST(IndepS7Calc, F16_SameBarStart) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  // Explore's segment: 4 bars of 4/4; Battle's: 3 bars. Same bar: bar 3 of the source -> 3 mod 3 = 0.
  SegmentEditor b(doc, kSegB);
  SND_REQUIRE(b.SetBars(3));
  MusicEditor me(doc, kBgm);
  auto rules = me.Rules();
  rules[0].timing = Timing::NextBar;
  rules[0].start = StartPosition::SameBar;
  rules[0].bridge.clear();
  SND_REQUIRE(me.SetRule(0, rules[0]));
  MusicSettings s;
  // Request at 5.0 s (bar 2 + 2 beats) -> fires at bar 3 (6.0 s) -> target bar 0 (3 mod 3).
  TransitionPreview p = me.Preview("Explore", "Battle", 5.0, s);
  SND_CHECK_NEAR(p.fireSeconds, 6.0, 1e-9);
  SND_CHECK_NEAR(p.targetStartTick, 0.0, 1e-9);
  // Request at 1.0 s: fires at bar 1 (2.0 s): target bar 1 = tick 3840.
  p = me.Preview("Explore", "Battle", 1.0, s);
  SND_CHECK_NEAR(p.fireSeconds, 2.0, 1e-9);
  SND_CHECK_NEAR(p.targetStartTick, 3840.0, 1e-9);
}

// --- F17: materials -------------------------------------------------------------------------------------------

SND_TEST(IndepS7Calc, F17_MaterialResolution) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  MaterialEditor m(doc, kPhys);
  auto same = [](const std::vector<std::string>& a, std::vector<std::string> b) {
    std::vector<std::string> x = a;
    std::sort(x.begin(), x.end());
    std::sort(b.begin(), b.end());
    return x == b;
  };
  MaterialResolution r = m.Resolve(0, 2, ContactKind::Impact);  // metal x glass: pair
  SND_CHECK_EQ(r.source, std::string("pair"));
  SND_CHECK(same(r.events, {kSting}));
  r = m.Resolve(2, 0, ContactKind::Impact);  // symmetric
  SND_CHECK(r.a == 0 && r.b == 2);
  SND_CHECK(same(r.events, {kSting}));
  r = m.Resolve(0, 2, ContactKind::Slide);  // a pair overrides every kind
  SND_CHECK(same(r.events, {kSting}));
  r = m.Resolve(0, 1, ContactKind::Impact);  // metal + wood singles
  SND_CHECK_EQ(r.source, std::string("single"));
  SND_CHECK(same(r.events, {kShot, kHit}));
  r = m.Resolve(1, 2, ContactKind::Impact);  // wood single + glass -> default
  SND_CHECK_EQ(r.source, std::string("single+default"));
  SND_CHECK(same(r.events, {kHit, kEngine}));
  r = m.Resolve(2, 2, ContactKind::Impact);  // glass x glass: the default once
  SND_CHECK_EQ(r.source, std::string("default"));
  SND_CHECK(same(r.events, {kEngine}));
  r = m.Resolve(1, 1, ContactKind::Impact);  // the same event from both sides sounds once
  SND_CHECK(same(r.events, {kHit}));
  r = m.Resolve(1, 2, ContactKind::Roll);  // nothing set
  SND_CHECK_EQ(r.source, std::string("none"));
  SND_CHECK(r.events.empty());
  // Out of range materials: no crash, nothing sounds.
  r = m.Resolve(7, 0, ContactKind::Impact);
  SND_CHECK(r.events.empty() || r.source == "default");
  r = m.Resolve(0xFFFFFFFFu, 0xFFFFFFFFu, ContactKind::Break);
  // Fallbacks: every (pair, kind) that does not sound the materials' own events.
  const auto fb = m.Fallbacks();
  bool woodGlass = false, glassGlass = false, metalWood = false;
  for (const auto& f : fb) {
    woodGlass |= f.a == 1 && f.b == 2 && f.kind == ContactKind::Impact;
    glassGlass |= f.a == 2 && f.b == 2 && f.kind == ContactKind::Impact;
    metalWood |= f.a == 0 && f.b == 1 && f.kind == ContactKind::Impact;
  }
  SND_CHECK(woodGlass);
  SND_CHECK(glassGlass);
  SND_CHECK(!metalWood);
}

SND_TEST(IndepS7Calc, F17_StrengthCurvesProvisional) {
  StrengthCurves c;  // 0.1 / 1 / 10, full 100
  SND_CHECK_EQ(EvaluateStrength(c, 0.05f, 1, 1).layer, -1);
  SND_CHECK_EQ(EvaluateStrength(c, 0.5f, 1, 1).layer, 0);
  SND_CHECK_EQ(EvaluateStrength(c, 5.0f, 1, 1).layer, 1);
  SND_CHECK_EQ(EvaluateStrength(c, 50.0f, 1, 1).layer, 2);
  SND_CHECK_NEAR(EvaluateStrength(c, 50.0f, 1, 1).velocity, 0.5f, 1e-6f);
  SND_CHECK_NEAR(EvaluateStrength(c, 1000.0f, 1, 1).velocity, 1.0f, 1e-6f);
  StrengthCurves d;
  d.massToPitchCents[0] = 100;
  d.massToPitchCents[1] = -100;
  d.speedToVolumeDb[0] = -20;
  d.speedToVolumeDb[1] = 0;
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 1, 0.1f).pitchCents, 100.0f, 1e-3f);
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 100, 0.1f).pitchCents, -100.0f, 1e-3f);
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 10, 0.1f).pitchCents, 0.0f, 1e-3f);    // log midpoint
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 1e6f, 0.1f).pitchCents, -100.0f, 1e-3f);  // clamped
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 1, 1.0f).volumeDb, -10.0f, 1e-3f);
  SND_CHECK_NEAR(EvaluateStrength(d, 5, 1, 0.0f).volumeDb, -20.0f, 1e-3f);
  // Zero / negative / NaN inputs: finite outputs.
  for (float x : {0.0f, -1.0f}) {
    const StrengthResult r = EvaluateStrength(d, x, x, x);
    SND_CHECK(std::isfinite(r.velocity) && std::isfinite(r.pitchCents) && std::isfinite(r.volumeDb));
  }
  // D-180b: non-finite inputs count as 0 (results always finite, the same as for 0).
  const float inf = std::numeric_limits<float>::infinity();
  for (float x : {kNaNf, inf, -inf}) {
    const StrengthResult rn = EvaluateStrength(d, x, x, x);
    const StrengthResult r0 = EvaluateStrength(d, 0, 0, 0);
    SND_CHECK(std::isfinite(rn.velocity) && std::isfinite(rn.pitchCents) && std::isfinite(rn.volumeDb));
    SND_CHECK_EQ(rn.layer, r0.layer);
    SND_CHECK_EQ(rn.velocity, r0.velocity);
    SND_CHECK_EQ(rn.pitchCents, r0.pitchCents);
    SND_CHECK_EQ(rn.volumeDb, r0.volumeDb);
  }
}

// D-179b: a "compose" written on a volume / pitch / lowpass / highpass / pan curve is accepted
// but the runtime ignores it: the result is the runtime's rule and the curve is reported in
// composeIgnored / ComposeIgnored().
SND_TEST(IndepS7Calc, F2_WrittenComposeOnARuntimeTarget_D179b) {
  ProjectDocument doc;
  SND_REQUIRE(LoadBase(doc));
  const std::string all = ParamWithAllScopes(doc);
  SND_REQUIRE(doc.Erase(kShot, "curves"));
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Volume, {{0, -6, 0}, {10, -6, 0}}) >= 0);
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Volume, {{0, -6, 0}, {10, -6, 0}}) >= 0);
  CurveEditor c0(doc, kShot, "curves[0]");
  CurveEditor c1(doc, kShot, "curves[1]");
  SND_CHECK(!c0.ComposeIgnored());  // nothing written yet
  SND_CHECK(c0.SetCompose(CurveCompose::Multiply));
  SND_CHECK(c1.SetCompose(CurveCompose::Multiply));
  SND_CHECK(c0.compose() == CurveCompose::Multiply);
  SND_CHECK(c0.ComposeIgnored());
  const ComposeResult r = ComposeEvent(doc, kShot, {}, 0);
  SND_CHECK_NEAR(r.volumeDb, -12.0, 1e-5);  // the runtime's dB addition
  SND_CHECK(r.composeIgnored.size() == 2u);
  // Which targets the runtime ignores compose for.
  for (CurveTarget t : {CurveTarget::Volume, CurveTarget::Pitch, CurveTarget::Lowpass, CurveTarget::Highpass, CurveTarget::Pan})
    SND_CHECK(ComposeIgnored(t));
  // A compose on a target the runtime leaves to it is used and not reported.
  SND_REQUIRE(AddEventCurve(doc, kShot, all, CurveTarget::Spread, {{0, 0.5, 0}, {10, 0.5, 0}}) >= 0);
  CurveEditor c2(doc, kShot, "curves[2]");
  SND_REQUIRE(c2.SetCompose(CurveCompose::Max));
  SND_CHECK(!c2.ComposeIgnored());
  const ComposeResult r2 = ComposeEvent(doc, kShot, {}, 0);
  SND_CHECK_EQ(r2.composeIgnored.size(), 2u);
}
