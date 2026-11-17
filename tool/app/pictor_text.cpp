// Headless rendering with text through Tela's Pictor surface (only when Tela
// was built with TELA_BUILD_PICTOR). Compiled with exceptions: Tela throws on a
// missing / unsupported font.
#include <cstdio>
#include <exception>

#include <tela/pictor_surface.hpp>

#include "host.hpp"

namespace snd::tool::app {

bool RenderWithPictor(const tela::Runtime& runtime, const char* font, tela::PixelSurface* out) {
  try {
    tela::PictorSurface surface(font);
    *out = surface.render(runtime);
    return out->width > 0 && out->height > 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sonus-tool: text rendering failed (%s); drawing without text\n", e.what());
    return false;
  }
}

}  // namespace snd::tool::app
