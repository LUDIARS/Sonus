// The parameters and curves screen: parameter members and the follow speed, the curve plot (point
// drags in value units, one undo step; double click adds; Delete removes, two points stay), target /
// compose / input, and the composition check against ComposeEvent.
#include <cmath>

#include "snd/test/test.hpp"
#include "snd/tool/curve_editor.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {
const std::string kShot = doctest::G(8), kSpeed = doctest::G(6);
}

SND_TEST(ToolUiParams, ParameterMembersAndRamp) {
  Editor t(ui::Screen::Params);
  SND_CHECK(t.app.view().params.parameter == kSpeed);
  for (const char* id : {"pc.par.switch", "pc.par.scope0", "pc.par.min", "pc.par.max", "pc.par.def", "pc.par.ramp",
                         "pc.par.ramp.graph", "pc.par.ramp.text"}) {
    SND_CHECK_MSG(t.Has(id), "missing %s", id);
  }
  // Ramp 5 units/s over 0..10 at 256 / 48000 s per block: 375 blocks -> "> 200 blocks".
  SND_CHECK(t.Label("pc.par.ramp.text").find("> 200") != std::string::npos);
  ParameterEditor pe(t.doc, kSpeed);
  SND_REQUIRE(t.Click("pc.par.ramp.inc"));  // + 0.5
  SND_CHECK_NEAR(pe.rampRate(), 5.5, 1e-9);
  SND_CHECK_EQ(pe.scope(), 7u);  // no "scope" member: every scope
  SND_REQUIRE(t.Click("pc.par.scope1"));
  SND_CHECK_EQ(pe.scope(), 5u);
  SND_REQUIRE(t.Click("pc.par.scope0"));
  SND_REQUIRE(t.Click("pc.par.scope2"));  // the last scope cannot go
  SND_CHECK_EQ(pe.scope(), 4u);
  SND_REQUIRE(t.Click("pc.par.def"));
  t.ClearEntry();
  t.Type("4");
  SND_CHECK_EQ(pe.defaultValue(), 4.0);
}

SND_TEST(ToolUiParams, CurvePointsDragAddAndRemove) {
  Editor t(ui::Screen::Params);
  ui::ParamsView& pv = t.app.view().params;
  SND_CHECK(pv.event == kShot && pv.curve == "curves[0]");
  SND_CHECK(t.Has("pc.curve.0") && t.Has("pc.curve.1"));  // pitch curve and the distance volume curve
  CurveEditor ce(t.doc, kShot, "curves[0]");
  SND_REQUIRE_EQ(ce.Points().size(), 2u);
  const tela::PlacedElement* plot = t.El("pc.plot");
  SND_REQUIRE(plot != nullptr);
  // The plot's axes: x 0..10 (Speed's range), y 0..1200 with 12 % margin.
  const ui::PlotAxes axes =
      ui::PlotAxes::Fit(0.0, 10.0, 0.0, 1200.0, plot->bounds.width, plot->bounds.height, 0.12, ui::kCurveXMargin);
  const std::size_t undo0 = t.doc.UndoCount();
  t.Drag("pc.pt.1", 0.0f, 30.0f, 3);
  const std::vector<CurvePoint> p = ce.Points();
  SND_CHECK_NEAR(p[1].x, 10.0, 1e-3);
  SND_CHECK_NEAR(p[1].y, axes.ValueY(axes.Y(1200.0) + 30.0f), 1e-2);
  SND_CHECK_EQ(t.doc.UndoCount(), undo0 + 1u);
  SND_CHECK_EQ(pv.point, 1);
  // Double click on the plot at x = 5 (the plot rescaled to the moved point: new axes).
  plot = t.El("pc.plot");
  SND_REQUIRE(plot != nullptr);
  const ui::PlotAxes axes2 = ui::PlotAxes::Fit(0.0, 10.0, 0.0, ce.Points()[1].y, plot->bounds.width, plot->bounds.height, 0.12,
                                                ui::kCurveXMargin);
  const tela::Point at{plot->bounds.x + axes2.X(5.0), plot->bounds.y + axes2.Y(300.0)};
  t.ptr.Down(at.x, at.y);
  t.ptr.Up(at.x, at.y);
  t.ptr.Down(at.x, at.y);
  t.ptr.Up(at.x, at.y);
  t.ptr.Advance(1000);
  t.Frame();
  SND_REQUIRE_EQ(ce.Points().size(), 3u);
  SND_CHECK_NEAR(ce.Points()[1].x, 5.0, 0.05);
  SND_CHECK_NEAR(ce.Points()[1].y, 300.0, 6.0);
  SND_CHECK_EQ(pv.point, 1);
  // Shape of the selected point, then Delete.
  SND_REQUIRE(t.Click("pc.pt.shape.inc"));
  SND_CHECK_NEAR(ce.Points()[1].shape, 0.1, 1e-9);
  t.Key(ui::kKeyDelete);
  SND_CHECK_EQ(ce.Points().size(), 2u);
  SND_CHECK_EQ(pv.point, -1);
  SND_REQUIRE(t.Click("pc.pt.0"));  // select (a press)
  SND_CHECK_EQ(pv.point, 0);
  t.Key(ui::kKeyDelete);  // refused: a curve keeps two points
  SND_CHECK_EQ(ce.Points().size(), 2u);
  // Target and compose.
  SND_REQUIRE(t.Pick("pc.c.target", "volume"));
  SND_CHECK(ce.target() == CurveTarget::Volume);
  SND_REQUIRE(t.Pick("pc.c.compose", "multiply"));
  SND_CHECK(ce.ComposeIgnored());
  SND_CHECK(t.Has("pc.c.warn"));
}

SND_TEST(ToolUiParams, CompositionMatchesTheModel) {
  Editor t(ui::Screen::Params);
  const std::string field = "pc.cmp.v." + Key(kSpeed);
  SND_REQUIRE(t.Has(field));
  SND_REQUIRE(t.Click(field + ".inc"));  // default 1 + 0.2
  std::map<std::string, ParamScopeValues> v;
  v[kSpeed].hasGlobal = true;
  v[kSpeed].global = 1.2;
  const ComposeResult r = ComposeEvent(t.doc, kShot, v, 10.0);
  char b[64];
  std::snprintf(b, sizeof b, "pitch %+.1f ct", r.pitchCents);
  SND_CHECK(t.Label("pc.cmp.r0").find(b) != std::string::npos);
  SND_CHECK_NEAR(r.pitchCents, 144.0, 1e-3);
  // The distance marker on the distance curve.
  SND_REQUIRE(t.Click("pc.curve.1"));
  SND_CHECK(t.app.view().params.curve == "distanceCurves.volume");
  SND_REQUIRE(t.Click("pc.cmp.dist.inc"));
  SND_CHECK_EQ(t.app.view().params.distance, 11.0);
  // A new curve on the selected parameter.
  SND_REQUIRE(t.Pick("pc.curve.add", "lowpass"));
  SND_CHECK(t.app.view().params.curve == "curves[1]");
  SND_CHECK(CurveEditor(t.doc, kShot, "curves[1]").target() == CurveTarget::Lowpass);
}

SND_TEST(ToolUiParams, CurvePointsAreDrawnAtTheirValues) {
  for (float dpi : {1.0f, 2.0f}) {
    Editor t(ui::Screen::Params, static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    const tela::PlacedElement* plot = t.El("pc.plot");
    SND_REQUIRE(plot != nullptr);
    tela::PixelSurface img;
    SND_REQUIRE(ui::Compose(t.app.runtime(), ui::ComposeOptions{}, &img));
    const ui::PlotAxes axes =
        ui::PlotAxes::Fit(0.0, 10.0, 0.0, 1200.0, plot->bounds.width, plot->bounds.height, 0.12, ui::kCurveXMargin);
    const tela::Color accent = t.app.palette().accent;
    for (const auto& [x, y] : {std::pair<double, double>{0.0, 0.0}, {10.0, 1200.0}}) {
      unsigned char rgb[3];
      ui::PixelRgb(img, static_cast<int>((plot->bounds.x + axes.X(x) + (x == 0.0 ? 1.0f : -1.0f)) * dpi),
                   static_cast<int>((plot->bounds.y + axes.Y(y)) * dpi), rgb);
      SND_CHECK_MSG(std::abs(rgb[0] - accent.r) <= 4 && std::abs(rgb[1] - accent.g) <= 4 && std::abs(rgb[2] - accent.b) <= 4,
                    "point (%g, %g) at dpi %g: %d %d %d", x, y, static_cast<double>(dpi), rgb[0], rgb[1], rgb[2]);
    }
  }
}
