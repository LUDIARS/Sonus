// Independent tests of group U, pixels (indep_s7_ui_properties.md H1..H3): the CPU compositor's
// image has the drawn items where the model's values put them, at DPI 1 and 2 (no stored images).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <tela/pixel_surface.hpp>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/event_editor.hpp"
#include "snd/tool/spatial_preview.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"

using namespace iu;

namespace {

struct Px {
  unsigned char c[3] = {0, 0, 0};
  bool ok = false;
};

Px At(const tela::PixelSurface& s, float lx, float ly, float dpi) {
  Px p;
  p.ok = ui::PixelRgb(s, static_cast<int>(std::lround(lx * dpi)), static_cast<int>(std::lround(ly * dpi)), p.c);
  return p;
}

int Diff(const Px& a, const Px& b) {
  return std::abs(a.c[0] - b.c[0]) + std::abs(a.c[1] - b.c[1]) + std::abs(a.c[2] - b.c[2]);
}

bool Compose(Rig& r, tela::PixelSurface* px) { return ui::Compose(r.rt(), ui::ComposeOptions{}, px); }

}  // namespace

// H1: the clip box: inside it the lane is painted differently from the lane after it; the box
// moves with startMs (model edit) by (delta / msPerPx) logical pixels; DPI 1 and 2.
SND_TEST(IndepS7UiRaster, ClipBoxIsWhereTheTimelineSays) {
  for (float dpi : {1.0f, 2.0f}) {
    Rig r;
    r.app->SetViewport(static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    r.app->Navigate(G(8));
    r.Frame();
    EventEditor ed(r.doc, G(8));
    for (double start : {0.0, 400.0}) {
      SND_REQUIRE(ed.MoveClip(0, 0, start));
      r.Frame();
      const tela::PlacedElement* lane = r.El("ev.t0.lane");
      SND_REQUIRE(lane != nullptr);
      const Timeline tl = ed.ComputeTimeline();
      const ui::EventView& v = r.app->view().event;
      const float x0 = lane->bounds.x + static_cast<float>((tl.clips[0].startMs - v.scrollMs) / v.msPerPx);
      const float x1 = lane->bounds.x + static_cast<float>((tl.clips[0].endMs - v.scrollMs) / v.msPerPx);
      const float ymid = lane->bounds.y + lane->bounds.height * 0.5f;
      tela::PixelSurface px;
      SND_REQUIRE(Compose(r, &px));
      const Px inside = At(px, (x0 + x1) * 0.5f, ymid, dpi);
      const Px after = At(px, x1 + 30.0f, ymid, dpi);
      const Px before = At(px, x0 - 8.0f, ymid, dpi);
      SND_REQUIRE(inside.ok && after.ok);
      SND_CHECK_MSG(Diff(inside, after) > 30, "dpi %.0f start %.0f: box not painted (diff %d)", static_cast<double>(dpi),
                    start, Diff(inside, after));
      if (start > 0.0) {
        SND_CHECK_MSG(Diff(before, after) <= 30, "dpi %.0f: the lane before the box is not the lane (diff %d)",
                      static_cast<double>(dpi), Diff(before, after));
      }
      // The box edges: 3 logical px inside differs from 3 px outside.
      const Px inL = At(px, x0 + 3.0f, ymid, dpi), outR = At(px, x1 + 3.0f, ymid, dpi);
      SND_CHECK(Diff(inL, outR) > 30);
    }
  }
}

// H2: the maxDistance circle: a line at radius maxDistance / metersPerPixel around the emitter
// (most of 8 directions show a line pixel within 2 px of the radius); DPI 1 and 2.
SND_TEST(IndepS7UiRaster, MaxDistanceCircleHasItsRadius) {
  for (float dpi : {1.0f, 2.0f}) {
    Rig r;
    r.app->SetViewport(static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    r.app->Navigate(G(8));
    r.Show(ui::Screen::Spatial);
    SpatialPreview* sp = r.app->spatial();
    SND_REQUIRE(sp != nullptr);
    const tela::PlacedElement* view = r.El("sp.view");
    SND_REQUIRE(view != nullptr);
    float ex = 0, ey = 0;
    sp->WorldToScreen(sp->emitter().position.x, sp->emitter().position.z, view->bounds.width, view->bounds.height, &ex, &ey);
    ex += view->bounds.x;
    ey += view->bounds.y;
    const float radius = sp->MaxDistance() / sp->metersPerPixel();
    SND_REQUIRE(radius > 20.0f);
    tela::PixelSurface px;
    SND_REQUIRE(Compose(r, &px));
    int found = 0, inside = 0;
    for (int k = 0; k < 8; ++k) {
      const float a = (22.5f + 45.0f * static_cast<float>(k)) * 3.14159265f / 180.0f;
      const float dx = std::cos(a), dy = std::sin(a);
      const float far = radius + 7.0f;
      const float qx = ex + dx * far, qy = ey + dy * far;
      if (qx < view->bounds.x || qy < view->bounds.y || qx > view->bounds.x + view->bounds.width ||
          qy > view->bounds.y + view->bounds.height)
        continue;
      ++inside;
      const Px out = At(px, ex + dx * (radius + 6.0f), ey + dy * (radius + 6.0f), dpi);
      const Px in = At(px, ex + dx * (radius - 6.0f), ey + dy * (radius - 6.0f), dpi);
      bool line = false;
      for (float d = -2.0f; d <= 2.0f && !line; d += 0.5f) {
        const Px on = At(px, ex + dx * (radius + d), ey + dy * (radius + d), dpi);
        line = Diff(on, out) > 40 && Diff(on, in) > 40;
      }
      found += line ? 1 : 0;
    }
    SND_REQUIRE(inside >= 4);
    SND_CHECK_MSG(found >= inside - 2, "dpi %.0f: circle at r=%.1f seen in %d of %d directions", static_cast<double>(dpi),
                  static_cast<double>(radius), found, inside);
  }
}

// H3: curve points are painted at their places (the point's centre differs from the plot's empty
// corner); DPI 1 and 2.
SND_TEST(IndepS7UiRaster, CurvePointsArePaintedAtTheirPlaces) {
  for (float dpi : {1.0f, 2.0f}) {
    Rig r;
    r.app->SetViewport(static_cast<int>(1280 * dpi), static_cast<int>(800 * dpi), dpi);
    r.app->Navigate(G(6));
    r.Frame();
    if (r.app->view().params.event.empty()) r.app->view().params.event = G(8);
    r.Frame();
    const tela::PlacedElement* plot = r.El("pc.plot");
    SND_REQUIRE(plot != nullptr);
    tela::PixelSurface px;
    SND_REQUIRE(Compose(r, &px));
    // The empty corner: top left of the plot for an ascending curve.
    const Px empty = At(px, plot->bounds.x + 20.0f, plot->bounds.y + 20.0f, dpi);
    for (const char* id : {"pc.pt.0", "pc.pt.1"}) {
      const tela::PlacedElement* p = r.El(id);
      SND_REQUIRE(p != nullptr);
      const tela::Point c = uitest::Center(p);
      // Points at the ends of the x range sit on the plot's edge (no x margin): half of them is
      // clipped. Look 2 px inside the plot.
      const float inX = std::min(std::max(c.x, plot->bounds.x + 2.0f), plot->bounds.x + plot->bounds.width - 2.0f);
      const Px at = At(px, inX, c.y, dpi);
      SND_CHECK_MSG(Diff(at, empty) > 40, "dpi %.0f: %s not painted (diff %d)", static_cast<double>(dpi), id,
                    Diff(at, empty));
    }
  }
}
