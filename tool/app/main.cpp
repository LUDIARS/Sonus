// sonus-tool: the authoring tool (design.md 11): the mixer and the profiler of a running game
// and, with a project, every editor of 11.1 over one editable document (stage 5).
#include <cstdio>
#include <memory>
#include <string>

#include "host.hpp"
#include "options.hpp"
#include "snd/platform/clock.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/live_sync.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/preview_engine.hpp"
#include "snd/tool/session.hpp"
#include "snd/tool/ui/app.hpp"
#include "snd/tool/ui/services.hpp"

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
  ProjectDocument document;
  bool haveDocument = false;
  if (!o.project.empty()) {
    if (!session.LoadProject(o.project, o.buildDir)) {
      for (const std::string& m : session.messages()) std::fprintf(stderr, "sonus-tool: %s\n", m.c_str());
      std::fprintf(stderr, "sonus-tool: the project has errors; the tool shows what could be read\n");
    }
    haveDocument = document.Load(o.project);
    if (!haveDocument) std::fprintf(stderr, "sonus-tool: cannot read %s as a document\n", o.project.c_str());
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

  // The preview engine (a runtime inside the tool): the tool connects to it instead of a game.
  std::unique_ptr<preview::PreviewEngine> engine;
  if (!o.preview.empty() && haveDocument) {
    preview::PreviewEngineDesc pd;
    pd.output = o.preview == "device" ? PreviewOutput::Device : PreviewOutput::Silent;
    engine = preview::PreviewEngine::Create(pd, &error);
    if (engine == nullptr) {
      std::fprintf(stderr, "sonus-tool: preview engine: %s\n", error.c_str());
    } else {
      if (!engine->LoadBuild(BuildDocument(document), &error)) {
        std::fprintf(stderr, "sonus-tool: preview engine: %s\n", error.c_str());
      }
      d.connection.connector = engine->connector();
    }
  }
  if (haveDocument) {
    LiveSyncOptions lo;
    lo.decision = o.refuseMismatch ? LiveDecision::Refuse : o.sendDiffs ? LiveDecision::SendDiffs : LiveDecision::Ask;
    if (engine != nullptr) {
      preview::PreviewEngine* e = engine.get();
      lo.writer = [e](const BankImage& bank, std::string* path, std::string*) {
        *path = e->PutRebuiltBank(bank.file, bank.bytes);
        return !path->empty();
      };
    } else if (!o.reloadDir.empty()) {
      lo.writer = DirectoryReloadWriter(o.reloadDir, o.reloadPrefix);
    }
    session.AttachDocument(&document, lo);
  }
  AuditionRouter audition;
  audition.Bind(&session.connection(), engine.get());
  audition.SetTarget(engine != nullptr ? AuditionTarget::Preview : AuditionTarget::Game);
  std::unique_ptr<ui::ReplayService> replay = ui::MakeReplayService();
  d.workspace.document = haveDocument ? &document : nullptr;
  d.workspace.preview = engine.get();
  d.workspace.audition = &audition;
  d.workspace.replay = replay.get();

  ui::App app(session, d);
  ui::Screen screen = ui::Screen::Mixer;
  ui::ParseScreen(o.screen, &screen);
  app.view().screen = screen;
  app.view().replay.reportDir = o.reportDir;
  if (o.connect || engine != nullptr) session.Connect(d.connection);

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
  if (replay != nullptr) replay->StopListening();
  session.Disconnect();
  session.AttachDocument(nullptr);
  return code;
}
