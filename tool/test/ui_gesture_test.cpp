// Pointer gestures on the drawn controls (pointer_router.hpp, control_math.hpp):
// drag distance to value, saturation at the ends, Shift fine drag, double click
// to the bank value, wheel steps, one undo step per gesture, and Tela keeping
// the gesture while the declaration is replaced every frame.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/ui/widgets.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;
using ui::FaderDbToPos;
using ui::FaderPosToDb;

namespace {

struct Rig {
  tooltest::ToolProject tool;
  Session s;
  ui::App app;
  Pointer ptr;
  Rig() : app(s, ui::AppDesc{}), ptr(app) {
    SND_REQUIRE(tool.ok);
    s.UseProject(tool.project);
    app.Frame(tooltest::Now());
  }
  std::int32_t Bus(const char* n) const { return tooltest::Bus(tool.project, n); }
  const tela::PlacedElement* El(const std::string& id) const { return Find(app.runtime(), id); }
  // Fader of a bus: hit bounds, travel and the y of the cap for a dB value.
  struct F {
    tela::Rect r;
    float travel;
    float x;
    float CapY(float db) const { return r.y + ui::FaderGeometry{r.height, 18.0f}.CapCenterY(FaderDbToPos(db)); }
  };
  F Fader(const char* bus) const {
    const tela::PlacedElement* p = El(ui::BusId(snd::HashId(bus)) + ".fader.hit");
    SND_CHECK(p != nullptr);
    if (p == nullptr) return F{};
    F f{p->bounds, p->bounds.height - 18.0f, p->bounds.x + p->bounds.width * 0.5f};
    return f;
  }
  float Db(const char* bus) const { return s.mixer().buses()[static_cast<std::size_t>(Bus(bus))].faderDb; }
};

}  // namespace

SND_TEST(ToolUiGesture, FaderLawRoundTripsAndSaturates) {
  for (float db : {-89.0f, -75.0f, -60.0f, -45.0f, -30.0f, -20.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
    SND_CHECK_NEAR(FaderPosToDb(FaderDbToPos(db)), db, 1e-4);
  }
  SND_CHECK_EQ(FaderDbToPos(0.0f), 0.75f);
  SND_CHECK_EQ(FaderDbToPos(-12.0f), 0.5f);
  SND_CHECK_EQ(FaderDbToPos(kFaderOffDb), 0.0f);
  SND_CHECK_EQ(FaderDbToPos(-90.0f), 0.0f);  // the bottom: shown and dragged as -inf
  SND_CHECK_EQ(FaderPosToDb(0.0f), kFaderOffDb);
  SND_CHECK_EQ(FaderPosToDb(-1.0f), kFaderOffDb);
  SND_CHECK_EQ(FaderPosToDb(2.0f), kFaderMaxDb);
  SND_CHECK_NEAR(FaderPosToDb(0.625f), -6.0f, 1e-5);
  // Monotonic.
  float prev = FaderPosToDb(0.001f);
  for (int i = 2; i <= 1000; ++i) {
    const float v = FaderPosToDb(static_cast<float>(i) / 1000.0f);
    SND_CHECK(v > prev);
    prev = v;
  }
  SND_CHECK_EQ(ui::FaderWheel(kFaderOffDb, 1.0f, false), -90.0f);
  SND_CHECK_EQ(ui::FaderWheel(-89.5f, -1.0f, false), kFaderOffDb);
  SND_CHECK_EQ(ui::FaderWheel(11.5f, 1.0f, false), kFaderMaxDb);
  SND_CHECK_NEAR(ui::FaderWheel(0.0f, 3.0f, true), 0.3f, 1e-6);
}

SND_TEST(ToolUiGesture, FaderDragMapsDistanceToPositionAndSaturates) {
  Rig t;
  const Rig::F f = t.Fader("Sfx");
  const float y0 = f.CapY(0.0f);
  const std::size_t undo0 = t.s.mixer().history().UndoCount();
  SND_CHECK(t.ptr.Down(f.x, y0));
  SND_CHECK(t.app.router().dragging());
  // A quarter of the travel up: p 0.75 -> 1.0 = +12 dB. Redeclared between moves.
  t.ptr.Move(f.x, y0 - f.travel * 0.25f);
  t.app.Frame(tooltest::Now());
  SND_CHECK_NEAR(t.Db("Sfx"), 12.0f, 0.05f);
  t.ptr.Move(f.x, y0 - f.travel);  // past the top: saturates
  t.app.Frame(tooltest::Now());
  SND_CHECK_EQ(t.Db("Sfx"), kFaderMaxDb);
  SND_CHECK(t.app.router().dragging());  // Tela kept the gesture across frames
  t.ptr.Move(f.x, y0 + f.travel * 0.25f);  // p 0.5 = -12 dB
  SND_CHECK_NEAR(t.Db("Sfx"), -12.0f, 0.05f);
  // Pointer positions are whole pixels: the expected position uses the rounded distance.
  auto expect = [&](float y) {
    return FaderPosToDb(0.75f + (std::round(y0) - std::round(y)) / f.travel);
  };
  t.ptr.Move(f.x, y0 + f.travel * 0.6f);  // p ~0.15 = ~-45 dB
  SND_CHECK_NEAR(t.Db("Sfx"), expect(y0 + f.travel * 0.6f), 1e-3);
  SND_CHECK_NEAR(t.Db("Sfx"), -45.0f, 0.5f);
  t.ptr.Move(f.x, y0 + f.travel * 2.0f);  // past the bottom: -inf
  t.app.Frame(tooltest::Now());
  SND_CHECK_EQ(t.Db("Sfx"), kFaderOffDb);
  t.ptr.Move(f.x + 300.0f, y0 + f.travel * 0.125f);  // horizontal motion does not matter
  SND_CHECK_NEAR(t.Db("Sfx"), expect(y0 + f.travel * 0.125f), 1e-3);
  t.ptr.Up(f.x + 300.0f, y0);
  SND_CHECK(!t.app.router().dragging());
  SND_CHECK_EQ(t.s.mixer().history().UndoCount(), undo0 + 1u);  // the whole drag is one step
  SND_CHECK(t.s.mixer().Undo());
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);
  // Moves after the release change nothing.
  t.ptr.Move(f.x, y0 - 50.0f);
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);
}

SND_TEST(ToolUiGesture, ShiftDragIsTenTimesFinerAndReanchors) {
  Rig t;
  const Rig::F f = t.Fader("Music");
  const float y0 = f.CapY(0.0f);
  t.ptr.Down(f.x, y0);
  t.ptr.Move(f.x, y0 - f.travel * 0.2f, ui::kModShift);  // 0.02 of travel
  SND_CHECK_NEAR(FaderDbToPos(t.Db("Music")), 0.77f, 2e-3);
  // Shift released: the next motion counts fully from here, no jump.
  const float y1 = y0 - f.travel * 0.2f;
  t.ptr.Move(f.x, y1);
  SND_CHECK_NEAR(FaderDbToPos(t.Db("Music")), 0.77f, 2e-3);
  t.ptr.Move(f.x, y1 - f.travel * 0.1f);
  SND_CHECK_NEAR(FaderDbToPos(t.Db("Music")), 0.87f, 2e-3);
  t.ptr.Up(f.x, y1);
}

SND_TEST(ToolUiGesture, DoubleClickSetsTheBankValue) {
  Rig t;
  t.s.mixer().Set(Target{Control::BusFader, t.Bus("Sfx"), 0, 0}, -20.0f);
  t.app.Frame(tooltest::Now());
  const Rig::F f = t.Fader("Sfx");
  const float y = f.CapY(-20.0f);
  t.ptr.Down(f.x, y);
  t.ptr.Up(f.x, y);
  t.ptr.Advance(150);
  const std::size_t undo0 = t.s.mixer().history().UndoCount();
  t.ptr.Down(f.x + 2.0f, y + 1.0f);
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);  // the bank's value
  SND_CHECK(!t.app.router().dragging());
  t.ptr.Move(f.x, y - 100.0f);  // the second press does not drag
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);
  t.ptr.Up(f.x, y);
  SND_CHECK_EQ(t.s.mixer().history().UndoCount(), undo0 + 1u);
  // Too slow: not a double click.
  t.s.mixer().Set(Target{Control::BusFader, t.Bus("Sfx"), 0, 0}, -20.0f);
  t.app.Frame(tooltest::Now());
  t.ptr.Advance(1000);
  t.ptr.Down(f.x, y);
  t.ptr.Up(f.x, y);
  t.ptr.Advance(401);
  t.ptr.Down(f.x, y);
  t.ptr.Up(f.x, y);
  SND_CHECK_EQ(t.Db("Sfx"), -20.0f);
}

SND_TEST(ToolUiGesture, WheelStepsMergeIntoOneUndoStep) {
  Rig t;
  const Rig::F f = t.Fader("Sfx");
  const float y = f.CapY(0.0f);
  const std::size_t undo0 = t.s.mixer().history().UndoCount();
  for (int i = 0; i < 3; ++i) t.ptr.Wheel(f.x, y, 1.0f);
  SND_CHECK_NEAR(t.Db("Sfx"), 3.0f, 1e-5);
  t.ptr.Wheel(f.x, y, -1.0f, ui::kModShift);
  SND_CHECK_NEAR(t.Db("Sfx"), 2.9f, 1e-5);
  SND_CHECK_EQ(t.s.mixer().history().UndoCount(), undo0 + 1u);
  SND_CHECK(t.s.mixer().Undo());
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);
  // Wheel over a passthrough area (the strip's name) does nothing.
  const tela::PlacedElement* name = t.El(ui::BusId(snd::HashId("Sfx")) + ".name");
  t.ptr.Wheel(Center(name).x, Center(name).y, 1.0f);
  SND_CHECK_EQ(t.Db("Sfx"), 0.0f);
}

SND_TEST(ToolUiGesture, KnobDragCoversTheRangeIn150Pixels) {
  Rig t;
  const std::string id = ui::BusId(snd::HashId("Sfx")) + ".fx0.2";  // delay DryDb: -96 .. 0, bank 0
  const tela::PlacedElement* k = t.El(id + ".hit");
  SND_REQUIRE(k != nullptr);
  const tela::Point c = Center(k);
  const Target dry{Control::EffectParam, t.Bus("Sfx"), 0, 2};
  t.ptr.Down(c.x, c.y);
  t.ptr.Move(c.x, c.y + 75.0f);  // half the range down
  t.app.Frame(tooltest::Now());
  SND_CHECK_NEAR(t.s.mixer().Get(dry), -48.0f, 1e-3);
  t.ptr.Move(c.x, c.y + 400.0f);
  SND_CHECK_EQ(t.s.mixer().Get(dry), -96.0f);
  t.ptr.Up(c.x, c.y);
  // DampingHz is logarithmic (500 .. 20000): 75 px from the bottom is the geometric middle.
  const Target damp{Control::EffectParam, t.Bus("Sfx"), 0, 4};
  t.s.mixer().Set(damp, 500.0f);
  t.app.Frame(tooltest::Now());
  const tela::Point d = Center(t.El(ui::BusId(snd::HashId("Sfx")) + ".fx0.4.hit"));
  t.ptr.Advance(1000);
  t.ptr.Down(d.x, d.y);
  t.ptr.Move(d.x, d.y - 75.0f);
  SND_CHECK_NEAR(t.s.mixer().Get(damp), std::sqrt(500.0f * 20000.0f), 1.0f);
  t.ptr.Up(d.x, d.y);
  // The knob drawing's pointer follows the value (control_math angle).
  ui::KnobShape shape;
  ui::KnobLook look;
  look.size = 32;
  look.norm = 0.5f;
  ui::DrawKnob(look, t.app.palette(), &shape);
  SND_CHECK_NEAR(shape.pointerEnd.x, shape.center.x, 1e-4);  // straight up at n = 0.5
  SND_CHECK(shape.pointerEnd.y < shape.center.y);
}

SND_TEST(ToolUiGesture, CancelEndsTheDragAndKeepsTheValue) {
  Rig t;
  const Rig::F f = t.Fader("Sfx");
  const float y0 = f.CapY(0.0f);
  t.ptr.Down(f.x, y0);
  t.ptr.Move(f.x, y0 + f.travel * 0.25f);
  SND_CHECK(t.app.router().dragging());
  t.ptr.Cancel();
  SND_CHECK(!t.app.router().dragging());
  SND_CHECK_NEAR(t.Db("Sfx"), -12.0f, 0.05f);
  SND_CHECK(!t.s.mixer().history().InGroup());
  // Losing the viewport's focus makes Tela cancel the gesture; the router follows.
  t.ptr.Advance(1000);
  t.ptr.Down(f.x, f.CapY(-12.0f));
  SND_CHECK(t.app.router().dragging());
  tela::Viewport v = t.app.runtime().viewport();
  v.focused = false;
  ++v.revision;
  t.app.runtime().viewport(v);
  t.app.Frame(tooltest::Now());
  SND_CHECK(!t.app.router().dragging());
  SND_CHECK(!t.s.mixer().history().InGroup());
}
