// Parameters and curves: point editing (order, minimum, shape), every curve of the
// project with one component, parameter rules, composition per 4.3 with scope priority,
// the ramp, undo / redo, the build output. The comparison with the runtime's own
// functions is in runtime_compare_test.cpp.
#include <cmath>

#include "doc_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"

using namespace snd::tool;
using namespace doctest;

SND_TEST(CurveEditor, PointsStayOrderedAndEvaluate) {
  ProjectDocument doc;
  LoadBase(doc);
  CurveEditor c(doc, G(8), "curves[0]");
  SND_REQUIRE(c.valid());
  SND_CHECK_EQ(c.Points().size(), std::size_t{2});
  SND_CHECK_EQ(c.AddPoint(5, 300), 1);
  SND_CHECK_EQ(c.AddPoint(5, 400), 2);  // equal x: after
  SND_CHECK_EQ(c.AddPoint(-1, 0), 0);
  SND_CHECK_EQ(c.Points().size(), std::size_t{5});
  SND_CHECK(!c.MovePoint(3, 4, 0));   // before its left neighbour (5)
  SND_CHECK(!c.MovePoint(3, 11, 0));  // beyond its right neighbour (10)
  SND_CHECK(c.MovePoint(3, 5, 500));
  SND_CHECK(!c.SetShape(1, -1));
  SND_CHECK(c.SetShape(3, 2));        // the segment 5 -> 10 is quadratic
  SND_CHECK_EQ(c.AddPoint(NAN, 0), -1);
  // Analytic values: x <= first -> first y; past the last -> last y.
  SND_CHECK_EQ(c.Evaluate(-5), 0.0f);
  SND_CHECK_EQ(c.Evaluate(100), 1200.0f);
  SND_CHECK_NEAR(c.Evaluate(2), 120.0, 1e-4);
  const std::vector<CurvePoint> p = c.Points();
  SND_REQUIRE_EQ(p.size(), std::size_t{5});
  // Points: (-1,0) (0,0) (5,300) (5,500 shape 2) (10,1200).
  SND_CHECK(p[0].x == -1 && p[1].x == 0 && p[2].x == 5 && p[3].x == 5 && p[3].shape == 2 && p[4].x == 10);
  SND_CHECK_NEAR(c.Evaluate(2.5f), 150.0, 1e-4);                                   // (0,0)-(5,300)
  SND_CHECK_NEAR(c.Evaluate(7.5f), 500 + (1200 - 500) * 0.25, 1e-3);             // t=0.5, t^2 = 0.25
  SND_CHECK_EQ(c.Evaluate(5.0f), 300.0f);                                          // first matching point
  // Removing keeps at least two.
  SND_CHECK(c.RemovePoint(0));
  SND_CHECK(c.RemovePoint(0));
  SND_CHECK(c.RemovePoint(0));
  SND_CHECK(!c.RemovePoint(0));
  SND_CHECK_EQ(c.Points().size(), std::size_t{2});
  SND_CHECK(!c.SetPoints({{0, 0, 0}}));
  SND_CHECK(!c.SetPoints({{1, 0, 0}, {0, 0, 0}}));
  // Target and compose.
  SND_CHECK(c.compose() == CurveCompose::AddCents);
  SND_CHECK(c.SetTarget(CurveTarget::Lowpass));
  SND_CHECK(c.compose() == CurveCompose::Min);
  SND_CHECK(c.SetCompose(CurveCompose::Multiply));
  SND_CHECK(c.compose() == CurveCompose::Multiply);
  SND_CHECK(!c.SetParameter(G(3)));  // a bus
  SND_CHECK(c.SetParameter("distance"));
  // Undo all: back to the file.
  while (doc.Undo()) {
  }
  SND_CHECK(doc.PendingChanges().empty());
}

SND_TEST(CurveEditor, EveryCurveOfTheProject) {
  ProjectDocument doc;
  std::map<std::string, std::string> f = BaseFiles();
  f["segments/S.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(50) + "\", \"name\": \"S\", \"asset\": \"tone\", \"bars\": 1, "
                         "\"stems\": [{\"channelFirst\": 0, \"channelCount\": 1, \"volumeCurve\": {\"parameter\": \"Speed\", "
                         "\"target\": \"volume\", \"points\": [[0, -60], [10, 0]]}}]}";
  LoadBase(doc, f);
  CurveEditor dist(doc, G(8), "distanceCurves.volume");
  SND_REQUIRE(dist.valid());
  SND_CHECK(dist.target() == CurveTarget::Volume);
  SND_CHECK(!dist.SetTarget(CurveTarget::Pitch));  // distance curves have a fixed target and input
  SND_CHECK_NEAR(dist.Evaluate(25.5f), -20.0, 1e-4);
  CurveEditor stem(doc, G(50), "stems[0].volumeCurve");
  SND_REQUIRE(stem.valid());
  SND_CHECK_NEAR(stem.Evaluate(5), -30.0, 1e-5);
  SND_CHECK(stem.MovePoint(1, 10, -6));
  // New curves.
  SND_CHECK_EQ(AddEventCurve(doc, G(8), G(6), CurveTarget::Volume, {{0, 0}, {10, -12}}), 1);
  SND_CHECK_EQ(AddEventCurve(doc, G(8), G(3), CurveTarget::Volume, {{0, 0}, {10, -12}}), -1);
  SND_CHECK(SetDistanceCurve(doc, G(8), CurveTarget::Lowpass, {{0, 20000}, {50, 2000}}));
  SND_CHECK(!SetDistanceCurve(doc, G(8), CurveTarget::Pitch, {{0, 0}, {1, 1}}));
  const DocBuild b = BuildDocument(doc);
  SND_CHECK_MSG(b.ok, "%s", Dump(b.issues).c_str());
  // The located CPNT cell holds the moved stem point? (stems are not in the field table;
  // the event's parameter curve points are.)
  const std::vector<CellRef> cells = LocateField(b, doc, G(8), "curves[1].points[1][1]");
  SND_REQUIRE_EQ(cells.size(), std::size_t{1});
  SND_CHECK_EQ(BitsToFloat(cells[0].bits), -12.0f);
}

SND_TEST(CurveEditor, ParameterRules) {
  ProjectDocument doc;
  LoadBase(doc);
  ParameterEditor p(doc, G(6));
  SND_REQUIRE(p.valid());
  SND_CHECK_EQ(p.scope(), 7u);
  SND_CHECK(!p.SetScope(0));
  SND_CHECK(p.SetScope(static_cast<std::uint32_t>(ParamScope::Emitter) | static_cast<std::uint32_t>(ParamScope::Playback)));
  SND_CHECK_EQ(p.scope(), 6u);
  SND_CHECK(!p.SetRange(5, 1));
  SND_CHECK(!p.SetRange(3, 3));  // continuous needs a span
  SND_CHECK(p.SetRange(2, 4));   // the default (1) moves inside
  SND_CHECK_EQ(p.defaultValue(), 2.0);
  SND_CHECK(!p.SetDefault(5));
  SND_CHECK(p.SetDefault(3.5));
  SND_CHECK(!p.SetRampRate(-1));
  SND_CHECK(p.SetRampRate(0.5));
  SND_CHECK(p.SetSwitch(true));
  SND_CHECK(p.isSwitch());
  SND_CHECK(p.SetSwitch(false));
  const DocBuild b = BuildDocument(doc);
  SND_REQUIRE_MSG(b.ok, "%s", Dump(b.issues).c_str());
  const std::vector<CellRef> c = LocateField(b, doc, G(6), "default");
  SND_REQUIRE_EQ(c.size(), std::size_t{1});
  SND_CHECK_EQ(BitsToFloat(c[0].bits), 3.5f);
  SND_CHECK_EQ(LocateField(b, doc, G(6), "scope")[0].bits, 6u);
}

SND_TEST(CurveEditor, CompositionAndScopePriority) {
  ProjectDocument doc;
  LoadBase(doc);
  // Shot: curve 0 Speed -> pitch (0..1200 over 0..10). Add: Speed -> volume, distance ->
  // volume, Speed -> lowpass twice (min), distance -> highpass, Speed -> pan, Speed ->
  // spread twice (multiply).
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), G(6), CurveTarget::Volume, {{0, 0}, {10, -10}}), 1);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), "distance", CurveTarget::Volume, {{0, 0}, {100, -20}}), 2);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), G(6), CurveTarget::Lowpass, {{0, 20000}, {10, 1000}}), 3);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), "distance", CurveTarget::Lowpass, {{0, 8000}, {100, 8000}}), 4);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), "distance", CurveTarget::Highpass, {{0, 100}, {100, 300}}), 5);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), G(6), CurveTarget::Pan, {{0, -1}, {10, 1}}), 6);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), G(6), CurveTarget::Spread, {{0, 0.5}, {10, 0.5}}), 7);
  SND_REQUIRE_EQ(AddEventCurve(doc, G(8), "distance", CurveTarget::Spread, {{0, 0.5}, {100, 0.5}}), 8);
  std::map<std::string, ParamScopeValues> v;
  // Nothing set: Speed's default (1).
  ComposeResult r = ComposeEvent(doc, G(8), v, 50);
  SND_CHECK_NEAR(r.pitchCents, 120.0, 1e-3);
  SND_CHECK_NEAR(r.volumeDb, -1.0 + -10.0, 1e-4);
  SND_CHECK_EQ(std::string(r.curves[0].inputFrom), std::string("default"));
  // Global 5, emitter 8, playback 2: playback wins.
  ParamScopeValues s;
  s.hasGlobal = true;
  s.global = 5;
  v[G(6)] = s;
  r = ComposeEvent(doc, G(8), v, 50);
  SND_CHECK_NEAR(r.pitchCents, 600.0, 1e-3);
  s.hasEmitter = true;
  s.emitter = 8;
  v[G(6)] = s;
  r = ComposeEvent(doc, G(8), v, 50);
  SND_CHECK_NEAR(r.pitchCents, 960.0, 1e-3);
  SND_CHECK_EQ(std::string(r.curves[0].inputFrom), std::string("emitter"));
  s.hasPlayback = true;
  s.playback = 2;
  v[G(6)] = s;
  r = ComposeEvent(doc, G(8), v, 50);
  SND_CHECK_NEAR(r.pitchCents, 240.0, 1e-3);
  SND_CHECK_NEAR(r.volumeDb, -2.0 + -10.0, 1e-4);           // dB add
  SND_CHECK_NEAR(r.lowpassHz, 8000.0, 1e-3);                 // min(20000 - 19000*0.2 = 16200, 8000)
  SND_CHECK_NEAR(r.highpassHz, 200.0, 1e-3);                 // the only highpass
  SND_CHECK_NEAR(r.pan, -0.6, 1e-5);                         // added
  SND_CHECK_NEAR(r.spread, 0.25, 1e-6);                      // multiplied
  SND_CHECK_EQ(r.blend3D, 1.0);                              // no curve
  SND_CHECK(r.outsideScope.empty());
  // A value at an undeclared scope is reported (the runtime still reads it).
  ParameterEditor(doc, G(6)).SetScope(static_cast<std::uint32_t>(ParamScope::Global));
  r = ComposeEvent(doc, G(8), v, 50);
  SND_CHECK(r.outsideScope.size() == 1u && r.outsideScope[0] == G(6));
}

SND_TEST(CurveEditor, RampFollowsTheRuntimeRule) {
  // 5 units/s, 256-frame blocks at 48 kHz: 0.026666 units per block.
  const std::vector<float> up = RampTrajectory(0.0f, 0.1f, 5.0f, false, 256, 48000, 6);
  const float step = 5.0f * (256.0f / 48000.0f);
  SND_CHECK_NEAR(up[0], step, 1e-7);
  SND_CHECK_NEAR(up[2], 3 * step, 1e-6);
  SND_CHECK_EQ(up[3], 0.1f);  // 4 * step > 0.1: arrives, no overshoot
  SND_CHECK_EQ(up[5], 0.1f);
  const std::vector<float> down = RampTrajectory(1.0f, 0.0f, 5.0f, false, 256, 48000, 1);
  SND_CHECK_NEAR(down[0], 1.0f - step, 1e-6);
  SND_CHECK_EQ(RampTrajectory(0.0f, 7.0f, 0.0f, false, 256, 48000, 1)[0], 7.0f);  // 0 = at once
  SND_CHECK_EQ(RampTrajectory(0.0f, 7.0f, 5.0f, true, 256, 48000, 1)[0], 7.0f);   // switch: at once
}
