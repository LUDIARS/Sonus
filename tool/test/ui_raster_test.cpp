// Pixels drawn with Tela::Raster (raster_compositor.hpp): meter bars are as high
// as their value on the meter scale, the timing graph's points and budget line
// are where control_math.hpp puts them, the fader cap sits at its position, and
// in the full tool the master meter shows the level the game rendered.
#include <algorithm>
#include <cmath>

#include <tela/document.hpp>

#include "snd/test/test.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "snd/tool/ui/safe_drawing.hpp"
#include "snd/tool/ui/widgets.hpp"
#include "ui_helpers.hpp"

using namespace snd::tool;
using namespace uitest;

namespace {

constexpr float kX = 10.0f, kY = 20.0f;  // where the canvas under test is placed

bool Near(const unsigned char rgb[3], tela::Color c, int tol = 2) {
  return std::abs(rgb[0] - c.r) <= tol && std::abs(rgb[1] - c.g) <= tol && std::abs(rgb[2] - c.b) <= tol;
}

// A runtime holding one canvas at (kX, kY), composed at DPI `scale`.
tela::PixelSurface Render(tela::Drawing d, float w, float h, float scale = 1.0f) {
  tela::Runtime rt;
  tela::Viewport v;
  v.host_id = "test";
  v.view_id = "raster";
  v.revision = 1;
  v.width = static_cast<int>((kX + w + 10.0f) * scale);
  v.height = static_cast<int>((kY + h + 10.0f) * scale);
  v.dpi_scale = scale;
  v.visible = v.focused = true;
  rt.viewport(v);
  tela::Document doc;
  doc.canvas("c", std::move(d), ui::At(kX, kY, w, h));
  rt.document(std::move(doc));
  tela::PixelSurface s;
  ui::ComposeOptions o;
  ui::Compose(rt, o, &s);
  return s;
}

// First row (top-down) in column x within [y0, y1) whose pixel is near `c`; -1 if none.
int FirstRow(const tela::PixelSurface& s, int x, int y0, int y1, tela::Color c) {
  unsigned char rgb[3];
  for (int y = y0; y < y1; ++y) {
    if (ui::PixelRgb(s, x, y, rgb) && Near(rgb, c)) return y;
  }
  return -1;
}

}  // namespace

SND_TEST(ToolUiRaster, MeterBarsAreAsHighAsTheirValue) {
  const ui::Palette p = ui::MakePalette(tela::Theme{});
  for (float scale : {1.0f, 2.0f}) {
    ui::MeterLook l;
    l.width = 30;
    l.height = 200;
    l.channels = 2;
    l.peakDb[0] = -6.0f;   // f 0.9
    l.rmsDb[0] = -12.0f;   // f 0.8
    l.holdDb[0] = -3.0f;   // f 0.95
    l.peakDb[1] = -30.0f;  // f 0.5
    l.rmsDb[1] = -42.0f;   // f 0.3
    l.holdDb[1] = -18.0f;  // f 0.7
    ui::MeterShape shape;
    const tela::PixelSurface s = Render(ui::DrawMeter(l, p, &shape), l.width, l.height, scale);
    SND_REQUIRE_EQ(shape.bars.size(), 2u);
    for (std::uint32_t c = 0; c < 2; ++c) {
      const ui::MeterShape::Bar& b = shape.bars[c];
      SND_CHECK_NEAR(b.peakTop, l.height * (1.0f - ui::MeterFraction(l.peakDb[c])), 1e-3);
      SND_CHECK_NEAR(b.rmsTop, l.height * (1.0f - ui::MeterFraction(l.rmsDb[c])), 1e-3);
      SND_CHECK_NEAR(b.holdY, l.height * (1.0f - ui::MeterFraction(l.holdDb[c])), 1e-3);
      // In pixels: the edge of the bar's zone colour (left edge column, outside the RMS inset).
      const int x = static_cast<int>((kX + b.x + 0.5f) * scale);
      const int top = static_cast<int>(kY * scale), bottom = static_cast<int>((kY + l.height) * scale);
      const int peakRow = FirstRow(s, x, top + static_cast<int>((b.holdY + 2.0f) * scale), bottom,
                                   ui::MeterZoneColor(l.peakDb[c], p));
      SND_CHECK_MSG(std::abs(peakRow - (kY + b.peakTop) * scale) <= 1.0f, "channel %u peak edge %d, expected %.1f", c,
                    peakRow, static_cast<double>((kY + b.peakTop) * scale));
      const int xm = static_cast<int>((kX + b.x + b.width * 0.5f) * scale);
      const int rmsRow = FirstRow(s, xm, top + static_cast<int>((b.holdY + 2.0f) * scale), bottom, p.rmsFill);
      SND_CHECK_MSG(std::abs(rmsRow - (kY + b.rmsTop) * scale) <= 1.0f, "channel %u rms edge %d", c, rmsRow);
      const int holdRow = FirstRow(s, xm, top, bottom, p.text);
      SND_CHECK_MSG(std::abs(holdRow - (kY + b.holdY - 1.0f) * scale) <= 1.0f, "channel %u hold %d", c, holdRow);
      // Bar height / canvas height = the meter fraction.
      const float h = static_cast<float>(bottom - peakRow) / (l.height * scale);
      SND_CHECK_NEAR(h, ui::MeterFraction(l.peakDb[c]), 1.5f / (l.height * scale));
    }
  }
}

SND_TEST(ToolUiRaster, TimingGraphPointsAndBudgetLine) {
  const ui::Palette p = ui::MakePalette(tela::Theme{});
  ui::GraphLook g;
  g.width = 300;
  g.height = 100;
  for (int i = 0; i < 100; ++i) {
    g.total.push_back(static_cast<float>(i));
    g.render.push_back(0.5f * static_cast<float>(i));
  }
  g.budget = 80.0f;
  ui::GraphShape shape;
  const tela::PixelSurface s = Render(ui::DrawTimingGraph(g, p, &shape), g.width, g.height);
  const float yMax = 1.1f * 99.0f;  // max(1.25 * 80, 1.1 * 99, 1)
  SND_CHECK_NEAR(shape.scale.yMax, yMax, 1e-3);
  SND_REQUIRE_EQ(shape.totalPoints.size(), 100u);
  for (std::uint32_t i = 0; i < 100; ++i) {
    SND_CHECK_NEAR(shape.totalPoints[i].x, static_cast<float>(i) * 300.0f / 99.0f, 1e-3);
    SND_CHECK_NEAR(shape.totalPoints[i].y, 100.0f - static_cast<float>(i) / yMax * 100.0f, 1e-3);
  }
  SND_CHECK_NEAR(shape.budgetY, 100.0f - 80.0f / yMax * 100.0f, 1e-3);
  // Pixels: the series colour on its points, the budget colour on its row (in a dash).
  unsigned char rgb[3];
  for (std::uint32_t i : {10u, 50u, 90u}) {
    const tela::Point q = shape.totalPoints[i];
    SND_CHECK(ui::PixelRgb(s, static_cast<int>(kX + q.x), static_cast<int>(kY + q.y), rgb));
    SND_CHECK_MSG(Near(rgb, p.accent, 60), "point %u: %d %d %d", i, rgb[0], rgb[1], rgb[2]);
  }
  SND_CHECK(ui::PixelRgb(s, static_cast<int>(kX + 3.0f), static_cast<int>(kY + shape.budgetY), rgb));
  SND_CHECK_MSG(Near(rgb, p.budget, 60), "budget: %d %d %d", rgb[0], rgb[1], rgb[2]);
  // Above every series and line: the canvas background.
  SND_CHECK(ui::PixelRgb(s, static_cast<int>(kX + 280.0f), static_cast<int>(kY + 2.0f), rgb));
  SND_CHECK(Near(rgb, p.canvas));
  // More samples than kGraphMaxPoints: the newest ones are drawn.
  ui::GraphLook many = g;
  many.total.assign(1000, 5.0f);
  many.total.back() = 50.0f;
  ui::DrawTimingGraph(many, p, &shape);
  SND_CHECK_EQ(shape.totalPoints.size(), ui::kGraphMaxPoints);
  SND_CHECK_NEAR(shape.totalPoints.back().y, 100.0f - 50.0f / shape.scale.yMax * 100.0f, 1e-3);
}

SND_TEST(ToolUiRaster, FaderCapSitsAtItsPosition) {
  const ui::Palette p = ui::MakePalette(tela::Theme{});
  for (float db : {12.0f, 0.0f, -12.0f, -60.0f, kFaderOffDb}) {
    ui::FaderLook l;
    l.width = 40;
    l.height = 300;
    l.pos = ui::FaderDbToPos(db);
    ui::FaderShape shape;
    const tela::PixelSurface s = Render(ui::DrawFader(l, p, &shape), l.width, l.height);
    const float expected = 9.0f + (1.0f - l.pos) * 282.0f;
    SND_CHECK_NEAR(shape.capCenter, expected, 1e-3);
    unsigned char rgb[3];
    // The 1 px centre line (theme text colour) at the cap's centre: brighter than the cap there.
    int brightest = 0;
    for (int dy = -1; dy <= 0; ++dy) {
      ui::PixelRgb(s, static_cast<int>(kX + 8.0f), static_cast<int>(kY + expected) + dy, rgb);
      brightest = std::max(brightest, rgb[0] + rgb[1] + rgb[2]);
    }
    SND_CHECK_MSG(brightest > p.accent.r + p.accent.g + p.accent.b + 60, "cap line at %.1f",
                  static_cast<double>(expected));
    ui::PixelRgb(s, static_cast<int>(kX + 8.0f), static_cast<int>(kY + expected - 5.0f), rgb);
    SND_CHECK(Near(rgb, p.accent, 3));
  }
}

SND_TEST(ToolUiRaster, ComposeRefusesWhatTelaWouldRefuse) {
  tela::Runtime rt;  // no viewport
  tela::PixelSurface s;
  SND_CHECK(!ui::Compose(rt, ui::ComposeOptions{}, &s));
  // The drawing builder refuses instead of letting Tela throw.
  ui::SafeDrawing d;
  SND_CHECK(!d.Rectangle(tela::Rect{0, 0, -1, 4}, tela::Color{}));
  SND_CHECK(!d.Rectangle(tela::Rect{NAN, 0, 1, 4}, tela::Color{}));
  SND_CHECK(!d.Polyline({tela::Point{0, 0}}, tela::Stroke{}));
  SND_CHECK(!d.Polyline({tela::Point{0, 0}, tela::Point{1, 1}}, tela::Stroke{tela::Color{}, 1.0f, 3.0f, 0.0f}));
  for (int i = 0; i < 300; ++i) d.Rectangle(tela::Rect{0, 0, 2, 2}, tela::Color{});
  SND_CHECK_EQ(d.shapes(), ui::SafeDrawing::kMaxShapes);
  SND_CHECK_EQ(d.dropped(), 4u + 300u - ui::SafeDrawing::kMaxShapes);
  ui::SafeDrawing big;
  SND_CHECK(big.Rectangle(tela::Rect{0, 0, 1000, 1000}, tela::Color{}));
  // Both series of a full graph fit (kGraphMaxPoints each, spikes to the full height).
  ui::GraphLook g;
  g.width = 1600;
  g.height = 400;
  for (std::uint32_t i = 0; i < 2 * ui::kGraphMaxPoints; ++i) {
    g.total.push_back(i % 2 == 0 ? 0.0f : 1000.0f);
    g.render.push_back(i % 2 == 0 ? 1000.0f : 0.0f);
  }
  ui::GraphShape gs;
  const tela::Drawing gd = ui::DrawTimingGraph(g, ui::MakePalette(tela::Theme{}), &gs);
  std::size_t points = 0;
  for (const tela::Shape& sh : gd.shapes()) points += sh.points.size();
  SND_CHECK(points >= 2u * ui::kGraphMaxPoints);
  SND_CHECK(!big.Rectangle(tela::Rect{0, 0, 4000, 4000}, tela::Color{}));  // work budget (over 60M at DPI 2)
  std::vector<tela::Point> line;
  for (int i = 0; i < 1000; ++i) line.push_back(tela::Point{static_cast<float>(i), static_cast<float>(i % 7)});
  ui::SafeDrawing poly;
  SND_CHECK(poly.Polyline(line, tela::Stroke{}));
  SND_CHECK_EQ(poly.shapes(), 143u);  // 1000 points in pieces of 8 sharing their ends: ceil(999 / 7)
  tela::PixelSurface ok = Render(poly.Take(), 1000, 10);
  SND_CHECK_EQ(ok.width, 1020);
}
