// Headless host: runs frames without a window and writes a snapshot drawn with
// Tela::Raster (or with Tela's Pictor surface when available and --font given).
#include <cstdio>

#include "host.hpp"
#include "snd/platform/clock.hpp"
#include "snd/tool/ui/image_file.hpp"
#include "snd/tool/ui/raster_compositor.hpp"

namespace snd::tool::app {

int RunHeadless(ui::App& app, const Options& o) {
  app.SetViewport(o.width, o.height, o.dpi);
  for (std::uint32_t f = 0; f < o.frames; ++f) {
    app.Frame(NowNs());
    if (f + 1 < o.frames && o.frameMs != 0u) platform::SleepFor(std::uint64_t{o.frameMs} * 1000000u);
  }
  if (o.frames == 0u) app.Frame(NowNs());
  for (const std::string& m : app.session().messages()) std::printf("%s\n", m.c_str());
  if (o.snapshot.empty()) return 0;
  tela::PixelSurface surface;
  bool drawn = false;
#if defined(SND_TOOL_HAVE_PICTOR)
  if (!o.font.empty()) drawn = RenderWithPictor(app.runtime(), o.font.c_str(), &surface);
#else
  if (!o.font.empty()) std::fprintf(stderr, "sonus-tool: Tela was built without Pictor; text is not drawn\n");
#endif
  if (!drawn) {
    ui::ComposeOptions co;
    co.textPlaceholders = o.textPlaceholders;
    drawn = ui::Compose(app.runtime(), co, &surface);
  }
  if (!drawn || !ui::WritePpm(surface, o.snapshot)) {
    std::fprintf(stderr, "sonus-tool: cannot write %s\n", o.snapshot.c_str());
    return 1;
  }
  std::printf("snapshot: %s (%dx%d)\n", o.snapshot.c_str(), surface.width, surface.height);
  return 0;
}

}  // namespace snd::tool::app
