// CPU composition with Tela::Raster.
#include "snd/tool/ui/raster_compositor.hpp"

#include <algorithm>
#include <cmath>

#include <tela/drawing_raster.hpp>

#include "snd/tool/ui/palette.hpp"
#include "snd/tool/ui/safe_drawing.hpp"

namespace snd::tool::ui {
namespace {

bool FiniteRect(const tela::Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) && std::isfinite(r.height) &&
         std::fabs(r.x) < 1.0e6f && std::fabs(r.y) < 1.0e6f && r.width < 1.0e6f && r.height < 1.0e6f;
}

// Fills `clip` (physical pixels) through Tela's rasterizer: a rectangle drawing in
// logical coordinates at origin 0.
void Fill(tela::PixelSurface& out, tela::Rect clipPx, tela::Color c, float scale) {
  if (clipPx.width <= 0.0f || clipPx.height <= 0.0f) return;
  SafeDrawing d(scale);
  d.Rectangle(tela::Rect{clipPx.x / scale, clipPx.y / scale, clipPx.width / scale, clipPx.height / scale}, c);
  tela::paint_drawing(out, d.drawing(), tela::Point{0, 0}, scale, clipPx);
}

}  // namespace

bool Compose(const tela::Runtime& runtime, const ComposeOptions& o, tela::PixelSurface* out) {
  const tela::Viewport& vp = runtime.viewport();
  const float scale = vp.dpi_scale;
  if (vp.width <= 0 || vp.height <= 0 || vp.width > 16384 || vp.height > 16384 ||
      static_cast<std::uint64_t>(vp.width) * static_cast<std::uint64_t>(vp.height) > 16777216u ||
      !(scale >= 0.25f && scale <= 8.0f)) {
    return false;
  }
  out->width = vp.width;
  out->height = vp.height;
  out->pixels.assign(static_cast<std::size_t>(vp.width) * static_cast<std::size_t>(vp.height) * 4u, 0);
  const tela::Rect all{0, 0, static_cast<float>(vp.width), static_cast<float>(vp.height)};
  Fill(*out, all, o.background, scale);
  const tela::Theme& theme = runtime.theme();
  for (const tela::PlacedElement& placed : runtime.elements()) {
    const tela::Element& e = placed.element;
    const tela::Rect clip{placed.clip.x * scale, placed.clip.y * scale, placed.clip.width * scale,
                          placed.clip.height * scale};
    if (!FiniteRect(clip) || clip.width <= 0.0f || clip.height <= 0.0f) continue;
    if (e.kind == tela::ElementKind::canvas && FiniteRect(placed.bounds)) {
      tela::paint_drawing(*out, e.drawing, tela::Point{placed.bounds.x, placed.bounds.y}, scale, clip);
    }
    if (e.kind == tela::ElementKind::panel) Fill(*out, clip, theme.panel, scale);
    if (e.kind == tela::ElementKind::button) {
      const tela::ElementState* s = runtime.state(e.id);
      Fill(*out, clip, s != nullptr && s->pressed ? theme.pressed : theme.button, scale);
    }
    if (!o.textPlaceholders || e.label.empty()) continue;
    // Placeholder: one bar per text row, as long as the label would be at about half an em per byte.
    const float pad = e.layout.padding;
    const float em = theme.font_size * e.layout.text_scale;
    const float avail = std::max(0.0f, placed.bounds.width - 2.0f * pad);
    const float len = std::min(avail, 0.5f * em * static_cast<float>(e.label.size()));
    if (len <= 0.0f) continue;
    const tela::Rect bar{(placed.bounds.x + pad) * scale, (placed.bounds.y + pad + em * 0.3f) * scale, len * scale,
                         std::max(1.0f, em * 0.45f) * scale};
    Fill(*out, tela::intersect(bar, clip), WithAlpha(theme.text, 90), scale);
  }
  return true;
}

}  // namespace snd::tool::ui
