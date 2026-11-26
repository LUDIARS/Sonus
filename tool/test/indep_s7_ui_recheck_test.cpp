// Independent tests of group U, re-check after the decisions D-186..D-189 (indep_s7_ui_properties.md
// L): keys while typing, Esc everywhere, typed numbers on every number field, wheel and button
// series, the vertical clip drag, the room of number fields at every window and DPI, ids made from
// names, the mixer rebind question, the cone, the Link screen's failures and not-connected text,
// problems once.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <set>

#include "indep_s7_ui_fixture.hpp"
#include "live_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/material_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/music_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/preview_engine.hpp"
#include "snd/tool/ui/kit.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "snd/tool/ui/services.hpp"

using namespace iu;

namespace {

std::map<std::string, std::string> Rich() { return LargeFiles(3, 2, 6, 4, 12); }

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool EndsWith(const std::string& s, const std::string& e) {
  return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// The number fields of the declared tree: the hit `<id>` (an empty button) with its `<id>.value`.
std::vector<std::string> NumberFields(const Rig& r) {
  std::vector<std::string> v;
  for (const tela::PlacedElement& p : r.rt().elements()) {
    const std::string& id = p.element.id;
    if (!EndsWith(id, ".value")) continue;
    const std::string base = id.substr(0, id.size() - 6);
    const tela::PlacedElement* hit = r.El(base);
    if (hit != nullptr && hit->element.kind == tela::ElementKind::button && hit->element.label.empty()) v.push_back(base);
  }
  return v;
}

// The selections that show every kind of number field of the rich project.
struct View {
  const char* name;
  void (*select)(Rig&);
};
void SelEvent(Rig& r) { r.app->Navigate(G(8)); r.Frame(); r.ClickId("ev.t0.c0"); }
void SelBus(Rig& r) { r.app->Navigate(G(3)); }
void SelSnapshots(Rig& r) { r.app->Navigate(G(9)); }
void SelDucking(Rig& r) { r.app->Navigate(G(3)); r.app->view().buses.tab = 2; }
void SelCategories(Rig& r) { r.app->Navigate(G(5)); }
void SelParams(Rig& r) { r.app->Navigate(G(6)); r.Frame(); if (r.app->view().params.event.empty()) r.app->view().params.event = G(8); r.Frame(); r.ClickId("pc.pt.0"); }
void SelSpatial(Rig& r) { r.app->Navigate(G(8)); r.app->OnScreen(ui::Screen::Spatial); }
void SelSegment(Rig& r) { r.app->Navigate(G(22)); }
void SelPlaylist(Rig& r) { r.app->Navigate(G(23)); }
void SelRules(Rig& r) { r.app->Navigate(G(24)); }
void SelStingers(Rig& r) { r.app->Navigate(G(24)); r.app->view().music.tab = 3; }
void SelMaterial(Rig& r) { r.app->Navigate(G(25)); }
void SelPatch(Rig& r) { r.app->Navigate(G(26)); r.Frame(); r.ClickId("pa.node.osc0"); }
void SelLink(Rig& r) { r.app->OnScreen(ui::Screen::Link); }
const View kViews[] = {{"event", SelEvent},       {"bus", SelBus},         {"snapshots", SelSnapshots},
                       {"ducking", SelDucking},   {"categories", SelCategories}, {"params", SelParams},
                       {"spatial", SelSpatial},   {"segment", SelSegment}, {"playlist", SelPlaylist},
                       {"rules", SelRules},       {"stingers", SelStingers}, {"material", SelMaterial},
                       {"patch", SelPatch},       {"link", SelLink}};

bool TypeInto(Rig& r, const std::string& id, const std::string& text) {
  if (!r.ClickId(id) || !r.app->text().active()) return false;
  for (int i = 0; i < 300; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  r.Type(text);
  r.Key(ui::kKeyEnter);
  return true;
}

}  // namespace

// ============================================================================================
// L1 (D-186): while an entry is open every key is the entry's.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, EveryKeyWhileTypingIsTheEntrys) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(r.doc.SetNumber(G(3), "volumeDb", -7.0));  // a step to undo
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c0"));                     // a selection Delete would remove
  SND_REQUIRE(r.ClickId("ev.list.name"));
  SND_REQUIRE(r.app->text().active());
  std::vector<ui::KeyEvent> keys;
  for (std::uint32_t mods : {0u, ui::kModControl, ui::kModShift, ui::kModControl | ui::kModShift}) {
    for (std::uint32_t k : {std::uint32_t{'Z'}, std::uint32_t{'Y'}, std::uint32_t{'S'}, std::uint32_t{'A'},
                            ui::kKeyDelete, ui::kKeyTab, ui::kKeyLeft, ui::kKeyRight, ui::kKeyUp, ui::kKeyDown,
                            ui::kKeyHome, ui::kKeyEnd, ui::kKeyPageUp, ui::kKeyPageDown}) {
      keys.push_back(ui::KeyEvent{k, 0, mods});
    }
    for (int f = 1; f <= 12; ++f) keys.push_back(ui::KeyEvent{ui::kKeyF1 + static_cast<std::uint32_t>(f) - 1u, 0, mods});
  }
  const std::uint64_t rev = r.doc.revision();
  for (const ui::KeyEvent& k : keys) {
    const bool used = r.app->Key(k);
    r.Frame();
    SND_CHECK_MSG(used, "key %x mods %u not used", k.key, k.modifiers);
    SND_CHECK_MSG(r.app->text().active(), "key %x mods %u closed the entry", k.key, k.modifiers);
    SND_CHECK_MSG(r.app->view().screen == ui::Screen::Event, "key %x mods %u changed the screen", k.key, k.modifiers);
    if (!r.app->text().active()) SND_REQUIRE(r.ClickId("ev.list.name"));
  }
  SND_CHECK_EQ(r.doc.revision(), rev);
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
  SND_CHECK(!r.doc.IsDirty() || r.doc.UndoCount() == 1);
  // Backspace edits the text; Esc closes without commit.
  r.Key(ui::kKeyBackspace);
  r.Key(ui::kKeyEscape);
  SND_CHECK(!r.app->text().active());
  SND_CHECK(r.doc.Find(G(8))->name() == "Shot");
}

// ============================================================================================
// L2 (D-186): Esc - an entry, a list, nothing, a pending connection, drags (one step at most back,
// no new step, no redo step).
// ============================================================================================
SND_TEST(IndepS7UiRecheck, EscClosesTheEntryAndTheListAndIsAlwaysTheTools) {
  Rig r(Rich());
  r.app->Navigate(G(3));
  r.Frame();
  int commits = 0, picks = 0;
  r.app->BeginText("indep.f", "x", [&](const std::string&) { ++commits; return true; }, false);
  SND_CHECK(r.Key(ui::kKeyEscape));
  SND_CHECK(!r.app->text().active());
  SND_CHECK_EQ(commits, 0);
  ui::Picker p;
  p.field = "indep.p";
  p.items = {ui::PickerItem{"a", "a"}};
  p.pick = [&](const std::string&) { ++picks; };
  r.app->OpenPicker(std::move(p));
  SND_CHECK(r.Key(ui::kKeyEscape));
  SND_CHECK(r.app->picker() == nullptr);
  SND_CHECK_EQ(picks, 0);
  // Nothing open: the tool takes Esc (the window must not get it) and nothing changes.
  for (ui::Screen sc : kAllScreens) {
    r.Show(sc);
    const std::uint64_t rev = r.doc.revision();
    SND_CHECK_MSG(r.Key(ui::kKeyEscape), "Esc on %s went to the window", ui::ScreenName(sc));
    SND_CHECK(r.app->view().screen == sc);
    SND_CHECK_EQ(r.doc.revision(), rev);
  }
  // A pending patch connection.
  r.app->Navigate(G(26));
  r.Frame();
  SND_REQUIRE(r.ClickId("pa.node.osc0.out"));  // (osc1's port lies under osc27 in the generated layout)
  SND_REQUIRE(!r.app->view().patch.pendingFrom.empty());
  SND_CHECK(r.Key(ui::kKeyEscape));
  SND_CHECK(r.app->view().patch.pendingFrom.empty());
}

namespace {
// A clip drag that Esc takes back: returns after Esc (the button still held), then releases.
void ClipDragEsc(Rig& r, float dx) {
  const tela::Point c = uitest::Center(r.El("ev.t0.c0"));
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x + dx * 0.5f, c.y);
  r.Frame();
  r.ptr->Move(c.x + dx, c.y);
  r.Frame();
  r.Key(ui::kKeyEscape);
  r.ptr->Move(c.x + dx * 2.0f, c.y);
  r.Frame();
  r.ptr->Up(c.x + dx * 2.0f, c.y);
  r.ptr->Advance(1000);
  r.Frame();
}
}  // namespace

// Esc during a clip drag: the start is back, the history is as before the press (no step, no redo
// step, the earlier redo step kept), the group closed, the release does nothing more.
SND_TEST(IndepS7UiRecheck, EscDuringAClipDragLeavesTheHistoryAsBefore) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 9.0));
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 10.0));
  SND_REQUIRE(r.doc.Undo());  // undo 1, redo 1
  r.Frame();
  SND_REQUIRE(ed.MoveClip(0, 0, 100.0));  // undo 2 (redo gone: a new step)
  r.Frame();
  const std::size_t u = r.doc.UndoCount(), rd = r.doc.RedoCount();
  ClipDragEsc(r, 40.0f);
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 100.0);
  SND_CHECK(!r.doc.InGroup());
  SND_CHECK_EQ(r.doc.UndoCount(), u);
  (void)rd;  // the redo side is Defect_EscOnADragLeavesItAsARedoStep
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 9.0);
}

// Defect hunt: Esc during a drag that changed nothing (dragged against the left end: the start stays
// 0) must not take back the earlier, unrelated step. Esc takes back "the drag's step" by ending the
// group and calling Undo; an empty group files no step, so Undo removes the step before it.
SND_TEST(IndepS7UiRecheck, Defect_EscOnADragThatChangedNothingUndoesTheEditBefore) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 9.0));
  r.Frame();
  ClipDragEsc(r, -60.0f);  // start 0: stays 0
  SND_CHECK_MSG(ed.NumberOf(EventNumber::Priority) == 9.0, "the earlier edit was undone (priority %g)",
                ed.NumberOf(EventNumber::Priority));
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
  SND_CHECK_EQ(r.doc.RedoCount(), std::size_t{0});
}

// Defect hunt: Esc during a drag leaves the drag as a redo step (Esc = Undo), and the redo step that
// was there before the drag is lost. "Taken back" should leave no trace in the history.
SND_TEST(IndepS7UiRecheck, Defect_EscOnADragLeavesItAsARedoStep) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 9.0));
  SND_REQUIRE(r.doc.Undo());  // redo 1: priority 9
  r.Frame();
  ClipDragEsc(r, 40.0f);
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
  SND_CHECK_MSG(r.doc.RedoCount() == 1u, "redo steps %u", static_cast<unsigned>(r.doc.RedoCount()));
  if (r.doc.Redo()) {
    SND_CHECK_MSG(ed.NumberOf(EventNumber::Priority) == 9.0 && ed.ClipNumberOf(0, 0, ClipNumber::StartMs) == 0.0,
                  "Redo after Esc gave priority %g start %g (expected the old redo step: 9, 0)",
                  ed.NumberOf(EventNumber::Priority), ed.ClipNumberOf(0, 0, ClipNumber::StartMs));
  }
}

// Esc during a number field drag and during a mixer fader drag: values back.
SND_TEST(IndepS7UiRecheck, EscDuringNumberAndFaderDrags) {
  Rig r;
  r.app->Navigate(G(3));
  r.Frame();
  auto vol = [&] { return NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0); };
  const double v0 = vol();
  const std::size_t u0 = r.doc.UndoCount();
  tela::Point c = uitest::Center(r.El("mx.bp.vol"));
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x, c.y - 40.0f);
  r.Frame();
  SND_CHECK(vol() != v0);
  r.Key(ui::kKeyEscape);
  r.ptr->Up(c.x, c.y - 40.0f);
  r.Frame();
  SND_CHECK_EQ(vol(), v0);
  SND_CHECK_EQ(r.doc.UndoCount(), u0);
  SND_CHECK(!r.doc.InGroup());
  // The mixer.
  r.Show(ui::Screen::Mixer);
  const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
  const Target t{Control::BusFader, sfx, 0, 0};
  const float f0 = r.s.mixer().Get(t);
  const std::size_t mu = r.s.mixer().history().UndoCount();
  c = uitest::Center(r.El(ui::BusId(snd::HashId("Sfx")) + ".fader.hit"));
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x, c.y - 60.0f);
  r.Frame();
  SND_CHECK(r.s.mixer().Get(t) != f0);
  r.Key(ui::kKeyEscape);
  r.ptr->Up(c.x, c.y - 60.0f);
  r.Frame();
  SND_CHECK_EQ(r.s.mixer().Get(t), f0);
  SND_CHECK_EQ(r.s.mixer().history().UndoCount(), mu);
}

// ============================================================================================
// L3 (D-187): every number field of every screen: text that is no finite number (empty, "-", ".",
// letters only, 1e999) is refused - the document does not change and the entry stays open; huge
// values either go in unchanged-or-refused consistently (changed <=> entry closed).
// ============================================================================================
SND_TEST(IndepS7UiRecheck, TypedNumbersOnEveryFieldGoToTheModelUnchanged) {
  std::string bad;
  std::size_t fields = 0;
  for (const View& v : kViews) {
    Rig r(Rich());
    v.select(r);
    r.Frame();
    const std::vector<std::string> ids = NumberFields(r);
    for (const std::string& id : ids) {
      if (r.El(id) == nullptr) continue;  // went away with an earlier edit
      ++fields;
      for (const char* text : {"", "-", ".", "nan", "e", "1e999", "-1e999", "1e", "--1"}) {
        const std::uint64_t rev = r.doc.revision();
        if (!TypeInto(r, id, text)) {
          bad += std::string(" ") + v.name + ":" + id + "(no entry)";
          break;
        }
        if (r.doc.revision() != rev || !r.app->text().active())
          bad += std::string(" ") + v.name + ":" + id + "['" + text + "' taken]";
        if (r.app->text().active()) r.Key(ui::kKeyEscape);
        if (r.doc.revision() != rev) {
          r.doc.Undo();
          r.Frame();
        }
      }
    }
  }
  std::printf("number fields typed into: %u\n", static_cast<unsigned>(fields));
  SND_CHECK(fields > 60u);
  SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
}

// D-187 "打った数値は切り詰めずにそのままモデルに渡す": the integer fields convert the typed number with
// static_cast to an unsigned integer before the model's setter (buses_view.cpp, music_view.cpp,
// event_view.cpp, link_view.cpp, profiler_view.cpp): 2.5 becomes 2 and is written, -1, 1e300 and
// 4294967296 are out of the integer's range (undefined behaviour; here they become a value the
// model takes or the current one, so the entry closes as if it had been taken). The port 70000 of
// the Link screen becomes 4464. Expected: refused, the document (and the port) unchanged, the entry
// open.
SND_TEST(IndepS7UiRecheck, Defect_TypedNumberIsCutToAnIntegerBeforeTheModel) {
  std::string bad;
  struct F {
    void (*select)(Rig&);
    const char* id;
  };
  for (const F& f : {F{SelBus, "mx.bp.ch"}, F{SelBus, "mx.bp.lod"}, F{SelSegment, "mu.seg.bars"},
                     F{SelSegment, "mu.seg.entry"}, F{SelPlaylist, "mu.pl.loops"}}) {
    for (const char* text : {"2.5", "-1", "1e300", "4294967296", "1.5e19"}) {
      Rig r(Rich());
      f.select(r);
      r.Frame();
      if (r.El(f.id) == nullptr) {
        bad += std::string(" ") + f.id + "(missing)";
        break;
      }
      const std::uint64_t rev = r.doc.revision();
      SND_REQUIRE(TypeInto(r, f.id, text));
      if (r.doc.revision() != rev || !r.app->text().active()) {
        bad += std::string(" ") + f.id + "['" + text + "': " + (r.doc.revision() != rev ? "written" : "closed") + "]";
      }
    }
  }
  Rig r;
  r.Show(ui::Screen::Link);
  const std::uint16_t port = r.app->connectionDesc().port;
  SND_REQUIRE(TypeInto(r, "ln.port", "70000"));
  if (r.app->connectionDesc().port != port) {
    bad += " ln.port['70000' -> " + std::to_string(r.app->connectionDesc().port) + "]";
  }
  SND_CHECK_MSG(bad.empty(), "%s", bad.c_str());
}

// The 3D screen's position fields take any finite number and SpatialPreview computes in float:
// 1e30 or 1e300 (finite doubles, accepted) give "distance inf m" and "azimuth -nan deg" on the screen
// (non-finite values shown as numbers). Expected: refused (out of float) or finite results. It stays
// within Tela's limits, no non-finite text.
SND_TEST(IndepS7UiRecheck, Defect_HugeTypedPositionsOnThe3DScreenShowInfAndNan) {
  for (const char* id : {"sp.l.x", "sp.l.z", "sp.e.x", "sp.e.z", "sp.l.yaw", "sp.e.yaw"}) {
    for (const char* text : {"1e300", "-1e300", "1e30"}) {
      Rig r;
      SelSpatial(r);
      r.Frame();
      SND_REQUIRE(TypeInto(r, id, text));
      if (r.app->text().active()) r.Key(ui::kKeyEscape);
      r.Frame();
      SND_CHECK_MSG(LimitProblem(r.rt()).empty(), "%s=%s: %s", id, text, LimitProblem(r.rt()).c_str());
      SND_CHECK(FirstDuplicate(r.rt()).empty());
      for (const tela::PlacedElement& e : r.rt().elements()) {
        const std::string& l = e.element.label;
        SND_CHECK_MSG(l.find("nan") == std::string::npos && l.find("inf") == std::string::npos, "%s=%s: %s: %s", id, text,
                      e.element.id.c_str(), l.c_str());
      }
      tela::PixelSurface px;
      ui::Compose(r.rt(), ui::ComposeOptions{}, &px);
    }
  }
}

// ============================================================================================
// L4 (D-187): n wheel notches / n button clicks without a declaration in between are n steps, stop at
// the field's range end, one undo step per wheel series.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, WheelAndButtonSeriesCountEveryStepAndStopAtTheEnd) {
  Rig r;
  r.app->Navigate(G(3));
  r.Frame();
  auto num = [&](const char* m, double d) { return NumberAt(r.doc.Find(G(3))->doc, m, d); };
  const double v0 = num("volumeDb", 0.0);
  SND_REQUIRE(r.ClickId("mx.bp.vol.inc"));
  const double step = num("volumeDb", 0.0) - v0;
  SND_REQUIRE(step > 0.0);
  SND_REQUIRE(r.doc.Undo());
  r.Frame();
  // Series on the same field merge into one step (the router's wheel key changes only when the
  // pointer goes to another control): all three series below are one step.
  const std::size_t wheelU = r.doc.UndoCount();
  for (int n : {1, 3, 7}) {
    const double a = num("volumeDb", 0.0);
    const tela::Point c = uitest::Center(r.El("mx.bp.vol"));
    for (int i = 0; i < n; ++i) r.ptr->Wheel(c.x, c.y, -1.0f);
    r.Frame();
    SND_CHECK_NEAR(num("volumeDb", 0.0), a - n * step, 1e-9);
    SND_CHECK_EQ(r.doc.UndoCount(), wheelU + 1);
    r.ptr->Advance(1000);
  }
  // Buttons without a declaration between the clicks.
  const double b = num("volumeDb", 0.0);
  const tela::Point inc = uitest::Center(r.El("mx.bp.vol.inc"));
  for (int i = 0; i < 4; ++i) r.ptr->Click(inc.x, inc.y);
  r.Frame();
  SND_CHECK_NEAR(num("volumeDb", 0.0), b + 4 * step, 1e-9);
  // Channels: 0..8, an integer; 20 clicks of "+" stop at 8, 20 of "-" at 0.
  const tela::Point chi = uitest::Center(r.El("mx.bp.ch.inc"));
  for (int i = 0; i < 20; ++i) r.ptr->Click(chi.x, chi.y);
  r.Frame();
  SND_CHECK_EQ(num("channels", 0.0), 8.0);
  const tela::Point chd = uitest::Center(r.El("mx.bp.ch.dec"));
  for (int i = 0; i < 20; ++i) r.ptr->Click(chd.x, chd.y);
  r.Frame();
  SND_CHECK_EQ(num("channels", 0.0), 0.0);
  // The wheel at the end: no step beyond it.
  const tela::Point chv = uitest::Center(r.El("mx.bp.ch"));
  const std::size_t u = r.doc.UndoCount();
  for (int i = 0; i < 5; ++i) r.ptr->Wheel(chv.x, chv.y, -1.0f);
  r.Frame();
  SND_CHECK_EQ(num("channels", 0.0), 0.0);
  SND_CHECK_EQ(r.doc.UndoCount(), u);
}

// ============================================================================================
// L5 (D-187): the vertical clip drag: track = clamp(t + round(dy / 48)) wherever the clip is pressed.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, VerticalClipDragFollowsTheMotionOnly) {
  for (float at : {0.1f, 0.5f, 0.9f}) {
    for (float dy : {-23.0f, 23.0f, -25.0f, 25.0f, 71.0f, -73.0f, 300.0f, -300.0f}) {
      Rig r;
      r.app->Navigate(G(8));
      r.Frame();
      EventEditor ed(r.doc, G(8));
      SND_REQUIRE(ed.AddTrack(0));
      SND_REQUIRE(ed.AddTrack(2));
      r.Frame();
      const tela::PlacedElement* c = r.El("ev.t1.c0");
      SND_REQUIRE(c != nullptr);
      const std::size_t u0 = r.doc.UndoCount();
      r.DragFrom(c->bounds.x + c->bounds.width * 0.5f, c->bounds.y + c->bounds.height * at, 0.0f, dy, 3);
      const int want = std::clamp(1 + static_cast<int>(std::lround(dy / 48.0f)), 0, 2);
      int got = -1;
      for (std::uint32_t t = 0; t < 3; ++t) {
        if (ed.ClipCount(t) == 1u) got = static_cast<int>(t);
      }
      SND_CHECK_MSG(got == want, "pressed at %.1f of the clip, dy %.0f: track %d, expected %d", static_cast<double>(at),
                    static_cast<double>(dy), got, want);
      SND_CHECK(r.doc.UndoCount() <= u0 + 1u);
    }
  }
}

// ============================================================================================
// L6 (D-187): number fields have room at every window and DPI; their parts do not overlap and stay in
// the window.
// ============================================================================================
namespace {
struct Win {
  float lw, lh;  // logical size
};
// Number fields at every DPI of `dpis` and window of `wins`: their value narrower than
// kMinNumberValue, outside the window (`outside`), overlapping their own parts. "" if none.
std::string RoomProblems(std::initializer_list<float> dpis, std::initializer_list<Win> wins, bool outside) {
  std::map<std::string, std::array<unsigned, 3>> combos;  // "<w>x<h>/<dpi>" -> narrow, outside, overlap
  std::string examples;
  std::size_t checked = 0;
  for (float dpi : dpis) {
    for (Win w : wins) {
      if (w.lw * dpi > 8000.0f) continue;
      char combo[48];
      std::snprintf(combo, sizeof combo, "%gx%g/%g", static_cast<double>(w.lw), static_cast<double>(w.lh), static_cast<double>(dpi));
      std::array<unsigned, 3>& n = combos[combo];
      for (const View& v : kViews) {
        Rig r(Rich());
        r.app->SetViewport(static_cast<int>(w.lw * dpi), static_cast<int>(w.lh * dpi), dpi);
        v.select(r);
        r.Frame();
        for (const std::string& id : NumberFields(r)) {
          ++checked;
          const tela::PlacedElement* val = r.El(id + ".value");
          std::string what;
          if (val->bounds.width + 0.01f < ui::kMinNumberValue) {
            ++n[0];
            what = "narrow";
          }
          if (outside && (val->bounds.x + val->bounds.width > w.lw + 0.5f || val->bounds.x < -0.5f ||
              val->bounds.y + val->bounds.height > w.lh + 0.5f)) {
            ++n[1];
            what += " outside";
          }
          for (const char* part : {".dec", ".inc", ".label"}) {
            const tela::PlacedElement* o = r.El(id + part);
            if (o == nullptr) continue;
            const tela::Rect a = val->bounds, q = o->bounds;
            const float ox = std::min(a.x + a.width, q.x + q.width) - std::max(a.x, q.x);
            const float oy = std::min(a.y + a.height, q.y + q.height) - std::max(a.y, q.y);
            if (ox > 0.5f && oy > 0.5f) {
              ++n[2];
              what += std::string(" overlaps ") + part;
            }
          }
          if (!what.empty() && examples.size() < 1500) {
            char b[200];
            std::snprintf(b, sizeof b, "\n  %s %s %s: %s (x %.0f w %.0f y %.0f)", combo, v.name, id.c_str(), what.c_str(),
                          static_cast<double>(val->bounds.x), static_cast<double>(val->bounds.width),
                          static_cast<double>(val->bounds.y));
            examples += b;
          }
        }
      }
    }
  }
  std::string summary;
  for (const auto& [c, n] : combos) {
    if (n[0] + n[1] + n[2] == 0u) continue;
    char b[120];
    std::snprintf(b, sizeof b, "\n  %s: narrow %u outside %u overlap %u", c.c_str(), n[0], n[1], n[2]);
    summary += b;
  }
  std::printf("number field values checked: %u\n", static_cast<unsigned>(checked));
  return summary.empty() ? std::string() : summary + examples;
}
}  // namespace

// D-187: every value at least kMinNumberValue wide, never over its own buttons or label, at every
// window and DPI; inside the window from 1280 x 800 up.
SND_TEST(IndepS7UiRecheck, NumberFieldsHaveRoomEverywhere) {
  const std::string narrow = RoomProblems({0.25f, 1.0f, 2.0f, 4.0f},
                                          {Win{320, 240}, Win{640, 400}, Win{1280, 800}, Win{1920, 1080}}, false);
  SND_CHECK_MSG(narrow.empty(), "%s", narrow.c_str());
  const std::string out = RoomProblems({0.25f, 1.0f, 2.0f, 4.0f}, {Win{1280, 800}, Win{1920, 1080}}, true);
  SND_CHECK_MSG(out.empty(), "%s", out.c_str());
}

// D-187 "入りきらない行は折り返す・段を分ける・ページを送る" at the smallest windows the tool takes
// (App declares at least 320 x 240 logical; 640 x 400 is the size the limits tests used before):
// number fields are declared outside the window (Event's fields, effects, segment, playlist,
// rules, material ...), where they can be neither seen nor pressed.
SND_TEST(IndepS7UiRecheck, Defect_NumberFieldsOutsideTheWindowAtSmallWindows) {
  const std::string out = RoomProblems({1.0f}, {Win{320, 240}, Win{640, 400}}, true);
  SND_CHECK_MSG(out.empty(), "%s", out.c_str());
}


// ============================================================================================
// L7 (D-189): ids made from names: no duplicate on any screen with names of symbols, non-ASCII,
// long names, a name used by objects of different kinds and by items of different kinds.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, NameMadeIdsAreUniqueWithAwkwardNames) {
  std::map<std::string, std::string> f = Rich();
  const std::string longName = LongName("X", 220);
  const char* const names[] = {"X", "x", "a.b", "a b", "../", "\xE6\x97\xA5\xE6\x9C\xAC", "\"q\"", "mat0"};
  // Materials, states, markers and stingers with the same awkward names.
  std::string mats, states, markers;
  int i = 0;
  for (const char* n : names) {
    std::string e = n;
    for (std::size_t k = 0; k < e.size(); ++k) {
      if (e[k] == '"') e.replace(k++, 1, "\\\"");
    }
    mats += std::string(i ? ", " : "") + "\"" + e + "\"";
    states += std::string(i ? ", " : "") + "{\"name\": \"" + e + "\", \"playlist\": \"" + G(23) + "\"}";
    markers += std::string(i ? ", " : "") + "{\"name\": \"" + e + "\", \"frame\": " + std::to_string(100 + 10 * i) + "}";
    ++i;
  }
  mats += ", \"" + longName + "\"";
  f["materials/Phys.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + G(25) +
                             "\", \"name\": \"X\", \"bank\": \"Main\", \"materials\": [" + mats + "]}\n";
  f["segments/Seg.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"X\", \"asset\": \"" + G(7) +
                           "\", \"bars\": 1, \"markers\": [" + markers + "]}\n";
  f["events/Music.json"] = "{\"type\": \"event\", \"guid\": \"" + G(24) + "\", \"name\": \"Music\", \"bank\": \"Main\", "
                           "\"music\": {\"initialState\": \"X\", \"states\": [" + states + "], \"stingers\": ["
                           "{\"name\": \"X\", \"event\": \"" + G(8) + "\"}, {\"name\": \"x\", \"event\": \"" + G(8) + "\"}]}}\n";
  f["parameters/X.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(41) + "\", \"name\": \"X\", \"min\": 0, \"max\": 1}\n";
  f["categories/X.json"] = "{\"type\": \"category\", \"guid\": \"" + G(42) + "\", \"name\": \"X\"}\n";
  Rig r(f);
  for (const char* sel : {"d0c00000-0000-4000-8000-000000000019", "d0c00000-0000-4000-8000-000000000016",
                          "d0c00000-0000-4000-8000-000000000018", "d0c00000-0000-4000-8000-000000000029"}) {
    r.app->Navigate(sel);
    r.Frame();
    for (std::uint32_t tab = 0; tab < 4; ++tab) {
      r.app->view().music.tab = tab;
      r.app->view().buses.tab = tab;
      r.Frame();
      SND_CHECK_MSG(FirstDuplicate(r.rt()).empty(), "%s tab %u: duplicate %s", sel, tab, FirstDuplicate(r.rt()).c_str());
      SND_CHECK(LimitProblem(r.rt()).empty());
    }
  }
  // Every material, state and marker row is there (by its name key).
  r.app->Navigate(G(25));
  r.Frame();
  MaterialEditor me(r.doc, G(25));
  std::size_t rows = 0;
  for (const std::string& m : me.Materials()) rows += r.El("ma.mat." + ui::NameKey(m)) != nullptr ? 1u : 0u;
  SND_CHECK_EQ(rows, me.Materials().size());
}

// ============================================================================================
// L8 (D-188): the rebind question.
// ============================================================================================
namespace {
struct MixRig : Rig {
  std::string tmp;
  MixRig() {
    tmp = MixerEditor(doc).AddBus("Tmp", G(2));
    Frame();
    Show(ui::Screen::Mixer);
  }
  std::int32_t Bus(const char* n) const { return s.project().FindBus(snd::HashId(n)); }
  bool HasLive(const char* bus, float v) const {
    for (const LiveValue& lv : s.mixer().LiveValues()) {
      if (lv.control == Control::BusFader && lv.value == v && lv.name == bus) return true;
    }
    return false;
  }
};
}  // namespace

SND_TEST(IndepS7UiRecheck, RebindCarriesWhatItCanAndAsksForTheRest) {
  MixRig r;
  SND_REQUIRE(!r.tmp.empty());
  SND_REQUIRE(r.Bus("Tmp") >= 0);
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, r.Bus("Tmp"), 0, 0}, -20.0f));
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, r.Bus("Sfx"), 0, 0}, -10.0f));
  r.Frame();
  // A new bus: everything has a place, carried, no question.
  SND_REQUIRE(!MixerEditor(r.doc).AddBus("New", G(2)).empty());
  r.Frame();
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK(r.HasLive("Tmp", -20.0f));
  SND_CHECK(r.HasLive("Sfx", -10.0f));
  // Tmp removed: its value has no place: pending, the question is shown, every override still there.
  SND_REQUIRE(r.doc.Remove(r.tmp));
  r.Frame();
  SND_CHECK(r.app->rebindPending());
  SND_CHECK(r.El("rebind.writeback") != nullptr && r.El("rebind.discard") != nullptr);
  SND_CHECK_EQ(r.app->rebindDropped().size(), std::size_t{1});
  SND_CHECK(r.HasLive("Tmp", -20.0f));
  // Edits while asking: a value edit and another structural edit keep the question (and its values).
  SND_REQUIRE(MixerEditor(r.doc).SetVolume(G(4), -3.0));
  r.Frame();
  SND_REQUIRE(!MixerEditor(r.doc).AddBus("New2", G(2)).empty());
  r.Frame();

  SND_CHECK(r.app->rebindPending());
  SND_CHECK(r.HasLive("Tmp", -20.0f));
  // Undo back to before the removal: the question goes.
  r.Show(ui::Screen::Event);
  r.Key('Z', ui::kModControl);
  r.Key('Z', ui::kModControl);
  r.Key('Z', ui::kModControl);
  r.Frame();
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK(r.El("rebind.writeback") == nullptr);
  SND_CHECK(r.HasLive("Tmp", -20.0f));
  SND_CHECK(r.HasLive("Sfx", -10.0f));
  // Again, then "Discard": the other override stays, Tmp's goes.
  SND_REQUIRE(r.doc.Remove(r.tmp));
  r.Frame();
  SND_REQUIRE(r.app->rebindPending());
  SND_REQUIRE(r.ClickId("rebind.discard"));
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK(r.HasLive("Sfx", -10.0f));
  SND_CHECK(r.Bus("Tmp") < 0);
  SND_CHECK(FirstDuplicate(r.rt()).empty());
}

SND_TEST(IndepS7UiRecheck, RebindQuestionGoesWhenTheUnplacedOverrideIsReverted) {
  MixRig r;
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, r.Bus("Tmp"), 0, 0}, -20.0f));
  r.Frame();
  SND_REQUIRE(r.doc.Remove(r.tmp));
  r.Frame();
  SND_REQUIRE(r.app->rebindPending());
  SND_REQUIRE(r.s.mixer().RevertAll());
  r.Frame();
  r.Frame();
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK(r.Bus("Tmp") < 0);
  SND_CHECK_EQ(r.s.mixer().buses().size(), r.doc.OfKind("bus").size());
}

// "Write back" with a removed bus: the document gets the values that have a place; nothing is
// written for the removed bus (it is not re-created) and the question goes.
SND_TEST(IndepS7UiRecheck, RebindWriteBackWritesWhatHasAPlace) {
  MixRig r;
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, r.Bus("Tmp"), 0, 0}, -20.0f));
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, r.Bus("Sfx"), 0, 0}, -10.0f));
  r.Frame();
  SND_REQUIRE(r.doc.Remove(r.tmp));
  r.Frame();
  SND_REQUIRE(r.app->rebindPending());
  const std::size_t buses = r.doc.OfKind("bus").size();
  SND_REQUIRE(r.ClickId("rebind.writeback"));
  r.Frame();
  SND_CHECK(!r.app->rebindPending());
  SND_CHECK_EQ(NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0), -10.0);
  SND_CHECK_EQ(r.doc.OfKind("bus").size(), buses);
  SND_CHECK(r.doc.Find(r.tmp) == nullptr);
}

// Defect hunt: an effect whose type changed under a live override of one of its parameters. The
// override has no place (another type at that index): pending. "Write back" must not put the old
// effect's value into the new effect (nor create an effect where one was removed).
SND_TEST(IndepS7UiRecheck, Defect_RebindWriteBackPutsAnOverrideIntoAnotherEffect) {
  Rig r;
  r.Show(ui::Screen::Mixer);
  const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(r.s.mixer().Set(Target{Control::EffectParam, sfx, 0, 0}, 500.0f));
  r.Frame();
  MixerEditor mx(r.doc);
  std::string other;
  for (const EffectTypeInfo& t : StandardEffectTypes()) {
    if (t.name != "snd.fx.lowpass" && !t.params.empty()) {
      other = t.name;
      break;
    }
  }
  SND_REQUIRE(!other.empty());
  SND_REQUIRE(mx.RemoveEffect(G(3), 0));
  SND_REQUIRE(mx.AddEffect(G(3), other, 0));
  r.Frame();
  SND_REQUIRE(r.app->rebindPending());
  const double before = mx.EffectParam(G(3), 0, 0);
  SND_REQUIRE(r.ClickId("rebind.writeback"));
  r.Frame();
  SND_CHECK_EQ(StringAt(r.doc.Find(G(3))->doc, "effects[0].type"), other);
  SND_CHECK_MSG(mx.EffectParam(G(3), 0, 0) == before, "%s param 0: %g -> %g (the lowpass override went into it)",
                other.c_str(), before, mx.EffectParam(G(3), 0, 0));
  // Removed effect: no effect appears.
  Rig q;
  q.Show(ui::Screen::Mixer);
  const std::int32_t s2 = q.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(q.s.mixer().Set(Target{Control::EffectParam, s2, 0, 0}, 500.0f));
  q.Frame();
  SND_REQUIRE(MixerEditor(q.doc).RemoveEffect(G(3), 0));
  q.Frame();
  SND_REQUIRE(q.app->rebindPending());
  SND_REQUIRE(q.ClickId("rebind.writeback"));
  q.Frame();
  const json::Value* fx = FindPath(q.doc.Find(G(3))->doc, "effects");
  SND_CHECK_MSG(fx == nullptr || fx->items.empty(), "%u effects after writing back over a removed effect",
                fx == nullptr ? 0u : static_cast<unsigned>(fx->items.size()));
}

// ============================================================================================
// L9 (D-189): the cone is shown the same on the Event and 3D screens (EventEditor::Cone).
// ============================================================================================
SND_TEST(IndepS7UiRecheck, ConeIsShownAlikeOnBothScreens) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  auto label = [&](const std::string& id) {
    const tela::PlacedElement* e = r.El(id);
    return e != nullptr ? e->element.label : std::string("<none>");
  };
  // No cone: 0 / 0 / 0 on the Event screen, "none" on the 3D screen.
  SND_CHECK(!ed.Cone().present);
  SND_CHECK(label("ev.f.cone.in.value").rfind("0", 0) == 0);
  SND_CHECK(label("ev.f.cone.out.value").rfind("0", 0) == 0);
  r.Show(ui::Screen::Spatial);
  SND_CHECK(label("sp.info").find("cone none") != std::string::npos);
  float in = 1, out = 1, db = 1;
  SND_REQUIRE(r.app->spatial() != nullptr);
  r.app->spatial()->Cone(&in, &out, &db);
  SND_CHECK(in == 0.0f && out == 0.0f && db == 0.0f);
  // A cone: both screens and the preview model agree.
  SND_REQUIRE(ed.SetCone(30.0, 90.0, -6.0));
  r.Frame();
  SND_CHECK(label("sp.info").find("30 / 90 deg -6 dB") != std::string::npos);
  r.app->spatial()->Cone(&in, &out, &db);
  SND_CHECK(in == 30.0f && out == 90.0f && db == -6.0f);
  r.app->Navigate(G(8));
  r.Frame();
  SND_CHECK(label("ev.f.cone.in.value") == "30 deg");
  SND_CHECK(label("ev.f.cone.out.value") == "90 deg");
  SND_CHECK(label("ev.f.cone.db.value") == "-6 dB");
}

// ============================================================================================
// L10 (D-189 and group P): the Link screen: not connected says so (no peer version); a failed
// reload lists every failed bank with its reason.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, LinkNotConnectedShowsNoPeerVersion) {
  Rig r;
  r.Show(ui::Screen::Link);
  SND_CHECK(r.AnyLabelContains("not connected"));
  SND_CHECK(!r.AnyLabelContains("protocol 1.0"));
  SND_CHECK(!r.AnyLabelContains("protocol 1.1"));
  SND_CHECK(r.Prefixed("ln.failed.").empty());
}

SND_TEST(IndepS7UiRecheck, LinkFailedBanksAreListed) {
  ProjectDocument doc;
  livetest::LoadLive(doc);
  const DocBuild build = BuildDocument(doc);
  std::string err;
  std::unique_ptr<preview::PreviewEngine> engine = preview::PreviewEngine::Create(preview::PreviewEngineDesc{}, &err);
  SND_REQUIRE_MSG(engine != nullptr && engine->LoadBuild(build, &err), "%s", err.c_str());
  Project p;
  p.Load(doc);
  Session s;
  s.UseProject(p);
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  o.writer = [](const BankImage&, std::string*, std::string* error) {
    *error = "indep: the disk is full";
    return false;
  };
  s.AttachDocument(&doc, o);
  ui::AppDesc d;
  d.connection.connector = engine->connector();
  d.workspace.document = &doc;
  d.workspace.preview = engine.get();
  ui::App app(s, d);
  SND_REQUIRE_EQ(s.Connect(d.connection), SND_OK);
  app.OnScreen(ui::Screen::Link);
  auto until = [&](auto pred) {
    for (int i = 0; i < 3000; ++i) {
      engine->Render(1, nullptr);
      app.Frame(livetest::Now());
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  SND_REQUIRE(until([&] { return s.live()->phase() == LivePhase::Live; }));
  json::Value init = json::MakeObject();
  json::Add(init, "bank", String("Main"));
  SND_REQUIRE(!doc.Create("event", "IndepNew", &init).empty());
  app.Edited();
  SND_REQUIRE(until([&] { return s.live()->phase() == LivePhase::Failed; }));
  app.Frame(livetest::Now());
  std::size_t rows = 0;
  for (const tela::PlacedElement& e : app.runtime().elements()) {
    if (StartsWith(e.element.id, "ln.failed.")) {
      ++rows;
      SND_CHECK(e.element.label.find("the disk is full") != std::string::npos);
    }
  }
  SND_CHECK_EQ(rows, s.live()->failedBanks().size());
  SND_CHECK(rows >= 1u);
  const tela::PlacedElement* st = uitest::Find(app.runtime(), "link.state");
  SND_CHECK(st != nullptr && st->element.label.find("failed") != std::string::npos);
  // Every screen still declares without duplicates while Failed.
  for (ui::Screen sc : kAllScreens) {
    app.OnScreen(sc);
    app.Frame(livetest::Now());
    SND_CHECK(FirstDuplicate(app.runtime()).empty());
  }
  s.Disconnect();
  s.AttachDocument(nullptr);
}

// ============================================================================================
// L11 (D-189 corrected): problems once on the screen and in the footer; CheckDocument unchanged.
// ============================================================================================
SND_TEST(IndepS7UiRecheck, ProblemsOnceOnScreenAndFooterCheckDocumentAsTheBuild) {
  std::map<std::string, std::string> f = doctest::BaseFiles();
  f["events/Broken.json"] = "{\"type\": \"event\", \"guid\": \"" + G(40) +
                            "\", \"name\": \"Broken\", \"bank\": \"Main\", \"tracks\": [{\"clips\": [{\"asset\": \"nope\"}, "
                            "{\"asset\": \"nope2\"}]}]}\n";
  Rig r(f);
  r.app->RefreshProblems();
  r.Show(ui::Screen::Problems);
  const std::vector<DocIssue> all = CheckDocument(r.doc);
  const std::vector<DocIssue> once = DistinctIssues(all);
  SND_CHECK_EQ(r.app->problems().size(), once.size());
  std::set<std::string> seen;
  for (const DocIssue& i : r.app->problems()) {
    const std::string k = i.file + ":" + std::to_string(i.line) + ":" + std::to_string(i.column) + ":" + i.message +
                          (i.error ? "e" : "w");
    SND_CHECK_MSG(seen.insert(k).second, "twice: %s", i.text.c_str());
  }
  // DistinctIssues keeps the order and drops only repeats.
  std::size_t j = 0;
  for (const DocIssue& i : all) {
    if (j < once.size() && once[j].text == i.text && once[j].line == i.line) ++j;
  }
  SND_CHECK_EQ(j, once.size());
}

