// CPU composition of a Tela runtime's declaration with Tela::Raster only
// (headless mode and tests; no OS host, no Pictor).
//
// Same order and colours as Tela's PictorSurface: elements in layout order,
// panels filled with theme.panel, buttons with theme.button (theme.pressed
// while pressed), canvases painted with tela::paint_drawing inside their clip.
// Text needs Pictor's TrueType renderer, which Tela::Raster does not have:
// labels are either left out or shown as placeholder bars (their extent at
// the theme's font size, not the glyphs).
#ifndef SND_TOOL_UI_RASTER_COMPOSITOR_HPP
#define SND_TOOL_UI_RASTER_COMPOSITOR_HPP

#include <tela/pixel_surface.hpp>
#include <tela/runtime.hpp>

namespace snd::tool::ui {

struct ComposeOptions {
  bool textPlaceholders = false;
  tela::Color background{0, 0, 0, 255};  // under everything (a window's background)
};

// false if the viewport is empty or outside Tela's raster limits (nothing painted).
bool Compose(const tela::Runtime& runtime, const ComposeOptions& options, tela::PixelSurface* out);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_RASTER_COMPOSITOR_HPP
