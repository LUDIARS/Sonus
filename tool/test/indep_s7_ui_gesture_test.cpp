// Independent tests of group U, gestures (indep_s7_ui_properties.md F6..F9): a drag interrupted by
// a screen change, a Tela cancel, its element going away or an undo key leaves the document's group
// closed (the next edit is a step of its own) and at most one step; number field drags and wheels.
#include <cmath>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/patch_editor.hpp"
#include "snd/tool/ui/control_math.hpp"

using namespace iu;

namespace {

struct EventRig : Rig {
  EventEditor ed{doc, G(8)};
  EventRig() {
    app->Navigate(G(8));
    Frame();
  }
  double Start() const { return ed.ClipNumberOf(0, 0, ClipNumber::StartMs); }
  tela::Point Clip() const {
    const tela::PlacedElement* c = El("ev.t0.c0");
    return c != nullptr ? uitest::Center(c) : tela::Point{-1, -1};
  }
  // After an interrupted gesture: the group is closed and a model edit is a step of its own.
  void CheckClosed(const char* what) {
    SND_CHECK_MSG(!doc.InGroup(), "%s: the document's group is still open", what);
    const std::size_t u = doc.UndoCount();
    SND_CHECK_MSG(u <= 1, "%s: %u steps", what, static_cast<unsigned>(u));
    SND_REQUIRE(ed.SetNumber(EventNumber::Priority, 7.0));
    SND_CHECK_MSG(doc.UndoCount() == u + 1, "%s: the next edit merged into the drag (%u -> %u)", what,
                  static_cast<unsigned>(u), static_cast<unsigned>(doc.UndoCount()));
    SND_REQUIRE(doc.Undo());
    SND_CHECK_EQ(ed.NumberOf(EventNumber::Priority), 128.0);
  }
};

}  // namespace

// F7: a drag goes on over many redeclarations with values following the pointer.
SND_TEST(IndepS7UiGesture, ClipDragAcrossManyDeclarationsIsContinuous) {
  EventRig r;
  const tela::Point p = r.Clip();
  const double mpp = r.app->view().event.msPerPx;
  r.ptr->Down(p.x, p.y);
  r.Frame();
  for (int i = 1; i <= 30; ++i) {
    r.ptr->Move(p.x + static_cast<float>(i), p.y);
    r.Frame();
    r.Frame();  // and an extra declaration without input
    SND_CHECK_NEAR(r.Start(), i * mpp, 1e-6);
  }
  r.ptr->Up(p.x + 30.0f, p.y);
  r.Frame();
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{1});
  SND_CHECK(!r.doc.InGroup());
}

// F8: the screen changes (F key) while the clip is held.
SND_TEST(IndepS7UiGesture, ScreenChangeDuringAClipDragClosesTheStep) {
  EventRig r;
  const tela::Point p = r.Clip();
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x + 20.0f, p.y);
  r.Frame();
  r.Key(ui::kKeyF1 + 3);  // F4: Buses
  r.ptr->Move(p.x + 60.0f, p.y);
  r.Frame();
  r.ptr->Up(p.x + 60.0f, p.y);
  r.ptr->Advance(1000);
  r.Frame();
  r.Frame();
  r.CheckClosed("screen change");
}

// F8: Tela cancels the gesture (focus lost).
SND_TEST(IndepS7UiGesture, TelaCancelDuringAClipDragClosesTheStep) {
  EventRig r;
  const tela::Point p = r.Clip();
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x + 20.0f, p.y);
  r.Frame();
  r.ptr->Cancel();
  r.Frame();
  r.Frame();
  r.CheckClosed("cancel");
}

// F8: the dragged clip's element goes away (its event removed from outside).
SND_TEST(IndepS7UiGesture, ElementGoneDuringAClipDragClosesTheStep) {
  EventRig r;
  const tela::Point p = r.Clip();
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x + 20.0f, p.y);
  r.Frame();
  // The screen shows another event: the clip element is gone.
  r.app->view().event.event = G(8) == r.app->view().event.event ? std::string() : G(8);
  r.Frame();
  r.ptr->Move(p.x + 40.0f, p.y);
  r.Frame();
  r.ptr->Up(p.x + 40.0f, p.y);
  r.Frame();
  SND_CHECK(!r.doc.InGroup());
  SND_CHECK(r.doc.UndoCount() <= 1);
}

// F8: Ctrl+Z while a clip is held (the group is open): the document stays consistent.
SND_TEST(IndepS7UiGesture, UndoKeyDuringAClipDragLeavesAConsistentHistory) {
  EventRig r;
  SND_REQUIRE(r.ed.SetNumber(EventNumber::Priority, 9.0));  // a step to undo
  r.Frame();
  const tela::Point p = r.Clip();
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x + 20.0f, p.y);
  r.Frame();
  r.Key('Z', ui::kModControl);
  r.ptr->Move(p.x + 40.0f, p.y);
  r.Frame();
  r.ptr->Up(p.x + 40.0f, p.y);
  r.ptr->Advance(1000);
  r.Frame();
  SND_CHECK(!r.doc.InGroup());
  // Undo everything there is: back to the loaded document.
  while (r.doc.Undo()) {
  }
  SND_CHECK_EQ(r.ed.NumberOf(EventNumber::Priority), 128.0);
  SND_CHECK_EQ(r.Start(), 0.0);
  SND_CHECK(!r.doc.IsDirty());
}

// F9: press and release on a clip without motion: selection only, no step.
SND_TEST(IndepS7UiGesture, PressWithoutMotionIsNoStep) {
  EventRig r;
  const tela::Point p = r.Clip();
  r.ptr->Down(p.x, p.y);
  r.Frame();
  r.ptr->Move(p.x, p.y);  // a move event without motion
  r.Frame();
  r.ptr->Up(p.x, p.y);
  r.Frame();
  SND_CHECK_EQ(r.doc.UndoCount(), std::size_t{0});
  SND_CHECK(!r.doc.IsDirty());
  SND_CHECK_EQ(r.app->view().event.clip, 0);
}

// F6: number field drag: kNumberDragPixels per step (step measured with the "+" button), Shift
// 1/10, clamped to the range, one step per drag; wheel notches merge into one step.
SND_TEST(IndepS7UiGesture, NumberFieldDragAndWheel) {
  Rig r;
  r.Show(ui::Screen::Buses);
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  auto vol = [&] { return NumberAt(r.doc.Find(G(3))->doc, "volumeDb", 0.0); };
  const double v0 = vol();
  SND_REQUIRE(r.ClickId("mx.bp.vol.inc"));
  const double step = vol() - v0;
  SND_REQUIRE(step > 0.0);
  SND_REQUIRE(r.doc.Undo());
  r.Frame();
  const std::size_t u0 = r.Undos();
  // 10 steps up.
  r.Drag(r.El("mx.bp.vol"), 0.0f, -10.0f * ui::kNumberDragPixels, 5);
  SND_CHECK_NEAR(vol(), v0 + 10.0 * step, 1e-9);
  SND_CHECK_EQ(r.Undos(), u0 + 1);
  // Shift: 1/10.
  r.Drag(r.El("mx.bp.vol"), 0.0f, 10.0f * ui::kNumberDragPixels, 5, ui::kModShift);
  SND_CHECK_NEAR(vol(), v0 + 9.0 * step, 1e-9);
  SND_CHECK_EQ(r.Undos(), u0 + 2);
  // Wheel notches in a row: one step.
  const tela::Point c = uitest::Center(r.El("mx.bp.vol"));
  for (int i = 0; i < 5; ++i) {
    r.ptr->Wheel(c.x, c.y, 1.0f);
    r.Frame();
  }
  SND_CHECK_NEAR(vol(), v0 + 14.0 * step, 1e-9);
  SND_CHECK_EQ(r.Undos(), u0 + 3);
  SND_CHECK(!r.doc.InGroup());
  // Channels: an integer 0..8; a long drag stops at 8.
  r.Drag(r.El("mx.bp.ch"), 0.0f, -100.0f * ui::kNumberDragPixels, 5);
  const double ch = NumberAt(r.doc.Find(G(3))->doc, "channels", 0.0);
  SND_CHECK(ch <= 8.0 && ch >= 0.0);
  SND_CHECK_EQ(ch, std::round(ch));
}

// F8: a number field drag interrupted by a screen change also closes its group.
SND_TEST(IndepS7UiGesture, ScreenChangeDuringANumberDragClosesTheStep) {
  Rig r;
  r.Show(ui::Screen::Buses);
  SND_REQUIRE(r.ClickId("mx.bus." + ui::ObjectKey(G(3))));
  const tela::Point c = uitest::Center(r.El("mx.bp.vol"));
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x, c.y - 20.0f);
  r.Frame();
  r.Key(ui::kKeyF1 + 4);  // F5: Event
  r.ptr->Move(c.x, c.y - 40.0f);
  r.Frame();
  r.ptr->Up(c.x, c.y - 40.0f);
  r.Frame();
  SND_CHECK(!r.doc.InGroup());
  SND_CHECK(r.doc.UndoCount() <= 1);
}

// F5 / F8: a patch node drag interrupted by Tela's cancel.
SND_TEST(IndepS7UiGesture, CancelDuringANodeDragClosesTheStep) {
  Rig r(LargeFiles(1, 1, 2, 2, 12));
  r.app->Navigate(G(26));
  r.Frame();
  const tela::PlacedElement* n = r.El("pa.node.osc0");
  SND_REQUIRE(n != nullptr);
  const tela::Point c = uitest::Center(n);
  r.ptr->Down(c.x, c.y);
  r.Frame();
  r.ptr->Move(c.x + 30.0f, c.y + 10.0f);
  r.Frame();
  r.ptr->Cancel();
  r.Frame();
  SND_CHECK(!r.doc.InGroup());
  SND_CHECK(r.doc.UndoCount() <= 1);
}
