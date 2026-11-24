// Independent tests of group U: defects (each Defect_* test fails on purpose and documents one
// defect; registered one CTest test each) and recorded behaviour of spec gaps (SpecGap_* tests
// pass and print what the screens do now; the expectation is for the acceptance side to decide).
// indep_s7_ui_properties.md K.
#include <cmath>
#include <cstdio>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer_editor.hpp"

using namespace iu;

namespace {

bool TypeInto(Rig& r, const std::string& id, const std::string& text) {
  if (!r.ClickId(id) || !r.app->text().active()) return false;
  for (int i = 0; i < 300; ++i) r.app->Key(ui::KeyEvent{ui::kKeyBackspace, 0, 0});
  r.Type(text);
  r.Key(ui::kKeyEnter);
  return true;
}

}  // namespace

// kit.hpp NumberField / text_entry.hpp: a typed value commits "through the model's setter
// (refused -> the entry stays open with a message)". The field clamps the typed text into its own
// range first (kit.cpp commitText: ClampNumber), so the model never refuses: typing -5 into the
// event's max distance silently writes 0 (a rule on the screen's side, and not what was typed).
// Expected: -5 is refused (document unchanged, entry open).
SND_TEST(IndepS7UiDefects, Defect_TypedNumberOutsideTheFieldRangeIsClampedNotRefused) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE_EQ(ed.NumberOf(EventNumber::MaxDistance), 50.0);
  SND_REQUIRE(TypeInto(r, "ev.f.n4", "-5"));
  SND_CHECK_MSG(ed.NumberOf(EventNumber::MaxDistance) == 50.0, "max distance became %g",
                ed.NumberOf(EventNumber::MaxDistance));
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
  SND_CHECK(r.app->text().active());
  r.Key(ui::kKeyEscape);
  // The same with a clip's start (min 0 on the screen, the model refuses negative starts).
  SND_REQUIRE(r.ClickId("ev.t0.c0"));
  std::string start;
  for (const tela::PlacedElement* e : r.Prefixed("ev.clip.", ".n0")) {
    if (e->element.id.size() > 3 && e->element.id.compare(e->element.id.size() - 3, 3, ".n0") == 0) start = e->element.id;
  }
  SND_REQUIRE(!start.empty());
  SND_REQUIRE(ed.MoveClip(0, 0, 300.0));
  r.Frame();
  const std::size_t u = r.doc.UndoCount();
  SND_REQUIRE(TypeInto(r, start, "-20"));
  SND_CHECK_MSG(ed.ClipNumberOf(0, 0, ClipNumber::StartMs) == 300.0, "start became %g",
                ed.ClipNumberOf(0, 0, ClipNumber::StartMs));
  SND_CHECK_EQ(r.doc.UndoCount(), u);
}

// kit.hpp NumberField: "the wheel steps (one undo step per series)". The wheel handler adds the
// notches to the value captured when the field was declared (kit.cpp: sp.value + step * notches),
// so several wheel events between two declarations (a fast wheel, a host that delivers a batch)
// count once: 5 notches give 1 step. Expected: 5 steps (the faders read the live value and do).
SND_TEST(IndepS7UiDefects, Defect_NumberFieldWheelNotchesBetweenFramesAreLost) {
  Rig r;
  r.Show(ui::Screen::Buses);
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  auto vol = [&] { return NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0); };
  const double v0 = vol();
  SND_REQUIRE(r.ClickId("mx.bp.vol.inc"));
  const double step = vol() - v0;
  SND_REQUIRE(r.doc.Undo());
  r.Frame();
  const tela::Point c = uitest::Center(r.El("mx.bp.vol"));
  for (int i = 0; i < 5; ++i) r.ptr->Wheel(c.x, c.y, 1.0f);  // no declaration in between
  r.Frame();
  SND_CHECK_MSG(std::fabs(vol() - (v0 + 5.0 * step)) < 1e-9, "5 notches moved %g steps", (vol() - v0) / step);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
}

// Decision e: "離した時に縦の移動量をレーンの間隔（48 px）で丸めたトラックへ". The release takes
// floor(local y of the release / 48) (event_view.cpp), i.e. where the pointer is in the clip's hit
// box, not how far it moved: a clip pressed near its top edge and moved up by 5 px lands on the
// track above; pressed near its bottom and moved down by 10 px, on the track below. Expected: a
// vertical motion under half a lane keeps the track (round(dy / 48) = 0).
SND_TEST(IndepS7UiDefects, Defect_ClipVerticalDragTrackComesFromThePressPointNotTheMotion) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddTrack(0));  // the clip is now on track 1 of 3
  SND_REQUIRE(ed.AddTrack(2));
  r.Frame();
  SND_REQUIRE_EQ(ed.ClipCount(1), 1u);
  const tela::PlacedElement* c = r.El("ev.t1.c0");
  SND_REQUIRE(c != nullptr);
  // Near the top, 5 px up.
  r.DragFrom(c->bounds.x + c->bounds.width * 0.5f, c->bounds.y + 2.0f, 0.0f, -5.0f, 2);
  SND_CHECK_MSG(ed.ClipCount(1) == 1u, "5 px up moved the clip to track %s", ed.ClipCount(0) == 1u ? "0" : "2");
  r.doc.Undo();
  r.Frame();
  // Near the bottom, 10 px down.
  c = r.El("ev.t1.c0");
  SND_REQUIRE(c != nullptr);
  r.DragFrom(c->bounds.x + c->bounds.width * 0.5f, c->bounds.y + c->bounds.height - 2.0f, 0.0f, 10.0f, 2);
  SND_CHECK_MSG(ed.ClipCount(1) == 1u, "10 px down moved the clip to track %s", ed.ClipCount(2) == 1u ? "2" : "0");
}

// kit.hpp NumberField: "[-] value [+]: a vertical drag on the value changes it, a click without
// motion types the value". Where the field gets little room (the parameter fields of the Params
// screen's list column, the input fields of the Material screen) the value element is 2..16
// logical px wide at 1280 x 800: its text cannot show and it can hardly be pressed (kit.cpp: the
// field is at least 40 px, the value gets the field minus 44 px, which can be negative).
// Expected: every number field's value at least 24 px wide at the default window size.
SND_TEST(IndepS7UiDefects, Defect_NumberFieldValueHasNoRoom) {
  Rig r(LargeFiles(3, 2, 6, 4, 12));
  std::string narrow;
  for (const std::string& g : {G(6), G(8), G(3), G(24), G(22), G(23), G(25), G(26)}) {
    r.app->Navigate(g);
    r.Frame();
    for (const tela::PlacedElement& p : r.rt().elements()) {
      const std::string& id = p.element.id;
      if (id.size() < 7 || id.compare(id.size() - 6, 6, ".value") != 0) continue;
      if (r.El(id.substr(0, id.size() - 6) + ".dec") == nullptr) continue;  // not a number field
      if (p.bounds.width < 24.0f) {
        char b[160];
        std::snprintf(b, sizeof b, " %s=%.0fpx", id.c_str(), p.bounds.width);
        if (narrow.find(id) == std::string::npos) narrow += b;
      }
    }
  }
  SND_CHECK_MSG(narrow.empty(), "narrow number fields:%s", narrow.c_str());
}

// app.hpp: `void Pick(const std::string& value)` with `const Picker* picker()`: the obvious call
// with the value of an item of the open list, `Pick(picker()->items[i].value)`, reads freed memory:
// App::Pick resets the picker (its items) before it calls the pick function with `value` (found by
// ASan: heap-use-after-free in PatchEditor::AddNode through the patch screen's "+ Node" pick).
// The screen's own list buttons capture a copy, so clicks are safe; a host or a test calling Pick is
// not. Expected: the pick function gets the item's value. (Without ASan the freed buffer usually
// holds the allocator's bookkeeping, so the value arrives changed.)
SND_TEST(IndepS7UiDefects, Defect_PickWithAnItemOfTheOpenListReadsFreedMemory) {
  Rig r;
  std::string got;
  const std::string want = "the second item's value, long enough to live on the heap";
  ui::Picker p;
  p.field = "indep.picker";
  p.title = "indep";
  p.items = {ui::PickerItem{"one", "the first item's value, also long enough for the heap"}, ui::PickerItem{"two", want}};
  p.pick = [&got](const std::string& v) { got = v; };
  r.app->OpenPicker(std::move(p));
  SND_REQUIRE(r.app->picker() != nullptr);
  r.app->Pick(r.app->picker()->items[1].value);
  SND_CHECK_MSG(got == want, "the pick function got %u bytes", static_cast<unsigned>(got.size()));
}

// Decision g + WriteBackMixer: with a live override the mixer is not rebound after value edits of
// the document; "Write back" must still not put the mixer's stale bank values over the newer
// document values of buses it does not override.
SND_TEST(IndepS7UiDefects, WriteBackAfterStaleMixerKeepsNewerDocumentValues) {
  Rig r;
  r.Show(ui::Screen::Mixer);
  const std::int32_t sfx = r.s.project().FindBus(snd::HashId("Sfx"));
  SND_REQUIRE(sfx >= 0);
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, sfx, 0, 0}, -10.0f));
  r.Frame();
  // A value edit of another bus in the document (Buses screen): the mixer stays as it is.
  SND_REQUIRE(MixerEditor(r.doc).SetVolume(G(4), -6.0));
  r.Frame();
  SND_REQUIRE(r.app->WriteBackMixer() >= 1u);
  r.Frame();
  SND_CHECK_EQ(NumberAt(r.doc.Find(G(4))->doc, "volumeDb", 0.0), -6.0);
  SND_CHECK_EQ(NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0), -10.0);
}

// D-188 (was a spec gap): a structural edit of the document (a new bus) rebinds the mixer and
// carries the live values over by object: the override stays, no question (everything has a place).
SND_TEST(IndepS7UiDefects, SpecGap_StructuralEditDropsLiveOverridesSilently) {
  Rig r;
  r.Show(ui::Screen::Mixer);
  SND_REQUIRE(r.s.mixer().Set(Target{Control::BusFader, 0, 0, 0}, -10.0f));
  r.Frame();
  const std::size_t before = r.s.mixer().Overrides().size();
  SND_REQUIRE(!MixerEditor(r.doc).AddBus("New", G(2)).empty());
  r.Frame();
  SND_CHECK_EQ(r.s.mixer().buses().size(), std::size_t{4});
  SND_CHECK_EQ(r.s.mixer().Overrides().size(), before);
  SND_CHECK(!r.app->rebindPending());
  bool kept = false;
  for (const LiveValue& v : r.s.mixer().LiveValues()) kept = kept || (v.control == Control::BusFader && v.value == -10.0f);
  SND_CHECK(kept);
}

// D-186 (was a spec gap): the clip selection follows the object; after an undo outside the screen
// the position selection is cleared and Delete removes nothing.
SND_TEST(IndepS7UiDefects, SpecGap_ClipSelectionIsAnIndexAcrossUndo) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  EventEditor ed(r.doc, G(8));
  SND_REQUIRE(ed.AddAssetClip(0, G(7), 500.0));  // clips: A at 0, B at 500
  SND_REQUIRE(ed.RemoveClip(0, 0));             // clips: B
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.t0.c0"));           // select B
  r.Key('Z', ui::kModControl);                  // clips: A, B
  r.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ed.ClipCount(0), 2u);
  SND_CHECK_EQ(r.app->view().event.clip, -1);
}

// D-186 (was a spec gap): Ctrl+Z while a text entry is open is the entry's: the document is not
// undone, the entry stays as it is.
SND_TEST(IndepS7UiDefects, SpecGap_UndoKeyDuringATextEntryUndoesTheDocument) {
  Rig r;
  r.app->Navigate(G(8));
  r.Frame();
  SND_REQUIRE(r.doc.SetNumber(G(3), "volumeDb", -7.0));
  r.Frame();
  SND_REQUIRE(r.ClickId("ev.list.name"));
  SND_REQUIRE(r.app->text().active());
  r.Key('Z', ui::kModControl);
  r.Key('Y', ui::kModControl);
  r.Key('Z', ui::kModControl | ui::kModShift);
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
  SND_CHECK_EQ(r.doc.RedoCount(), std::size_t{0});
  SND_CHECK_EQ(NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0), -7.0);
}
