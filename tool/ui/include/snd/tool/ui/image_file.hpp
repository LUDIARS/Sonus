// Writes a Tela pixel surface (premultiplied BGRA) as a binary PPM (P6, RGB),
// i.e. composited over black as an opaque window shows it.
#ifndef SND_TOOL_UI_IMAGE_FILE_HPP
#define SND_TOOL_UI_IMAGE_FILE_HPP

#include <string>

#include <tela/pixel_surface.hpp>

namespace snd::tool::ui {

bool WritePpm(const tela::PixelSurface& surface, const std::string& path);
// RGB of one pixel (premultiplied over black). false outside the surface.
bool PixelRgb(const tela::PixelSurface& surface, int x, int y, unsigned char rgb[3]);

}  // namespace snd::tool::ui

#endif  // SND_TOOL_UI_IMAGE_FILE_HPP
