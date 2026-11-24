// The Event screen (design.md 11.1 Event エディタ): declaration over the document, stable ids,
// buttons and pickers through EventEditor, clip drags (start and track, one undo step), the time
// line's zoom and scroll, text entry of names and numbers, the clip box in the image.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

const std::string kShot = doctest::G(8);
using ui::EventView;

}  // namespace

SND_TEST(ToolUiEvent, DeclaresTheEventItsTracksAndClips) {
  Editor t(ui::Screen::Event);
  SND_CHECK(t.app.view().event.event == kShot);  // the first event is selected
  SND_CHECK(t.Has("ev.list." + Key(kShot)));
  for (const char* id : {"ev.f.seq", "ev.f.bus", "ev.f.cat", "ev.f.n0", "ev.f.3d", "ev.f.cone.in", "ev.track.add",
                         "ev.ruler", "ev.t0.lane", "ev.t0.lane.hit", "ev.t0.c0", "ev.t0.select", "ev.clip.none"}) {
    SND_CHECK_MSG(t.Has(id), "missing %s", id);
  }
  SND_CHECK(t.Label("ev.f.bus").find("Sfx") == 0u);
  // Ids do not change with frames or values.
  const std::set<std::string> a = Ids(t.app.runtime());
  t.Frame();
  SND_CHECK(Ids(t.app.runtime()) == a);
  EventEditor(t.doc, kShot).SetNumber(EventNumber::Priority, 9);
  t.Frame();
  SND_CHECK(Ids(t.app.runtime()) == a);
  // Away and back.
  t.app.OnScreen(ui::Screen::Mixer);
  t.Frame();
  t.app.OnScreen(ui::Screen::Event);
  t.Frame();
  SND_CHECK(Ids(t.app.runtime()) == a);
  SND_CHECK(t.app.runtime().elements().size() < ui::kMaxElements);
}

SND_TEST(ToolUiEvent, ClipDragMovesTheStartAsOneUndoStep) {
  Editor t(ui::Screen::Event);
  EventView& ev = t.app.view().event;
  ev.msPerPx = 2.0;
  t.Frame();
  const std::size_t undo0 = t.doc.UndoCount();
  t.Drag("ev.t0.c0", 50.0f, 0.0f, 5);
  EventEditor ed(t.doc, kShot);
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 100.0, 1e-9);  // 50 px x 2 ms
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  SND_CHECK_EQ(ev.clip, 0);  // the press selected it
  SND_CHECK(t.Has("ev.clip.t0.c0.n0"));
  // Back past zero: clamped at 0.
  t.Drag("ev.t0.c0", -400.0f, 0.0f, 4);
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0);
  t.Key('Z', ui::kModControl);  // the document's undo (not the mixer's)
  SND_CHECK_NEAR(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 100.0, 1e-9);
  t.Key('Y', ui::kModControl);
  SND_CHECK_EQ(ed.ClipNumberOf(0, 0, ClipNumber::StartMs), 0.0);
}

SND_TEST(ToolUiEvent, ClipDragDownMovesItToTheNextTrack) {
  Editor t(ui::Screen::Event);
  SND_REQUIRE(t.Click("ev.track.add"));
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE_EQ(ed.TrackCount(), 2u);
  const std::size_t undo0 = t.doc.UndoCount();
  t.Drag("ev.t0.c0", 0.0f, 48.0f, 4);  // one lane down
  SND_CHECK_EQ(ed.ClipCount(0), 0u);
  SND_CHECK_EQ(ed.ClipCount(1), 1u);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  SND_CHECK_EQ(t.app.view().event.track, 1u);
  SND_CHECK(t.Has("ev.t1.c0"));
  SND_CHECK(t.app.DocUndo());
  SND_CHECK_EQ(ed.ClipCount(0), 1u);
}

SND_TEST(ToolUiEvent, ButtonsAndPickersEditThroughTheModel) {
  Editor t(ui::Screen::Event);
  EventEditor ed(t.doc, kShot);
  SND_REQUIRE(t.Pick("ev.f.seq", "random"));
  SND_CHECK(ed.sequence() == SequenceKind::Random);
  SND_REQUIRE(t.Pick("ev.f.bus", "Reverb"));
  SND_CHECK_EQ(StringAt(*ed.event(), "bus"), doctest::G(4));
  SND_REQUIRE(t.Click("ev.f.3d"));
  SND_CHECK(!ed.Flag(EventFlag::Is3D));
  // A number field: + and -, then typing.
  const double prio = ed.NumberOf(EventNumber::Priority);
  SND_REQUIRE(t.Click("ev.f.n1.inc"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), prio + 1.0);
  SND_REQUIRE(t.Click("ev.f.n1.dec"));
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), prio);
  SND_REQUIRE(t.Click("ev.f.n1"));  // a click without motion types the value
  SND_CHECK(t.app.text().active());
  t.ClearEntry();
  t.Type("200");
  SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 200.0);
  SND_CHECK(!t.app.text().active());
  // A refused value keeps the entry open.
  SND_REQUIRE(t.Click("ev.f.n1"));
  t.ClearEntry();
  t.Type("abc");  // letters are not taken by a number field: the text stays empty -> refused
  SND_CHECK(t.app.text().active());
  t.Key(ui::kKeyEscape);
  SND_CHECK(!t.app.text().active());
  // Tracks and clips.
  SND_REQUIRE(t.Click("ev.track.add"));
  SND_CHECK_EQ(ed.TrackCount(), 2u);
  SND_CHECK_EQ(t.app.view().event.track, 1u);
  SND_REQUIRE(t.Click("ev.clip.add"));  // the first asset ("tone") on track 2
  SND_CHECK_EQ(ed.ClipCount(1), 1u);
  SND_CHECK_EQ(t.app.view().event.clip, 0);
  SND_REQUIRE(t.Click("ev.clip.t1.c0.remove"));
  SND_CHECK_EQ(ed.ClipCount(1), 0u);
  SND_REQUIRE(t.Click("ev.track.remove"));
  SND_CHECK_EQ(ed.TrackCount(), 1u);
  // Actions of the track.
  SND_REQUIRE(t.Click("ev.track.0.action.add"));
  SND_CHECK_EQ(ed.ActionCount(0), 1u);
  SND_REQUIRE(t.Pick("ev.track.0.action.kind", "setParameter"));
  SND_CHECK(ed.Action(0, 0).action == ActionKind::SetParameter);
  SND_CHECK_EQ(ed.Action(0, 0).target, doctest::G(6));  // the first parameter
}

SND_TEST(ToolUiEvent, RenameNewAndRemoveEvents) {
  Editor t(ui::Screen::Event);
  SND_REQUIRE(t.Click("ev.list.name"));
  t.ClearEntry();
  t.Type("Gun");
  SND_CHECK_EQ(t.doc.Find(kShot)->name(), std::string("Gun"));
  SND_REQUIRE(t.Click("ev.list.new"));
  const std::string g = t.app.view().event.event;
  SND_REQUIRE(g != kShot);
  SND_CHECK_EQ(t.doc.Find(g)->name(), std::string("Event 1"));
  SND_CHECK(t.Has("ev.t0.lane"));  // a new event has one empty track
  SND_REQUIRE(t.Click("ev.list.remove"));
  SND_CHECK(t.doc.Find(g) == nullptr);
  SND_CHECK(t.app.view().event.event == kShot);
  // Ctrl+S saves (in memory: nothing to write -> a message, no crash).
  t.Key('S', ui::kModControl);
}

SND_TEST(ToolUiEvent, WheelZoomsAndDragScrollsTheTimeLine) {
  Editor t(ui::Screen::Event);
  EventView& ev = t.app.view().event;
  const double per = ev.msPerPx;
  const tela::PlacedElement* lane = t.El("ev.t0.lane.hit");
  SND_REQUIRE(lane != nullptr);
  // Right of the clip, on the lane's background.
  const tela::Point p{lane->bounds.x + lane->bounds.width - 20.0f, lane->bounds.y + 10.0f};
  t.ptr.Wheel(p.x, p.y, 1.0f);
  SND_CHECK_NEAR(ev.msPerPx, per * 0.8, 1e-9);
  t.Frame();
  ev.scrollMs = 500.0;
  t.Frame();
  t.DragFrom(p, 40.0f, 0.0f, 2);
  SND_CHECK_NEAR(ev.scrollMs, 500.0 - 40.0 * ev.msPerPx, 1e-6);
  SND_CHECK(t.Click("ev.zoom.fit"));
  SND_CHECK_EQ(ev.scrollMs, 0.0);
}

SND_TEST(ToolUiEvent, ClipBoxIsDrawnAtItsTime) {
  for (float dpi : {1.0f, 2.0f}) {
    Editor t(ui::Screen::Event, static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    EventView& ev = t.app.view().event;
    ev.msPerPx = 1.0;
    EventEditor(t.doc, kShot).MoveClip(0, 0, 100.0);
    t.Frame();
    const tela::PlacedElement* lane = t.El("ev.t0.lane");
    SND_REQUIRE(lane != nullptr);
    tela::PixelSurface img;
    SND_REQUIRE(ui::Compose(t.app.runtime(), ui::ComposeOptions{}, &img));
    // The clip (100 ms .. 200 ms: 4800 frames at 48 kHz) from x 100 to 200 in the lane.
    const tela::Color fill = ui::Mix(t.app.palette().accent, t.app.palette().canvas, 0.45f);
    auto at = [&](float lx) {
      unsigned char rgb[3];
      ui::PixelRgb(img, static_cast<int>((lane->bounds.x + lx) * dpi), static_cast<int>((lane->bounds.y + 22.0f) * dpi), rgb);
      return std::abs(rgb[0] - fill.r) <= 3 && std::abs(rgb[1] - fill.g) <= 3 && std::abs(rgb[2] - fill.b) <= 3;
    };
    SND_CHECK(!at(96.0f));
    SND_CHECK(at(104.0f));
    SND_CHECK(at(150.0f));
    SND_CHECK(at(196.0f));
    SND_CHECK(!at(204.0f));
  }
}
