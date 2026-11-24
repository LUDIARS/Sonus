// Independent tests of group U, the screens against their models (indep_s7_ui_properties.md C5,
// C6, D1..D10, E2, E3): every row and value of the model in the element tree, edits through the
// model (one undo step, the document dirty), refused edits refused from the screen too, the
// screen's own state (selection, pages) never pointing at what the document no longer has.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace iu;

namespace {

std::map<std::string, std::string> Rich() { return LargeFiles(3, 2, 6, 4, 12); }

// Union of the GUIDs of the list rows `<prefix><32 hex>` over every page (`page` is the screen's
// page field, walked from 0 until a page adds nothing new twice).
std::set<std::string> ListedOverPages(Rig& r, const std::string& prefix, std::uint32_t* page) {
  std::set<std::string> keys;
  for (std::uint32_t p = 0; p < 1000; ++p) {
    *page = p;
    r.Frame();
    std::size_t added = 0;
    for (const tela::PlacedElement* e : r.Prefixed(prefix)) {
      const std::string rest = e->element.id.substr(prefix.size());
      if (rest.size() == 32 && rest.find('.') == std::string::npos) added += keys.insert(rest).second ? 1u : 0u;
    }
    if (added == 0 && p > 0) break;
  }
  *page = 0;
  r.Frame();
  return keys;
}

std::set<std::string> KeysOf(const ProjectDocument& doc, const char* kind) {
  std::set<std::string> s;
  for (const DocObject* o : doc.OfKind(kind)) s.insert(ui::ObjectKey(o->guid));
  return s;
}

// Starts the text entry of a field by clicking it (a click without motion), types, Enter.
bool TypeInto(Rig& r, const std::string& id, const std::string& text, bool clear = true) {
  if (!r.ClickId(id)) return false;
  if (!r.app->text().active()) return false;
  if (clear) {
    for (int i = 0; i < 300; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  }
  r.Type(text);
  r.Key(ui::kKeyEnter);
  return true;
}

}  // namespace

// ============================================================================================
// Event (D1)
// ============================================================================================

// D1: the list holds every event (over its pages), on a large project too.
SND_TEST(IndepS7UiScreens, EventListShowsEveryEventOverItsPages) {
  Rig r(LargeFiles(300, 3, 6, 4, 30));
  r.Show(ui::Screen::Event);
  const std::set<std::string> listed = ListedOverPages(r, "ev.list.", &r.app->view().event.listPage);
  SND_CHECK_EQ(listed.size(), KeysOf(r.doc, "event").size());
  SND_CHECK(listed == KeysOf(r.doc, "event"));
}

// D1: clip boxes are the time line of the model mapped with the screen's axis; a model edit moves
// the box at the next declaration; ids stay (E2).
SND_TEST(IndepS7UiScreens, EventClipBoxesFollowTheModelsTimeline) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  SND_REQUIRE(r.app->view().screen == ui::Screen::Event);
  EventEditor ed(r.doc, G(8));
  const ui::EventView& v = r.app->view().event;
  SND_REQUIRE(r.El("ev.t0.lane") != nullptr);
  auto check = [&](const char* when) {
    const tela::PlacedElement* lane = r.El("ev.t0.lane");
    SND_REQUIRE(lane != nullptr);
    const Timeline tl = ed.ComputeTimeline();
    SND_REQUIRE(!tl.clips.empty());
    const tela::PlacedElement* c = r.El("ev.t0.c0");
    SND_REQUIRE(c != nullptr);
    const double x0 = lane->bounds.x + (tl.clips[0].startMs - v.scrollMs) / v.msPerPx;
    const double w = (tl.clips[0].endMs - tl.clips[0].startMs) / v.msPerPx;
    SND_CHECK_MSG(std::fabs(c->bounds.x - x0) <= 0.75, "%s: x %.2f expected %.2f", when, c->bounds.x, x0);
    SND_CHECK_MSG(std::fabs(c->bounds.width - w) <= 0.75, "%s: width %.2f expected %.2f", when, c->bounds.width, w);
  };
  check("as loaded");
  SND_REQUIRE(ed.MoveClip(0, 0, 200.0));
  r.Frame();
  check("after MoveClip");
  r.app->view().event.msPerPx = 2.0;
  r.Frame();
  check("zoomed");
  r.app->view().event.scrollMs = 100.0;
  r.Frame();
  check("scrolled");
}

// F1, F9: a horizontal drag moves startMs by dx * msPerPx (absolute from the press), one undo
// step; back to where it started in the same drag: no step; never below 0.
SND_TEST(IndepS7UiScreens, EventClipDragIsAbsoluteAndOneStep) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  const double mpp = r.app->view().event.msPerPx;
  const std::size_t u0 = r.Undos();
  r.Drag(r.El("ev.t0.c0"), 40.0f, 0.0f, 5);
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 40.0 * mpp, 1e-6);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  SND_CHECK(r.app->dirty());
  // A drag out and back in one gesture leaves no step.
  const tela::PlacedElement* c = r.El("ev.t0.c0");
  SND_REQUIRE(c != nullptr);
  const tela::Point p = uitest::Center(c);
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x + 30.0f, p.y);
  r.Frame();
  r.ptr->Move(p.x, p.y);
  r.Frame();
  r.ptr->Up(p.x, p.y);
  r.ptr->Advance(1000);
  r.Frame();
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 40.0 * mpp, 1e-6);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  SND_CHECK(!r.doc.InGroup());
  // Far left: not below 0.
  r.Drag(r.El("ev.t0.c0"), -400.0f, 0.0f, 4);
  SND_CHECK(ed.ClipNumberOf(0, 0, ClipNumber::StartMs) >= 0.0);
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0, 1e-9);
  SND_CHECK_EQ(r.Undos(), u0 + 2);
  // One undo per drag.
  SND_CHECK(r.doc.Undo());
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 40.0 * mpp, 1e-6);
  SND_CHECK(r.doc.Undo());
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0, 1e-9);
}

// F2: a vertical drag of 48 px per track moves the clip to that track (one step), never past
// the last or before the first track.
SND_TEST(IndepS7UiScreens, EventClipVerticalDragChangesTrackWithinRange) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddTrack(1));
  SND_REQUIRE(ed.AddTrack(2));
  r.Frame();
  const std::size_t u0 = r.Undos();
  r.Drag(r.El("ev.t0.c0"), 0.0f, 48.0f, 4);
  SND_CHECK_EQ(ed.ClipCount(0), 0u);
  SND_CHECK_EQ(ed.ClipCount(1), 1u);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  // Far down (beyond the last track) and far up (before the first): the clip stays on a track
  // that exists (the last / first, or where it was: decision e does not say), never lost, at most
  // one step each; the horizontal part of the drag is kept either way.
  r.Drag(r.El("ev.t1.c0"), 20.0f, 48.0f * 10.0f, 4);
  SND_CHECK_EQ(ed.ClipCount(0) + ed.ClipCount(1) + ed.ClipCount(2), 1u);
  SND_CHECK_EQ(ed.TrackCount(), 3u);
  SND_CHECK(r.Undos() <= u0 + 2);
  std::uint32_t at = ed.ClipCount(1) == 1 ? 1u : 2u;
  SND_CHECK_NEAR(ed.ClipNumberOf(at, 0, ClipNumber::StartMs), 20.0 * r.app->view().event.msPerPx, 1e-6);
  const std::size_t u1 = r.Undos();
  r.Drag(r.El("ev.t" + std::to_string(at) + ".c0"), 0.0f, -48.0f * 10.0f, 4);
  SND_CHECK_EQ(ed.ClipCount(0) + ed.ClipCount(1) + ed.ClipCount(2), 1u);
  SND_CHECK_EQ(ed.TrackCount(), 3u);
  SND_CHECK(r.Undos() <= u1 + 1);
  SND_CHECK(!r.doc.InGroup());
}

// D1: + Track / - Track change TrackCount by one (one step each); the Delete key removes the
// selected clip (one step).
SND_TEST(IndepS7UiScreens, EventTrackButtonsAndDeleteKeyAreModelSteps) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  const std::size_t u0 = r.Undos();
  SND_REQUIRE(r.ClickId("ev.track.add"));
  SND_CHECK_EQ(ed.TrackCount(), 2u);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  SND_REQUIRE(r.ClickId("ev.track.remove"));
  SND_CHECK_EQ(ed.TrackCount(), 1u);
  SND_CHECK_EQ(r.Undos(), u0 + 2);
  SND_REQUIRE(r.ClickId("ev.t0.c0"));
  SND_CHECK_EQ(r.app->view().event.clip, 0);
  Dump(r.rt(), "event with a clip selected");
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ed.ClipCount(0), 0u);
  SND_CHECK_EQ(r.Undos(), u0 + 3);
  SND_CHECK(r.doc.Undo());
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
}

// D10: the selection is the screen's own state. After an undo removed the selected clip, Delete
// must not remove another clip (the first one).
SND_TEST(IndepS7UiScreens, EventDeleteAfterUndoDoesNotRemoveAnotherClip) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddAssetClip(0, G(7), 500.0));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c1"));
  SND_REQUIRE_EQ(r.app->view().event.clip, 1);
  SND_REQUIRE(r.doc.Undo());  // the selected clip is gone
  r.Frame();
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
}

// D10: the selected track removed from outside (undo of AddTrack) - the screen does not edit a
// track that no longer exists, nor crash; - Track does not remove the remaining one silently.
SND_TEST(IndepS7UiScreens, EventSelectedTrackRemovedFromOutside) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddTrack(1));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t1.select"));
  SND_REQUIRE_EQ(r.app->view().event.track, 1u);
  SND_REQUIRE(r.doc.Undo());
  r.Frame();
  SND_CHECK(r.app->view().event.track < ed.TrackCount() || ed.TrackCount() == 0);
  SND_CHECK(FirstDuplicate(r.rt()).empty());
  SND_CHECK(r.El("ev.t0.lane") != nullptr);
}

// C5: the event name field: a taken name is refused (document unchanged, entry open); a new name
// is one step; Escape changes nothing.
SND_TEST(IndepS7UiScreens, EventNameFieldRefusesTakenNames) {
  Rig r(Rich());
  r.app->Navigate(G(8));
  r.Frame();
  const std::size_t u0 = r.Undos();
  SND_REQUIRE(TypeInto(r, "ev.list.name", "Big"));
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
  SND_CHECK(r.app->text().active());
  SND_CHECK_EQ(r.Undos(), u0);
  r.Key(ui::kKeyEscape);
  SND_CHECK(!r.app->text().active());
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
  SND_REQUIRE(TypeInto(r, "ev.list.name", "Shot 2"));
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot 2");
  SND_CHECK(!r.app->text().active());
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  // The list row of the renamed object keeps its id (E3).
  SND_CHECK(r.El("ev.list." + ui::ObjectKey(G(8))) != nullptr);
  r.Key('Z', ui::kModControl);
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
  // Empty and control characters are refused too.
  SND_REQUIRE(TypeInto(r, "ev.list.name", ""));
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
  r.Key(ui::kKeyEscape);
}

// C6: number fields: values the model refuses do not reach the document, typed or stepped; a typed
// valid value is one step.
SND_TEST(IndepS7UiScreens, EventNumberFieldsGoThroughTheModelsRules) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  const std::size_t u0 = r.Undos();
  // Max instances is 0: "-" must not make it negative.
  SND_REQUIRE(r.ClickId("ev.f.n0.dec"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::MaxInstances), 0.0);
  SND_CHECK_EQ(r.Undos(), u0);
  // Max distance: typed -5 never reaches the document as -5 (refusal or not, see the defects
  // test for the clamp), 1e999 is not a number, 75 is one step.
  SND_REQUIRE(TypeInto(r, "ev.f.n4", "-5"));
  SND_CHECK(ed.NumberOf(EventNumber::MaxDistance) >= 0.0);
  r.Key(ui::kKeyEscape);
  SND_REQUIRE(ed.SetNumber(EventNumber::MaxDistance, 50.0));
  r.Frame();
  const std::size_t u1 = r.Undos();
  SND_REQUIRE(TypeInto(r, "ev.f.n4", "1e999"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::MaxDistance), 50.0);
  SND_CHECK_EQ(r.Undos(), u1);
  r.Key(ui::kKeyEscape);
  SND_REQUIRE(TypeInto(r, "ev.f.n4", "75"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::MaxDistance), 75.0);
  SND_CHECK_EQ(r.Undos(), u1 + 1);
  SND_CHECK(r.AnyLabelContains("75 m"));
  // Priority 0..255: typed 300 is refused (or clamped into range): never 300.
  SND_REQUIRE(TypeInto(r, "ev.f.n1", "300"));
  SND_CHECK(ed.NumberOf(EventNumber::Priority) <= 255.0);
  r.Key(ui::kKeyEscape);
  // The screen shows what the model holds after an outside edit.
  SND_REQUIRE(ed.SetNumber(EventNumber::MaxDistance, 12.5));
  r.Frame();
  SND_CHECK(r.AnyLabelContains("12.5 m"));
  // Cone: inner > outer is refused by the model; the screen must not store it.
  SND_REQUIRE(ed.SetCone(90.0, 180.0, -6.0));
  r.Frame();
  SND_REQUIRE(TypeInto(r, "ev.f.cone.in", "200"));
  const json::Value* cone = FindPath(r.doc.Find(G(8))->doc, "cone");
  SND_REQUIRE(cone != nullptr);
  SND_CHECK(NumberAt(*cone, "innerDeg", 0) <= NumberAt(*cone, "outerDeg", 0));
  r.Key(ui::kKeyEscape);
}

// ============================================================================================
// Buses (D2)
// ============================================================================================

// D2: every bus is listed (over pages).
SND_TEST(IndepS7UiScreens, BusesListShowsEveryBus) {
  Rig r(LargeFiles(2, 100, 6, 4, 60));
  r.Show(ui::Screen::Buses);
  const std::set<std::string> listed = ListedOverPages(r, "mx.bus.", &r.app->view().buses.listPage);
  SND_CHECK(listed == KeysOf(r.doc, "bus"));
}

// D2: what the model refuses is refused from the screen: removing the master, removing a bus
// with children, a parent that makes a cycle, a send to the bus itself.
SND_TEST(IndepS7UiScreens, BusesScreenDoesNotPassRefusedEdits) {
  Rig r;
  r.Show(ui::Screen::Buses);
  MixerEditor mx(r.doc);
  // The master.
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(2))));
  std::size_t u0 = r.Undos();
  SND_REQUIRE(r.ClickId("mx.bus.remove"));
  SND_CHECK(r.doc.Find(G(2)) != nullptr);
  SND_CHECK_EQ(r.Undos(), u0);
  // A bus with a child.
  const std::string child = mx.AddBus("Child", G(3));
  SND_REQUIRE(!child.empty());
  r.Frame();
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  u0 = r.Undos();
  SND_REQUIRE(r.ClickId("mx.bus.remove"));
  SND_CHECK(r.doc.Find(G(3)) != nullptr);
  SND_CHECK_EQ(r.Undos(), u0);
  // Parent -> its own child: a cycle.
  SND_REQUIRE(r.ClickId("mx.bp.parent"));
  SND_REQUIRE(r.app->picker() != nullptr);
  std::string childValue, selfValue;
  for (const ui::PickerItem& it : r.app->picker()->items) {
    if (it.label.find("Child") != std::string::npos) childValue = it.value;
    if (it.label.find("Sfx") != std::string::npos) selfValue = it.value;
  }
  if (!childValue.empty()) {
    r.app->Pick(childValue);
    r.Frame();
  } else {
    r.app->ClosePicker();
  }
  SND_CHECK(StringAt(r.doc.Find(G(3))->doc, "parent") == "Master" || StringAt(r.doc.Find(G(3))->doc, "parent") == G(2));
  SND_CHECK_EQ(r.Undos(), u0);
  if (!selfValue.empty()) {
    r.ClickId("mx.bp.parent");
    if (r.app->picker() != nullptr) r.app->Pick(selfValue);
    r.Frame();
    SND_CHECK_EQ(r.Undos(), u0);
  }
  // A send to itself.
  SND_REQUIRE(r.ClickId("mx.send.add"));
  SND_REQUIRE(r.app->picker() != nullptr);
  std::string sfx;
  for (const ui::PickerItem& it : r.app->picker()->items) {
    if (it.label.find("Sfx") != std::string::npos) sfx = it.value;
  }
  if (!sfx.empty()) {
    r.app->Pick(sfx);
    r.Frame();
  } else {
    r.app->ClosePicker();
    r.Frame();
  }
  const json::Value* sends = FindPath(r.doc.Find(G(3))->doc, "sends");
  SND_REQUIRE(sends != nullptr);
  SND_CHECK_EQ(sends->items.size(), std::size_t{1});
  SND_CHECK_EQ(r.Undos(), u0);
}

// D2 + g: "+ Child" is one step, the new bus is listed and the mixer follows (a new strip).
SND_TEST(IndepS7UiScreens, BusesAddChildIsOneStepAndTheMixerFollows) {
  Rig r;
  r.Show(ui::Screen::Buses);
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  const std::size_t strips = r.s.mixer().buses().size();
  const std::size_t buses = r.doc.OfKind("bus").size();
  const std::size_t u0 = r.Undos();
  SND_REQUIRE(r.ClickId("mx.bus.add"));
  SND_CHECK_EQ(r.doc.OfKind("bus").size(), buses + 1);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  std::string added;
  for (const DocObject* o : r.doc.OfKind("bus")) {
    if (o->guid != G(2) && o->guid != G(3) && o->guid != G(4)) added = o->guid;
  }
  SND_REQUIRE(!added.empty());
  const std::string parent = StringAt(r.doc.Find(added)->doc, "parent");
  SND_CHECK(parent == G(3) || parent == "Sfx");
  r.Frame();
  SND_CHECK(r.El("mx.bus." + ui::ObjectKey(added)) != nullptr);
  SND_CHECK_EQ(r.s.mixer().buses().size(), strips + 1);
  r.Key('Z', ui::kModControl);
  r.Frame();
  SND_CHECK_EQ(r.doc.OfKind("bus").size(), buses);
  SND_CHECK_EQ(r.s.mixer().buses().size(), strips);
}

// D2: effect parameters typed on the screen are stored as the model normalises them (dsp range).
SND_TEST(IndepS7UiScreens, BusesEffectParamIsNormalisedByTheModel) {
  Rig r;
  r.Show(ui::Screen::Buses);
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  MixerEditor mx(r.doc);
  SND_REQUIRE(TypeInto(r, "mx.fx.0.p0", "99999999"));
  const double stored = mx.EffectParam(G(3), 0, 0);
  SND_CHECK_EQ(stored, MixerEditor::NormalizeEffectParam("snd.fx.lowpass", 0, 99999999.0));
  SND_CHECK(stored < 99999999.0);
  r.Key(ui::kKeyEscape);
}

// ============================================================================================
// Params (D3)
// ============================================================================================

// D3: the parameter's values in the fields; the composition result equals ComposeEvent.
SND_TEST(IndepS7UiScreens, ParamsFieldsAndCompositionFollowTheModel) {
  Rig r;
  r.app->Navigate(G(6));
  r.Frame();
  SND_REQUIRE(r.app->view().screen == ui::Screen::Params);
  ParameterEditor pe(r.doc, G(6));
  auto label = [&](const std::string& id) {
    const tela::PlacedElement* e = r.El(id);
    return e != nullptr ? e->element.label : std::string("<none>");
  };
  SND_CHECK(label("pc.par.min.value") == "0");
  SND_CHECK(label("pc.par.max.value") == "10");
  SND_CHECK(label("pc.par.def.value") == "1");
  SND_REQUIRE(pe.SetDefault(5.0));
  r.Frame();
  SND_CHECK(label("pc.par.def.value") == "5");
  if (r.app->view().params.event.empty()) r.app->view().params.event = G(8);
  r.Frame();
  const ComposeResult cr = ComposeEvent(r.doc, G(8), {}, r.app->view().params.distance);
  char want[64];
  std::snprintf(want, sizeof want, "pitch %+.1f ct", cr.pitchCents);
  SND_CHECK_MSG(r.AnyLabelContains(want), "no '%s' (ComposeEvent)", want);
  SND_CHECK_NEAR(cr.pitchCents, 600.0, 1e-3);
}

// D3 / F3: curve points: drag of a point by dy changes y by dy in the plot's scale (derived from
// the two points' places), never passes its neighbour, one step; Delete with two points left is
// refused.
SND_TEST(IndepS7UiScreens, ParamsCurvePointsDragAndDelete) {
  Rig r;
  r.app->Navigate(G(6));
  r.Frame();
  if (r.app->view().params.event.empty()) r.app->view().params.event = G(8);
  r.Frame();
  CurveEditor ce(r.doc, G(8), "curves[0]");
  SND_REQUIRE(ce.valid());
  const tela::PlacedElement* p0 = r.El("pc.pt.0");
  const tela::PlacedElement* p1 = r.El("pc.pt.1");
  SND_REQUIRE(p0 != nullptr && p1 != nullptr);
  const std::vector<CurvePoint> pts = ce.Points();
  const tela::Point c0 = uitest::Center(p0), c1 = uitest::Center(p1);
  const double unitsPerPxY = (pts[1].y - pts[0].y) / static_cast<double>(c0.y - c1.y);
  const double unitsPerPxX = (pts[1].x - pts[0].x) / static_cast<double>(c1.x - c0.x);
  SND_REQUIRE(std::isfinite(unitsPerPxY) && std::isfinite(unitsPerPxX));
  const std::size_t u0 = r.Undos();
  r.Drag(p1, 0.0f, 20.0f, 4);
  SND_CHECK_NEAR(ce.Points()[1].y, pts[1].y - 20.0 * unitsPerPxY, std::fabs(unitsPerPxY) * 1.01);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  // Past its neighbour: x stays at or before the next point.
  r.Drag(r.El("pc.pt.0"), 2000.0f, 0.0f, 4);
  const std::vector<CurvePoint> after = ce.Points();
  SND_CHECK_EQ(after.size(), std::size_t{2});
  SND_CHECK(after[0].x <= after[1].x);
  SND_CHECK_EQ(after[1].x, pts[1].x);
  // Delete with two points: refused.
  SND_REQUIRE(r.ClickId("pc.pt.0"));
  const std::size_t u1 = r.Undos();
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ce.Points().size(), std::size_t{2});
  SND_CHECK_EQ(r.Undos(), u1);
}

// ============================================================================================
// 3D (D4, F4)
// ============================================================================================

// D4: the values shown are SpatialPreview::Compute's; F4: an emitter drag lands where the view
// transform says; the circle of maxDistance has radius maxDistance / metersPerPixel.
SND_TEST(IndepS7UiScreens, SpatialValuesAndEmitterDrag) {
  Rig r;
  r.app->Navigate(G(8));
  r.Show(ui::Screen::Spatial);
  SpatialPreview* sp = r.app->spatial();
  SND_REQUIRE(sp != nullptr);
  char want[64];
  std::snprintf(want, sizeof want, "distance %.3f m", static_cast<double>(sp->Compute().distance));
  SND_CHECK_MSG(r.AnyLabelContains(want), "no '%s'", want);
  const tela::PlacedElement* view = r.El("sp.view");
  const tela::PlacedElement* em = r.El("sp.emitter");
  SND_REQUIRE(view != nullptr && em != nullptr);
  const tela::Point c = uitest::Center(em);
  // Where the emitter is drawn: WorldToScreen of its position.
  float px = 0, py = 0;
  sp->WorldToScreen(sp->emitter().position.x, sp->emitter().position.z, view->bounds.width, view->bounds.height, &px, &py);
  SND_CHECK_NEAR(view->bounds.x + px, c.x, 1.0);
  SND_CHECK_NEAR(view->bounds.y + py, c.y, 1.0);
  float wx = 0, wz = 0;
  sp->ScreenToWorld(px + 60.0f, py - 30.0f, view->bounds.width, view->bounds.height, &wx, &wz);
  r.Drag(em, 60.0f, -30.0f, 6);
  sp = r.app->spatial();
  SND_REQUIRE(sp != nullptr);
  SND_CHECK_NEAR(sp->emitter().position.x, wx, 0.02 * std::max(1.0f, std::fabs(wx)));
  SND_CHECK_NEAR(sp->emitter().position.z, wz, 0.02 * std::max(1.0f, std::fabs(wz)));
  std::snprintf(want, sizeof want, "distance %.3f m", static_cast<double>(sp->Compute().distance));
  SND_CHECK_MSG(r.AnyLabelContains(want), "after the drag: no '%s'", want);
  // The 3D view never edits the document.
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
}

// ============================================================================================
// Music (D5)
// ============================================================================================

std::map<std::string, std::string> MusicWithUnreachable(std::uint32_t rules) {
  return LargeFiles(1, 1, rules, 4, 12);
}

// D5: the rule table has one row per rule (over pages); unreachable rows are marked; Auto sort is
// refused while a row is unreachable (the rows keep their order).
SND_TEST(IndepS7UiScreens, MusicRuleTableRowsAndAutoSort) {
  Rig r(MusicWithUnreachable(150));
  r.app->Navigate(G(24));
  r.Frame();
  r.app->view().music.tab = 2;
  r.Frame();
  MusicEditor me(r.doc, G(24));
  const std::vector<TransitionRule> rules = me.Rules();
  SND_REQUIRE_EQ(rules.size(), std::size_t{150});
  std::set<int> rows;
  for (std::uint32_t p = 0; p < 200; ++p) {
    r.app->view().music.rulePage = p;
    r.Frame();
    std::size_t added = 0;
    for (const tela::PlacedElement* e : r.Prefixed("mu.rule.", ".from")) {
      const std::string mid = e->element.id.substr(8, e->element.id.size() - 8 - 5);
      added += rows.insert(std::atoi(mid.c_str())).second ? 1u : 0u;
    }
    if (added == 0 && p > 0) break;
  }
  SND_CHECK_EQ(rows.size(), rules.size());
  SND_REQUIRE(!me.Unreachable().empty());
  const std::size_t u0 = r.Undos();
  r.app->view().music.rulePage = 0;
  r.Frame();
  SND_REQUIRE(r.ClickId("mu.rule.sort"));
  const std::vector<TransitionRule> after = me.Rules();
  bool same = after.size() == rules.size();
  for (std::size_t i = 0; same && i < rules.size(); ++i) same = after[i].from == rules[i].from && after[i].to == rules[i].to;
  SND_CHECK(same);
  SND_CHECK_EQ(r.Undos(), u0);
}

// D5: the preview line shows MusicEditor::Preview's numbers.
SND_TEST(IndepS7UiScreens, MusicPreviewShowsTheModelsNumbers) {
  Rig r(Rich());
  r.app->Navigate(G(24));
  r.Frame();
  r.app->view().music.tab = 2;
  r.Frame();
  MusicEditor me(r.doc, G(24));
  const ui::MusicView& v = r.app->view().music;
  const TransitionPreview pv = me.Preview(v.from, v.to, v.requestSeconds, v.settings);
  SND_REQUIRE(pv.valid);
  char want[64];
  std::snprintf(want, sizeof want, "fires at %.3f s", pv.fireSeconds);
  SND_CHECK_MSG(r.AnyLabelContains(want), "no '%s'", want);
  const std::vector<LookaheadEntry> la = me.MinimumLookahead(v.settings);
  std::size_t shown = 0;
  for (const tela::PlacedElement* e : r.Prefixed("mu.la.")) {
    if (e->element.id != "mu.la.title") ++shown;
  }
  SND_CHECK_EQ(shown, la.size());
}

// ============================================================================================
// Material (D6)
// ============================================================================================

// D6: one row per material, the last material cannot be removed from the screen, a pair set on
// (a, b) shows on (b, a).
SND_TEST(IndepS7UiScreens, MaterialRowsLastMaterialAndSymmetricPairs) {
  std::map<std::string, std::string> f = Rich();
  Rig r(f);
  r.app->Navigate(G(25));
  r.Frame();
  MaterialEditor me(r.doc, G(25));
  SND_CHECK_EQ(r.Prefixed("ma.mat.", "").size() >= me.Materials().size(), true);
  for (std::size_t i = 0; i < me.Materials().size(); ++i) {
    SND_CHECK_MSG(r.El("ma.mat." + std::to_string(i)) != nullptr, "row %u", static_cast<unsigned>(i));
  }
  // Symmetric pairs: the table shows (0, 1) and (1, 0) the same.
  const tela::PlacedElement* a = r.El("ma.pair.0.1");
  const tela::PlacedElement* b = r.El("ma.pair.1.0");
  SND_REQUIRE(a != nullptr && b != nullptr);
  SND_CHECK(a->element.label == b->element.label);
  // Remove until one is left; the last one stays.
  for (int i = 0; i < 10; ++i) {
    r.ClickId("ma.mat.0");
    r.ClickId("ma.mat.remove");
  }
  SND_CHECK_EQ(me.Materials().size(), std::size_t{1});
  SND_CHECK(r.El("ma.mat.0") != nullptr);
}

// ============================================================================================
// Patch (D7)
// ============================================================================================

// D7: at the node limit "+ Node" adds nothing; a connection that makes a cycle is not made.
SND_TEST(IndepS7UiScreens, PatchLimitAndCycleAreRefused) {
  Rig r(Rich());
  r.app->Navigate(G(26));
  r.Frame();
  PatchEditor pe(r.doc, G(26));
  SND_REQUIRE_EQ(pe.Nodes().size(), std::size_t{32});
  const std::size_t u0 = r.Undos();
  if (r.ClickId("pa.add") && r.app->picker() != nullptr && !r.app->picker()->items.empty()) {
    const std::string v = r.app->picker()->items.front().value;  // a copy: see Defect_PickWithAnItemOfTheOpenList...
    r.app->Pick(v);
    r.Frame();
  }
  r.app->ClosePicker();
  r.Frame();
  SND_CHECK_EQ(pe.Nodes().size(), std::size_t{32});
  SND_CHECK_EQ(r.Undos(), u0);
  // Every node inside the view is drawn with its hit (the others are scrolled away).
  const tela::PlacedElement* view = r.El("pa.view");
  SND_REQUIRE(view != nullptr);
  for (const PatchNode& n : pe.Nodes()) {
    if (n.x + 120.0f < view->bounds.width && n.y + 80.0f < view->bounds.height)
      SND_CHECK_MSG(r.El("pa.node." + n.id) != nullptr, "node %s at %.0f %.0f", n.id.c_str(), n.x, n.y);
  }
  // A cycle: two filters f1 -> f2 (audio); dragging f2's output to f1's input must be refused.
  PatchEditor& m = pe;
  SND_REQUIRE(m.RemoveNode("osc29"));
  SND_REQUIRE(m.RemoveNode("osc30"));
  SND_REQUIRE(m.AddNode("filter", 300.0f, 400.0f, "f1") == "f1");
  SND_REQUIRE(m.AddNode("filter", 500.0f, 400.0f, "f2") == "f2");
  SND_REQUIRE(m.Connect("f1", "f2", "in"));
  r.Frame();
  const std::size_t u1 = r.Undos();
  const tela::PlacedElement* from = r.El("pa.node.f2.out");
  const tela::PlacedElement* to = r.El("pa.node.f1.in.in");
  SND_REQUIRE(from != nullptr && to != nullptr);
  const tela::Point a = uitest::Center(from), b = uitest::Center(to);
  r.DragFrom(a.x, a.y, b.x - a.x, b.y - a.y, 6);
  SND_CHECK_EQ(r.Undos(), u1);
  SND_CHECK(!r.doc.InGroup());
  for (const PatchConnection& c : m.Connections()) SND_CHECK(!(c.from == "f2" && c.to == "f1"));
}

// D7 / F5: a node drag moves the node by the motion (one step).
SND_TEST(IndepS7UiScreens, PatchNodeDragMovesByTheMotion) {
  Rig r(Rich());
  r.app->Navigate(G(26));
  r.Frame();
  PatchEditor pe(r.doc, G(26));
  auto pos = [&](const std::string& id) {
    for (const PatchNode& n : pe.Nodes()) {
      if (n.id == id) return tela::Point{n.x, n.y};
    }
    return tela::Point{-1e9f, -1e9f};
  };
  const tela::Point before = pos("osc0");
  const std::size_t u0 = r.Undos();
  r.Drag(r.El("pa.node.osc0"), 50.0f, 40.0f, 5);
  const tela::Point after = pos("osc0");
  SND_CHECK_NEAR(after.x - before.x, 50.0f, 1.0f);
  SND_CHECK_NEAR(after.y - before.y, 40.0f, 1.0f);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
}

// ============================================================================================
// Problems (D9), Profiler / Replay / Link (D8)
// ============================================================================================

// D9: one row per CheckDocument issue (errors first); Go shows the object's screen.
SND_TEST(IndepS7UiScreens, ProblemsRowsAreTheBuildsIssues) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  // An error: a clip naming no asset.
  f["events/Broken.json"] = "{\"type\": \"event\", \"guid\": \"" + G(40) +
                            "\", \"name\": \"Broken\", \"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"nope\"}]}]}\n";
  Rig r(f);
  r.app->RefreshProblems();
  r.Show(ui::Screen::Problems);
  const std::vector<DocIssue> issues = CheckDocument(r.doc);
  std::size_t errors = 0;
  for (const DocIssue& i : issues) errors += i.error ? 1u : 0u;
  SND_REQUIRE(errors > 0);
  std::size_t rows = 0, errorRows = 0;
  for (const tela::PlacedElement* e : r.Prefixed("prob.")) {
    const std::string rest = e->element.id.substr(5);
    if (rest.empty() || !std::all_of(rest.begin(), rest.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) continue;
    if (e->element.label.rfind("error", 0) == 0 || e->element.label.rfind("warning", 0) == 0) ++rows;
    if (e->element.label.rfind("error", 0) == 0) ++errorRows;
  }
  SND_CHECK_EQ(rows, issues.size());
  SND_CHECK_EQ(errorRows, errors);
  // Errors first.
  for (std::size_t i = 0; i < errors; ++i) {
    const tela::PlacedElement* e = r.El("prob." + std::to_string(i));
    SND_CHECK(e != nullptr && e->element.label.rfind("error", 0) == 0);
  }
  const tela::PlacedElement* first = r.El("prob.0");
  SND_REQUIRE(first != nullptr);
  SND_CHECK(first->element.label.rfind("error", 0) == 0);
  // Go on the error row: the event screen with Broken selected.
  SND_REQUIRE(r.ClickId("prob.0.go"));
  SND_CHECK(r.app->view().screen == ui::Screen::Event);
  SND_CHECK(r.app->view().event.event == G(40));
}

// D8: the live screens without a connection, a recording or a replay service: no crash, a reason.
SND_TEST(IndepS7UiScreens, LiveScreensWithoutAGameSayWhy) {
  Rig r;
  r.Show(ui::Screen::Replay);
  SND_CHECK(r.AnyLabelContains("no replay service"));
  for (std::uint32_t tab = 0; tab < 6; ++tab) {
    r.app->view().profiler.tab = tab;
    r.Show(ui::Screen::Profiler);
    SND_CHECK(FirstDuplicate(r.rt()).empty());
  }
  for (std::uint32_t tab = 0; tab < 3; ++tab) {
    r.app->view().link.tab = tab;
    r.Show(ui::Screen::Link);
    SND_CHECK(FirstDuplicate(r.rt()).empty());
  }
  // Pressing every button of the replay screen without a service does not crash.
  r.app->view().link.tab = 0;
  r.Show(ui::Screen::Replay);
  for (const char* id : {"rp.twice", "rp.wav", "rp.listen", "rp.report.write", "rp.report.open", "rp.save", "rp.load"}) {
    r.ClickId(id);
    r.app->text().Cancel();
    r.app->ClosePicker();
  }
  SND_CHECK(FirstDuplicate(r.rt()).empty());
}
