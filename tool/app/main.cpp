// sonus-tool: the authoring tool's mixer and profiler (design.md 11, stage 2).
#include <cstdio>
#include <string>

#include "host.hpp"
#include "options.hpp"
#include "snd/platform/clock.hpp"
#include "snd/tool/session.hpp"
#include "snd/tool/ui/app.hpp"

namespace snd::tool::app {
std::uint64_t NowNs() { return platform::MonotonicNanoseconds(); }
}  // namespace snd::tool::app

int main(int argc, char** argv) {
  using namespace snd::tool;
  app::Options o;
  std::string error;
  if (!app::ParseOptions(argc, argv, &o, &error)) {
    std::fprintf(stderr, "sonus-tool: %s\n%s", error.c_str(), app::Usage());
    return 2;
  }
  if (o.help) {
    std::printf("%s", app::Usage());
    return 0;
  }
  Session session;
  if (!o.project.empty() && !session.LoadProject(o.project, o.buildDir)) {
    for (const std::string& m : session.messages()) std::fprintf(stderr, "sonus-tool: %s\n", m.c_str());
    std::fprintf(stderr, "sonus-tool: the project has errors; the tool shows what could be read\n");
  }
  ui::AppDesc d;
  d.width = o.width;
  d.height = o.height;
  d.dpiScale = o.dpi;
  d.hostOwnsViewport = !o.headless;
  d.connection.address = o.address;
  d.connection.port = o.port;
  d.connection.policy = o.refuseMismatch ? MismatchPolicy::Refuse : MismatchPolicy::SendDiffs;
  d.recordingPath = o.recording;
  ui::App app(session, d);
  app.view().screen = o.screen == "profiler" ? ui::Screen::Profiler : ui::Screen::Mixer;
  if (o.connect) session.Connect(d.connection);

  int code = 0;
  if (o.headless) {
    code = app::RunHeadless(app, o);
  } else {
#if defined(SND_TOOL_HOST_WINDOWS)
    code = app::RunWindows(app, o);
#elif defined(SND_TOOL_HOST_MACOS)
    code = app::RunMacOS(app, o);
#else
    std::fprintf(stderr, "sonus-tool: no window host on this platform (Tela has Windows and macOS hosts); "
                         "use --headless\n");
    code = 2;
#endif
  }
  session.Disconnect();
  return code;
}
