// Independent tests (tool/test/indep_properties.md C, D, F): pointer gestures through the
// real App and Tela runtime (drag without vertical motion, Shift, double click limits,
// wheel merging, parameter knobs), SafeDrawing at Tela's limits, and drawing at large
// DPI scales (a Tela exception ends the -fno-exceptions tool: checked in a child process).
#include <cmath>
#include <cstring>

#include <tela/drawing_raster.hpp>

#include "indep_isolate.hpp"
#include "indep_scripted_game.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "snd/tool/ui/safe_drawing.hpp"
#include "snd/tool/ui/widgets.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;
using ui::FaderDbToPos;
using ui::FaderPosToDb;
namespace ll = snd::livelink;

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
  tela::Point C(const std::string& id) const {
    const tela::PlacedElement* p = El(id);
    SND_CHECK(p != nullptr);
    return p != nullptr ? Center(p) : tela::Point{0, 0};
  }
  std::string FaderHit(const char* bus) const { return ui::BusId(snd::HashId(bus)) + ".fader.hit"; }
  // Rounded point on the fader cap for a dB value.
  tela::Point Cap(const char* bus, float db) const {
    const tela::PlacedElement* p = El(FaderHit(bus));
    SND_CHECK(p != nullptr);
    if (p == nullptr) return tela::Point{0, 0};
    const float y = p->bounds.y + ui::FaderGeometry{p->bounds.height, 18.0f}.CapCenterY(FaderDbToPos(db));
    return tela::Point{std::round(p->bounds.x + p->bounds.width * 0.5f), std::round(y)};
  }
  float Get(const Target& t) const { return s.mixer().Get(t); }
  std::size_t Undos() const { return s.mixer().history().UndoCount(); }
};

}  // namespace

// D3: the drag is "the vertical motion from where it was pressed". A press and a purely
// horizontal motion (no vertical motion) must leave the value as it is; here for faders
// at ordinary values (no undo step either).
SND_TEST(IndepToolUi, DragWithoutVerticalMotionKeepsOrdinaryFaderValues) {
  Rig t;
  const Target sfx{Control::BusFader, t.Bus("Sfx"), 0, 0};
  for (float db : {-60.0f, -30.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f}) {
    t.s.mixer().Set(sfx, db);
    t.app.Frame(tooltest::Now());
    const std::size_t u = t.Undos();
    const tela::Point p = t.Cap("Sfx", db);
    t.ptr.Down(p.x, p.y);
    t.ptr.Move(p.x + 3.0f, p.y);
    t.ptr.Move(p.x - 2.0f, p.y);
    t.ptr.Up(p.x - 2.0f, p.y);
    t.ptr.Advance(1000);
    SND_CHECK_EQ(t.Get(sfx), db);
    SND_CHECK_EQ(t.Undos(), u);
  }
}

// The same for a fader at -90 dB (the lowest audible value) or a bank value below -90 dB:
// the law maps both to p = 0, and p = 0 is -inf, so a press with a sideways jitter
// silences the bus.
SND_TEST(IndepToolUi, Defect_fader_press_without_motion_at_minus90_silences) {
  Rig t;
  const Target sfx{Control::BusFader, t.Bus("Sfx"), 0, 0};
  t.s.mixer().Set(sfx, -90.0f);
  t.app.Frame(tooltest::Now());
  const tela::Point p = t.Cap("Sfx", -90.0f);
  t.ptr.Down(p.x, p.y);
  t.ptr.Move(p.x + 2.0f, p.y);
  t.ptr.Up(p.x + 2.0f, p.y);
  SND_CHECK_EQ(t.Get(sfx), -90.0f);
}

// A knob whose value does not survive the value -> norm -> value round trip exactly
// (logarithmic Hz knob) changes on a press with a sideways jitter: an undo step and an
// override row appear although nothing was turned.
SND_TEST(IndepToolUi, Defect_knob_press_without_motion_changes_the_value) {
  Rig t;
  const Target damp{Control::EffectParam, t.Bus("Sfx"), 0, 4};  // delay DampingHz 500..20000, log
  const ui::KnobRange r{500.0f, 20000.0f, true, false};
  float v = 0.0f;
  for (float x = 501.0f; x < 20000.0f; x += 7.3f) {
    if (ui::KnobFromNorm(ui::KnobToNorm(x, r), r) != x) {
      v = x;
      break;
    }
  }
  SND_REQUIRE(v != 0.0f);
  t.s.mixer().Set(damp, v);
  t.app.Frame(tooltest::Now());
  const std::size_t u = t.Undos();
  const tela::Point c = t.C(ui::BusId(snd::HashId("Sfx")) + ".fx0.4.hit");
  const float x = std::round(c.x), y = std::round(c.y);
  t.ptr.Down(x, y);
  t.ptr.Move(x + 3.0f, y);
  t.ptr.Up(x + 3.0f, y);
  SND_CHECK_EQ(t.Get(damp), v);
  SND_CHECK_EQ(t.Undos(), u);
}

// D3: Shift pressed or released mid-drag (no motion at that moment) does not move the
// value; afterwards the motion counts 1/10 (knob: 150 px = whole range).
SND_TEST(IndepToolUi, KnobShiftToggleMidDragDoesNotJump) {
  Rig t;
  const Target dry{Control::EffectParam, t.Bus("Sfx"), 0, 2};  // -96 .. 0 dB, linear, bank 0
  const tela::Point c = t.C(ui::BusId(snd::HashId("Sfx")) + ".fx0.2.hit");
  const float x = std::round(c.x), y = std::round(c.y);
  t.ptr.Down(x, y);
  t.ptr.Move(x, y + 50.0f);  // -32 dB
  SND_CHECK_NEAR(t.Get(dry), -32.0f, 1e-3);
  t.ptr.Move(x, y + 50.0f, ui::kModShift);
  SND_CHECK_NEAR(t.Get(dry), -32.0f, 1e-3);
  t.ptr.Move(x, y + 80.0f, ui::kModShift);  // 30 px fine = 1.92 dB
  SND_CHECK_NEAR(t.Get(dry), -33.92f, 1e-3);
  t.ptr.Move(x, y + 80.0f);
  SND_CHECK_NEAR(t.Get(dry), -33.92f, 1e-3);
  t.ptr.Move(x, y + 30.0f);  // 50 px up, full speed: +32 dB
  SND_CHECK_NEAR(t.Get(dry), -1.92f, 1e-3);
  t.ptr.Up(x, y + 30.0f);
  SND_CHECK(t.s.mixer().Undo());  // one drag, one step
  SND_CHECK_EQ(t.Get(dry), 0.0f);
}

// D4: double click = the same control pressed again within 400 ms and 4 px (both
// inclusive); 5 px or another control is no double click.
SND_TEST(IndepToolUi, DoubleClickLimitsAreInclusiveAndPerControl) {
  Rig t;
  const Target sfx{Control::BusFader, t.Bus("Sfx"), 0, 0};
  const Target mus{Control::BusFader, t.Bus("Music"), 0, 0};
  auto prepare = [&] {
    t.s.mixer().Set(sfx, -20.0f);
    t.s.mixer().Set(mus, -20.0f);
    t.app.Frame(tooltest::Now());
    t.ptr.Advance(5000);
  };
  prepare();
  tela::Point p = t.Cap("Sfx", -20.0f);
  t.ptr.Down(p.x, p.y);
  t.ptr.Up(p.x, p.y);
  t.ptr.Advance(400);
  t.ptr.Down(p.x + 4.0f, p.y - 4.0f);
  t.ptr.Up(p.x + 4.0f, p.y - 4.0f);
  SND_CHECK_EQ(t.Get(sfx), 0.0f);  // 400 ms, 4 px: double click
  prepare();
  t.ptr.Down(p.x, p.y);
  t.ptr.Up(p.x, p.y);
  t.ptr.Advance(100);
  t.ptr.Down(p.x + 5.0f, p.y);
  t.ptr.Up(p.x + 5.0f, p.y);
  SND_CHECK_EQ(t.Get(sfx), -20.0f);  // 5 px: no
  prepare();
  const tela::Point q = t.Cap("Music", -20.0f);
  t.ptr.Down(p.x, p.y);
  t.ptr.Up(p.x, p.y);
  t.ptr.Advance(100);
  t.ptr.Down(q.x, q.y);
  t.ptr.Up(q.x, q.y);
  SND_CHECK_EQ(t.Get(sfx), -20.0f);  // another control: no
  SND_CHECK_EQ(t.Get(mus), -20.0f);
}

// C2: wheel turns merge per control while they follow each other; another control between
// them starts a new step.
SND_TEST(IndepToolUi, WheelOnAnotherControlStartsANewStep) {
  Rig t;
  const tela::Point a = t.Cap("Sfx", 0.0f), b = t.Cap("Music", 0.0f);
  const std::size_t u = t.Undos();
  t.ptr.Wheel(a.x, a.y, 1.0f);
  t.ptr.Wheel(a.x, a.y, 1.0f);
  t.ptr.Wheel(b.x, b.y, -1.0f);
  t.ptr.Wheel(a.x, a.y, 1.0f);
  SND_CHECK_EQ(t.Undos(), u + 3u);
  SND_CHECK_NEAR(t.Get(Target{Control::BusFader, t.Bus("Sfx"), 0, 0}), 3.0f, 1e-5);
  t.s.mixer().Undo();
  SND_CHECK_NEAR(t.Get(Target{Control::BusFader, t.Bus("Sfx"), 0, 0}), 2.0f, 1e-5);
}

// B10 / C2: a parameter knob turns its override on with the value as one step; wheel turns
// on it merge into one step like on any other control.
SND_TEST(IndepToolUi, ParameterKnobDragIsOneStepWithTheOverride) {
  Rig t;
  const std::int32_t knob = t.tool.project.FindParameter(snd::HashId("Knob"));
  SND_REQUIRE(knob >= 0);
  const Target val{Control::ParamOverride, knob, 0, 0}, on{Control::ParamOverrideOn, knob, 0, 0};
  const tela::Point c = t.C(ui::ParameterId(snd::HashId("Knob")) + ".hit");
  const float x = std::round(c.x), y = std::round(c.y);
  const std::size_t u = t.Undos();
  t.ptr.Down(x, y);
  t.ptr.Move(x, y - 15.0f);  // +0.1 of 0..1
  t.ptr.Move(x, y - 30.0f);
  t.ptr.Up(x, y - 30.0f);
  SND_CHECK_NEAR(t.Get(val), 0.45f, 1e-5);
  SND_CHECK_EQ(t.Get(on), 1.0f);
  SND_CHECK_EQ(t.Undos(), u + 1u);
  SND_CHECK(t.s.mixer().Undo());
  SND_CHECK_EQ(t.Get(on), 0.0f);
  SND_CHECK_NEAR(t.Get(val), 0.25f, 1e-6);
}

SND_TEST(IndepToolUi, Defect_parameter_knob_wheel_is_two_undo_steps) {
  Rig t;
  const std::int32_t knob = t.tool.project.FindParameter(snd::HashId("Knob"));
  SND_REQUIRE(knob >= 0);
  const tela::Point c = t.C(ui::ParameterId(snd::HashId("Knob")) + ".hit");
  const std::size_t u = t.Undos();
  for (int i = 0; i < 3; ++i) t.ptr.Wheel(std::round(c.x), std::round(c.y), 1.0f);
  SND_CHECK_NEAR(t.Get(Target{Control::ParamOverride, knob, 0, 0}), 0.28f, 1e-5);
  SND_CHECK_EQ(t.Undos(), u + 1u);  // one wheel series = one step (with the override on)
}

// --- F: SafeDrawing at Tela's limits ---------------------------------------------------------

SND_TEST(IndepToolUi, SafeDrawingStopsExactlyAtTelasLimits) {
  // 256 shapes pass, the 257th is refused (Tela would throw).
  ui::SafeDrawing d;
  for (int i = 0; i < 256; ++i) SND_CHECK(d.Rectangle(tela::Rect{0, 0, 1, 1}, tela::Color{1, 2, 3, 255}));
  SND_CHECK(!d.Rectangle(tela::Rect{0, 0, 1, 1}, tela::Color{1, 2, 3, 255}));
  SND_CHECK_EQ(d.drawing().shapes().size(), 256u);
  SND_CHECK_EQ(d.dropped(), 1u);
  // Points: up to 4096 in all.
  ui::SafeDrawing q;
  std::vector<tela::Point> pts;
  for (int i = 0; i < 8; ++i) pts.push_back(tela::Point{static_cast<float>(i % 2), static_cast<float>(i % 3)});
  std::uint32_t added = 0;
  for (int i = 0; i < 600; ++i) {
    if (q.Polyline(pts, tela::Stroke{{255, 255, 255, 255}, 1})) added += 8;
  }
  std::size_t total = 0;
  for (const tela::Shape& s : q.drawing().shapes()) total += s.points.size();
  SND_CHECK(total <= 4096u);
  SND_CHECK(q.drawing().shapes().size() <= 256u);
  SND_CHECK(added == total);
  // Refused input: non-finite, out of range, negative sizes, dashes on a rectangle,
  // too few points.
  ui::SafeDrawing r;
  SND_CHECK(!r.Rectangle(tela::Rect{NAN, 0, 1, 1}, tela::Color{}));
  SND_CHECK(!r.Rectangle(tela::Rect{0, 0, -1, 1}, tela::Color{}));
  SND_CHECK(!r.Rectangle(tela::Rect{2e6f, 0, 1, 1}, tela::Color{}));
  SND_CHECK(!r.Rectangle(tela::Rect{0, 0, 1, 1}, tela::Color{}, tela::Stroke{{}, 1, 2, 2}));
  SND_CHECK(!r.Ellipse(tela::Rect{0, 0, INFINITY, 1}, tela::Color{}));
  SND_CHECK(!r.Polyline({tela::Point{0, 0}}, tela::Stroke{{}, 1}));
  SND_CHECK(!r.Polyline({tela::Point{0, 0}, tela::Point{NAN, 1}}, tela::Stroke{{}, 1}));
  SND_CHECK(!r.Line(tela::Point{0, 0}, tela::Point{1, 1}, tela::Stroke{{}, -1}));
  SND_CHECK_EQ(r.drawing().shapes().size(), 0u);
}

namespace {

// Tela's raster work for one drawing (drawing_raster.cpp paint_drawing): for each shape the
// clipped pixel box (bounds padded by stroke / 2 + glow + 1 / scale) times its points.
double TelaWork(const tela::Drawing& d, float scale, float clipW, float clipH) {
  double work = 0.0;
  for (const tela::Shape& s : d.shapes()) {
    float x0 = s.bounds.x, y0 = s.bounds.y, x1 = s.bounds.x + s.bounds.width, y1 = s.bounds.y + s.bounds.height;
    if (s.kind == tela::ShapeKind::polyline && !s.points.empty()) {
      x0 = x1 = s.points[0].x;
      y0 = y1 = s.points[0].y;
      for (const tela::Point& p : s.points) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
      }
    }
    const float pad = s.stroke.width / 2 + s.stroke.glow.radius + 1.0f / scale;
    const float bx0 = std::max(0.0f, (x0 - pad) * scale), by0 = std::max(0.0f, (y0 - pad) * scale);
    const float bx1 = std::min(clipW * scale, (x1 + pad) * scale), by1 = std::min(clipH * scale, (y1 + pad) * scale);
    const double w = std::max(0.0f, bx1 - bx0), h = std::max(0.0f, by1 - by0);
    work += (std::ceil(w) + 1.0) * (std::ceil(h) + 1.0) * static_cast<double>(std::max<std::size_t>(1, s.points.size()));
  }
  return work;
}

std::vector<float> Spiky(std::size_t n, float hi) {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = (i % 2 == 0) ? 0.0f : hi;
  return v;
}

}  // namespace

// F3: the timing graph with the worst data (every other block at the maximum) stays within
// Tela's raster budget at every DPI scale the tool accepts. The headless host takes --dpi up
// to 4 and draws without exceptions; App::SetViewport takes up to 8.
SND_TEST(IndepToolUi, TimingGraphWorkAtDpi2IsWithinTelasBudget) {
  ui::Palette pal = ui::MakePalette(tela::Theme{});
  ui::GraphLook g;
  g.width = 8000.0f;  // a 16384 px wide viewport at DPI 2
  g.height = 160.0f;
  g.total = Spiky(240, 1000.0f);
  g.render = Spiky(240, 900.0f);
  g.budget = 500.0f;
  const tela::Drawing d = ui::DrawTimingGraph(g, pal);
  SND_CHECK(TelaWork(d, 2.0f, g.width, g.height) <= 64.0e6);
}

SND_TEST(IndepToolUi, Defect_timing_graph_exceeds_telas_budget_at_dpi4) {
  ui::Palette pal = ui::MakePalette(tela::Theme{});
  ui::GraphLook g;
  g.width = 2000.0f;  // the graph of an 8192 x 2048 viewport at DPI 4 (2048 logical wide)
  g.height = 160.0f;
  g.total = Spiky(240, 1000.0f);
  g.render = Spiky(240, 900.0f);
  g.budget = 500.0f;
  const tela::Drawing d = ui::DrawTimingGraph(g, pal);
  const double work = TelaWork(d, 4.0f, g.width, g.height);
  SND_CHECK_MSG(work <= 64.0e6, "Tela work %.0f at DPI 4", work);
  std::printf("timing graph 2000 x 160 logical at DPI 4: Tela work %.0f (budget 64000000)\n", work);
  // The real thing: the profiler screen of a connected tool at --dpi 4 (headless limit),
  // drawn by the CPU compositor in a child process.
  tooltest::ToolProject tool;
  indep::ScriptedGame game;
  SND_REQUIRE(tool.ok && game.ok);
  Session s;
  s.UseProject(tool.project);
  SND_REQUIRE_EQ(s.Connect(indep::GameConn(game, tool.project.buildHash())), SND_OK);
  SND_REQUIRE(indep::Drive(s, game, [&] { return s.connection().state() == LinkState::Online; }));
  ll::Client* c = s.connection().client();
  for (std::uint32_t i = 0; i < 240; ++i) {
    ll::BlockTiming bt{};
    bt.blockIndex = i;
    bt.logicMicroseconds = 10.0f;
    bt.renderMicroseconds = (i % 2 == 0) ? 1.0f : 5000.0f;
    bt.budgetMicroseconds = 2000.0f;
    game.Send(ll::MessageType::BlockTiming, &bt, sizeof bt);
  }
  SND_REQUIRE(indep::Drive(s, game, [&] { return c->Received(ll::MessageType::BlockTiming) >= 240u; }));
  s.Disconnect();  // no network thread across fork
  ui::App app(s, ui::AppDesc{});
  app.OnScreen(ui::Screen::Profiler);
  app.SetViewport(8192, 2048, 4.0f);
  app.Declare(indep::NowNs());
  SND_REQUIRE(s.profiler().timings().size() >= 240u);
  const int r = indep::Isolated([&] {
    tela::PixelSurface px;
    return ui::Compose(app.runtime(), ui::ComposeOptions{}, &px) ? 0 : 3;
  });
  SND_CHECK_MSG(r == 0 || r == indep::kNotIsolated, "compose at DPI 4: child result %d (abort = %d)", r,
                indep::kSignalBase + 6);
}

// F3: the whole tool (mixer with meters, profiler) composes at DPI 1, 2 and with odd
// viewports (tiny, very wide) without a Tela exception.
SND_TEST(IndepToolUi, ScreensComposeAtOrdinaryDpiAndOddViewports) {
  Rig t;
  struct V {
    int w, h;
    float dpi;
  };
  for (const V v : {V{1280, 800, 1.0f}, V{2560, 1600, 2.0f}, V{1, 1, 1.0f}, V{16384, 300, 1.0f}, V{300, 16384, 2.0f},
                    V{640, 480, 0.25f}}) {
    t.app.SetViewport(v.w, v.h, v.dpi);
    for (ui::Screen sc : {ui::Screen::Mixer, ui::Screen::Profiler}) {
      t.app.OnScreen(sc);
      t.app.Frame(tooltest::Now());
      const int r = indep::Isolated([&] {
        tela::PixelSurface px;
        return ui::Compose(t.app.runtime(), ui::ComposeOptions{true}, &px) ? 0 : 3;
      });
      SND_CHECK_MSG(r == 0 || r == indep::kNotIsolated, "viewport %dx%d dpi %g screen %d: child %d", v.w, v.h,
                    static_cast<double>(v.dpi), static_cast<int>(sc), r);
    }
  }
}

// E3: the timing graph shows the last 240 blocks; the vertical scale is
// max(1.25 x budget, 1.1 x maximum).
SND_TEST(IndepToolUi, TimingGraphShowsTheLast240Blocks) {
  ui::Palette pal = ui::MakePalette(tela::Theme{});
  ui::GraphLook g;
  g.width = 479.0f;
  g.height = 100.0f;
  for (int i = 0; i < 300; ++i) {
    g.total.push_back(static_cast<float>(i));
    g.render.push_back(static_cast<float>(i) * 0.5f);
  }
  g.budget = 100.0f;
  ui::GraphShape shape;
  ui::DrawTimingGraph(g, pal, &shape);
  SND_REQUIRE_EQ(shape.totalPoints.size(), 240u);
  SND_CHECK_NEAR(shape.scale.yMax, 1.1f * 299.0f, 1e-3);
  // oldest shown = block 60 at the left, newest = 299 at the top right
  SND_CHECK_NEAR(shape.totalPoints.front().x, 0.0f, 1e-3);
  SND_CHECK_NEAR(shape.totalPoints.back().x, 479.0f, 1e-3);
  SND_CHECK_NEAR(shape.totalPoints.front().y, 100.0f - 60.0f / (1.1f * 299.0f) * 100.0f, 1e-2);
  SND_CHECK_NEAR(shape.totalPoints.back().y, 100.0f - 299.0f / (1.1f * 299.0f) * 100.0f, 1e-2);
  SND_CHECK_NEAR(shape.budgetY, 100.0f - 100.0f / (1.1f * 299.0f) * 100.0f, 1e-2);
}
