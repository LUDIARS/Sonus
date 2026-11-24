// Pixels of the editor drawings (CPU compositor, DPI 1 and 2): the cone of the 3D view along the
// emitter's facing, a pair table cell and a time line marker where the values put them, and the
// drawings' own geometry (DrawTopDown, DrawLane, DrawMarks) against control_math.
#include <cmath>

#include <tela/drawing_raster.hpp>

#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/ui/editor_draw.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

namespace {

bool Near(const unsigned char rgb[3], tela::Color c, int tol) {
  return std::abs(rgb[0] - c.r) <= tol && std::abs(rgb[1] - c.g) <= tol && std::abs(rgb[2] - c.b) <= tol;
}

// Some pixel of the 3 x 3 around (x, y) is nearer to `c` than to `background` (an antialiased line).
bool LineAt(const tela::PixelSurface& s, int x, int y, tela::Color c, tela::Color background) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      unsigned char rgb[3];
      if (!snd::tool::ui::PixelRgb(s, x + dx, y + dy, rgb)) continue;
      auto dist = [&](tela::Color k) {
        return std::abs(rgb[0] - k.r) + std::abs(rgb[1] - k.g) + std::abs(rgb[2] - k.b);
      };
      if (dist(c) < dist(background) && dist(c) < 120) return true;
    }
  }
  return false;
}

// Paints one drawing on its own surface (logical size w x h at `dpi`).
tela::PixelSurface Paint(const tela::Drawing& d, float w, float h, float dpi) {
  tela::PixelSurface s;
  s.width = static_cast<int>(std::ceil(w * dpi));
  s.height = static_cast<int>(std::ceil(h * dpi));
  s.pixels.assign(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) * 4u, 0);
  tela::paint_drawing(s, d, tela::Point{0, 0}, dpi, tela::Rect{0, 0, static_cast<float>(s.width), static_cast<float>(s.height)});
  return s;
}

}  // namespace

SND_TEST(ToolUiEditorRaster, ConeLinesFollowTheEmitterFacing) {
  const ui::Palette p = ui::MakePalette(tela::Theme{});
  for (float dpi : {1.0f, 2.0f}) {
    ui::TopDownLook l;
    l.dpi = dpi;
    l.width = 200.0f;
    l.height = 200.0f;
    l.gridPx = 0.0f;
    l.listener = tela::Point{20.0f, 180.0f};
    l.emitter = tela::Point{100.0f, 100.0f};
    l.emitterYaw = 0.0f;  // facing up the screen
    l.innerDeg = 60.0f;
    l.outerDeg = 120.0f;
    l.coneLengthPx = 60.0f;
    l.handlePx = 20.0f;
    ui::TopDownShape shape;
    const tela::PixelSurface s = Paint(ui::DrawTopDown(l, p, &shape), l.width, l.height, dpi);
    // The inner edge at +-30 deg, the outer at +-60 deg, 50 px from the emitter.
    unsigned char rgb[3];
    for (float sign : {-1.0f, 1.0f}) {
      const tela::Point in = ui::YawHandle(l.emitter, sign * 30.0f * 3.14159265f / 180.0f, 50.0f);
      SND_CHECK_MSG(LineAt(s, static_cast<int>(in.x * dpi), static_cast<int>(in.y * dpi), p.solo, p.canvas),
                    "inner cone edge at dpi %g", static_cast<double>(dpi));
      const tela::Point out = ui::YawHandle(l.emitter, sign * 60.0f * 3.14159265f / 180.0f, 50.0f);
      SND_CHECK_MSG(LineAt(s, static_cast<int>(out.x * dpi), static_cast<int>(out.y * dpi), p.mute, p.canvas),
                    "outer cone edge at dpi %g", static_cast<double>(dpi));
      // The inner edge's colour is not on the outer edge (the two lines are told apart).
      SND_CHECK(!LineAt(s, static_cast<int>(out.x * dpi), static_cast<int>(out.y * dpi), p.solo, p.canvas));
    }
    // Straight below the emitter (behind it): the background.
    ui::PixelRgb(s, static_cast<int>(100.0f * dpi), static_cast<int>(150.0f * dpi), rgb);
    SND_CHECK(Near(rgb, p.canvas, 2));
    SND_CHECK_NEAR(shape.emitterHandle.x, 100.0f, 1e-4f);
    SND_CHECK_NEAR(shape.emitterHandle.y, 80.0f, 1e-4f);
  }
}

SND_TEST(ToolUiEditorRaster, LaneClipsAndMarksAtTheirTimes) {
  const ui::Palette p = ui::MakePalette(tela::Theme{});
  for (float dpi : {1.0f, 2.0f}) {
    ui::LaneLook l;
    l.dpi = dpi;
    l.width = 400.0f;
    l.height = 40.0f;
    l.axis = ui::TimeAxis{100.0, 2.0};  // 100 ms at x 0, 2 ms per px
    l.clips.push_back(ui::LaneClip{200.0, 400.0, 200.0, 0.0, 0.0, false, true, false});
    l.clips.push_back(ui::LaneClip{0.0, 50.0, 50.0, 0.0, 0.0, false, true, false});  // before the view
    ui::LaneShape shape;
    const tela::PixelSurface s = Paint(ui::DrawLane(l, p, &shape), l.width, l.height, dpi);
    SND_REQUIRE_EQ(shape.clips.size(), 2u);
    SND_CHECK_NEAR(shape.clips[0].x, 50.0f, 1e-4f);
    SND_CHECK_NEAR(shape.clips[0].width, 100.0f, 1e-4f);
    SND_CHECK_EQ(shape.clips[1].width, 0.0f);  // not visible
    const tela::Color fill = ui::Mix(p.accent, p.canvas, 0.45f);
    unsigned char rgb[3];
    ui::PixelRgb(s, static_cast<int>(52.0f * dpi), static_cast<int>(20.0f * dpi), rgb);
    SND_CHECK(Near(rgb, fill, 3));
    ui::PixelRgb(s, static_cast<int>(148.0f * dpi), static_cast<int>(20.0f * dpi), rgb);
    SND_CHECK(Near(rgb, fill, 3));
    ui::PixelRgb(s, static_cast<int>(153.0f * dpi), static_cast<int>(20.0f * dpi), rgb);
    SND_CHECK(!Near(rgb, fill, 3));
    // Marks: a bar line and the firing mark at their x.
    ui::MarkLook m;
    m.dpi = dpi;
    m.width = 300.0f;
    m.height = 40.0f;
    m.marks.push_back(ui::Mark{80.0f, ui::MarkKind::Bar});
    m.marks.push_back(ui::Mark{200.0f, ui::MarkKind::Fire});
    const tela::PixelSurface ms = Paint(ui::DrawMarks(m, p), m.width, m.height, dpi);
    ui::PixelRgb(ms, static_cast<int>(200.0f * dpi), static_cast<int>(10.0f * dpi), rgb);
    SND_CHECK(Near(rgb, p.failed, 40));
    ui::PixelRgb(ms, static_cast<int>(80.0f * dpi), static_cast<int>(10.0f * dpi), rgb);
    SND_CHECK(Near(rgb, p.textDim, 40));
    ui::PixelRgb(ms, static_cast<int>(140.0f * dpi), static_cast<int>(10.0f * dpi), rgb);
    SND_CHECK(Near(rgb, p.canvas, 2));
  }
}

SND_TEST(ToolUiEditorRaster, PairTableCellsSitInTheirGrid) {
  for (float dpi : {1.0f, 2.0f}) {
    std::map<std::string, std::string> f = doctest::BaseFiles();
    f["materials/T.json"] = "{\"type\": \"materialTable\", \"guid\": \"" + doctest::G(300) + "\", \"name\": \"T\", \"bank\": \"Main\", "
                            "\"materials\": [\"a\", \"b\", \"c\"]}";
    Editor t(ui::Screen::Material, static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi, f);
    const tela::PlacedElement* c00 = t.El("ma.pair.0.0");
    const tela::PlacedElement* c12 = t.El("ma.pair.1.2");
    SND_REQUIRE(c00 != nullptr && c12 != nullptr);
    // Row 1 is one row (24 px) below row 0, column 2 two cells right of column 0.
    SND_CHECK_NEAR(c12->bounds.y - c00->bounds.y, 24.0f, 1e-3f);
    SND_CHECK_NEAR(c12->bounds.x - c00->bounds.x, 2.0f * (c00->bounds.width + 2.0f), 1e-3f);
    tela::PixelSurface img;
    SND_REQUIRE(ui::Compose(t.app.runtime(), ui::ComposeOptions{}, &img));
    unsigned char rgb[3];
    ui::PixelRgb(img, static_cast<int>((c12->bounds.x + 2.0f) * dpi), static_cast<int>((c12->bounds.y + c12->bounds.height - 2.0f) * dpi),
                 rgb);
    SND_CHECK(Near(rgb, t.app.palette().theme.button, 3));
  }
}
