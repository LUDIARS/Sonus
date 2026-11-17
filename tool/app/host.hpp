// Hosts of sonus-tool. Each runs the UI loop until the window closes (or the
// frames of the headless run are done) and returns the process exit code.
#ifndef SND_TOOL_APP_HOST_HPP
#define SND_TOOL_APP_HOST_HPP

#include <cstdint>

#include "options.hpp"
#include "snd/tool/ui/app.hpp"

namespace snd::tool::app {

int RunHeadless(ui::App& app, const Options& options);
#if defined(SND_TOOL_HOST_WINDOWS)
int RunWindows(ui::App& app, const Options& options);
#endif
#if defined(SND_TOOL_HOST_MACOS)
int RunMacOS(ui::App& app, const Options& options);
#endif
#if defined(SND_TOOL_HAVE_PICTOR)
// Text-capable rendering through tela::PictorSurface (catches Tela's exceptions).
bool RenderWithPictor(const tela::Runtime& runtime, const char* font, tela::PixelSurface* out);
#endif

std::uint64_t NowNs();

}  // namespace snd::tool::app

#endif  // SND_TOOL_APP_HOST_HPP
