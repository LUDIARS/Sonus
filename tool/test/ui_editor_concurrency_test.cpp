// The stage 5 screens while the preview engine runs in real time (Silent output: its audio thread
// renders, the live link client receives on its network thread): the UI thread edits through the
// screens (clip drags that go live, number fields, the 3D audition moving its emitter, a transition
// audition), switches screens and profiler tabs and disconnects / reconnects. For TSan.
#include <chrono>
#include <thread>

#include "live_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/ui/services.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;

SND_TEST(ToolUiEditorConcurrency, ScreensEditWhileThePreviewEngineRuns) {
  ProjectDocument doc;
  livetest::LoadLive(doc, true);
  const DocBuild build = BuildDocument(doc);
  SND_REQUIRE(build.ok);
  preview::PreviewEngineDesc ed;
  ed.output = PreviewOutput::Silent;
  std::string err;
  std::unique_ptr<preview::PreviewEngine> engine = preview::PreviewEngine::Create(ed, &err);
  SND_REQUIRE_MSG(engine != nullptr, "%s", err.c_str());
  SND_REQUIRE_MSG(engine->LoadBuild(build, &err), "%s", err.c_str());
  Session s;
  Project p;
  p.Load(doc);
  s.UseProject(p);
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  preview::PreviewEngine* e = engine.get();
  o.writer = [e](const BankImage& b, std::string* path, std::string*) {
    *path = e->PutRebuiltBank(b.file, b.bytes);
    return true;
  };
  s.AttachDocument(&doc, o);
  AuditionRouter router;
  router.Bind(&s.connection(), engine.get());
  std::unique_ptr<ui::ReplayService> replay = ui::MakeReplayService();
  ui::AppDesc d;
  d.connection.connector = engine->connector();
  d.workspace.document = &doc;
  d.workspace.preview = engine.get();
  d.workspace.audition = &router;
  d.workspace.replay = replay.get();
  ui::App app(s, d);
  uitest::Pointer ptr(app);
  SND_REQUIRE_EQ(s.Connect(d.connection), SND_OK);
  auto frame = [&] {
    app.Frame(livetest::Now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };
  auto click = [&](const std::string& id) {
    const tela::PlacedElement* el = Find(app.runtime(), id);
    if (el == nullptr) return false;
    const tela::Point c = Center(el);
    ptr.Click(c.x, c.y);
    frame();
    return true;
  };
  auto drag = [&](const std::string& id, float dx, float dy) {
    const tela::PlacedElement* el = Find(app.runtime(), id);
    if (el == nullptr) return;
    const tela::Point c = Center(el);
    ptr.Down(c.x, c.y);
    for (int i = 1; i <= 4; ++i) {
      ptr.Move(c.x + dx * static_cast<float>(i) / 4.0f, c.y + dy * static_cast<float>(i) / 4.0f);
      frame();
    }
    ptr.Up(c.x + dx, c.y + dy);
    ptr.Advance(1000);
    frame();
  };
  for (int i = 0; i < 3000 && s.connection().state() != LinkState::Online; ++i) frame();
  SND_REQUIRE(s.connection().state() == LinkState::Online);
  for (int round = 0; round < 3; ++round) {
    app.OnScreen(ui::Screen::Event);
    app.view().event.event = livetest::ToneGuid();
    frame();
    drag("ev.t0.c0", 20.0f, 0.0f);
    click("ev.f.n1.inc");
    app.OnScreen(ui::Screen::Buses);
    app.view().buses.bus = livetest::SfxGuid();
    frame();
    drag("mx.bp.vol", 0.0f, -8.0f);
    app.OnScreen(ui::Screen::Spatial);
    app.view().spatial.event = doctest::G(12);  // Dc3D
    frame();
    if (!app.view().spatial.playing) click("sp.play");
    drag("sp.emitter", 15.0f, -5.0f);
    app.OnScreen(ui::Screen::Music);
    app.view().music.tab = 2;
    frame();
    click("mu.pv.play");
    app.OnScreen(ui::Screen::Profiler);
    click("prof.tab." + std::to_string(1 + round % 5));
    for (int i = 0; i < 5; ++i) frame();
    app.OnScreen(ui::Screen::Link);
    frame();
    if (round == 1) {
      s.connection().Drop();  // the client reconnects
      for (int i = 0; i < 20; ++i) frame();
    }
    app.DocUndo();
    frame();
  }
  click("sp.play");
  router.StopAll();
  for (int i = 0; i < 20; ++i) frame();
  s.Disconnect();
  s.AttachDocument(nullptr);
  SND_CHECK(app.declarations() > 100u);
}
