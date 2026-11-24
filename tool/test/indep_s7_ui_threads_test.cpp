// Independent tests of group U, threads (indep_s7_ui_properties.md J1): the real-time preview
// engine (its audio thread, Silent output), the live link server and client threads, while the UI
// thread runs the App's frames and drives every screen through pointer and key events (drags,
// buttons, auditions, undo, screen changes, disconnect / connect). TSan and Release, repeated.
//
// Who touches what (checked by reading app.hpp / session.hpp / preview.hpp): the App, the Session,
// LiveSync, the document, the AuditionRouter and the engine's owner functions are the UI thread's
// (this test's thread); the engine renders on its audio thread and the live link client / server
// run their own threads, reached only through their thread-safe calls.
#include <chrono>
#include <memory>
#include <string>

#include "indep_s7_ui_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/preview_engine.hpp"

using namespace iu;
using namespace snd::tool::preview;

SND_TEST(IndepS7UiThreads, ScreensDrivenWhileEngineAndLinkRun) {
  ProjectDocument doc;
  doctest::LoadBase(doc);
  const DocBuild build = BuildDocument(doc);
  SND_REQUIRE(build.ok);
  PreviewEngineDesc ed;
  ed.output = PreviewOutput::Silent;
  std::string err;
  std::unique_ptr<PreviewEngine> engine = PreviewEngine::Create(ed, &err);
  SND_REQUIRE_MSG(engine != nullptr, "%s", err.c_str());
  SND_REQUIRE_MSG(engine->LoadBuild(build, &err), "%s", err.c_str());

  Project project;
  SND_REQUIRE(project.Load(doc));
  Session s;
  s.UseProject(project);
  s.AttachDocument(&doc);
  AuditionRouter router;
  router.Bind(&s.connection(), engine.get());
  router.SetTarget(AuditionTarget::Preview);
  ui::AppDesc desc;
  desc.workspace.document = &doc;
  desc.workspace.preview = engine.get();
  desc.workspace.audition = &router;
  desc.connection.connector = engine->connector();
  desc.connection.reconnectDelayMs = 10;
  ui::App app(s, desc);
  uitest::Pointer ptr(app);
  ConnectionDesc cd = desc.connection;
  SND_REQUIRE_EQ(s.Connect(cd), SND_OK);
  app.Frame(tooltest::Now());

  auto el = [&](const std::string& id) { return uitest::Find(app.runtime(), id); };
  auto click = [&](const std::string& id) {
    if (const tela::PlacedElement* e = el(id)) {
      const tela::Point c = uitest::Center(e);
      ptr.Click(c.x, c.y);
    }
  };
  auto drag = [&](const std::string& id, float dx, float dy) {
    const tela::PlacedElement* e = el(id);
    if (e == nullptr) return;
    const tela::Point c = uitest::Center(e);
    ptr.Down(c.x, c.y);
    app.Frame(tooltest::Now());
    ptr.Move(c.x + dx * 0.5f, c.y + dy * 0.5f);
    app.Frame(tooltest::Now());
    ptr.Move(c.x + dx, c.y + dy);
    app.Frame(tooltest::Now());
    ptr.Up(c.x + dx, c.y + dy);
    ptr.Advance(1000);
  };
  auto key = [&](std::uint32_t k, std::uint32_t mods = 0) {
    ui::KeyEvent e;
    e.key = k;
    e.modifiers = mods;
    app.Key(e);
  };

  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  std::uint32_t frame = 0;
  while (std::chrono::steady_clock::now() < end) {
    ++frame;
    switch (frame % 16u) {
      case 0: app.Navigate(G(8)); break;
      case 1: drag("ev.t0.c0", (frame % 3u == 0u) ? 12.0f : -12.0f, 0.0f); break;
      case 2: click("ev.play"); break;
      case 3: key(ui::kKeyF1 + 6); break;  // F7: 3D
      case 4: drag("sp.emitter", 15.0f, (frame % 2u) ? -10.0f : 10.0f); break;
      case 5: click("sp.play"); break;
      case 6: key(ui::kKeyF1 + 3); click("mx.bus." + ui::ObjectKey(G(3))); break;  // F4: Buses
      case 7: click((frame % 2u) ? "mx.bp.vol.inc" : "mx.bp.vol.dec"); break;
      case 8: key('Z', ui::kModControl); break;
      case 9: key('Y', ui::kModControl); break;
      case 10: key(ui::kKeyF1 + 2); app.view().profiler.tab = frame % 6u; break;  // F3: profiler
      case 11: app.OnScreen(ui::Screen::Link); break;
      case 12: if (frame % 64u == 12u) click("link.toggle"); break;
      case 13: app.OnScreen(ui::Screen::Problems); break;
      case 14: key(ui::kKeyF1 + 1); drag(ui::BusId(snd::HashId("Sfx")) + ".fader.hit", 0.0f, -8.0f); break;  // F2: mixer
      case 15: if (frame % 64u == 15u && !s.connection().IsOpen()) click("link.toggle"); break;
      default: break;
    }
    app.Frame(tooltest::Now());
    (void)engine->RecentOutput(2);
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
  router.StopAll();
  s.Disconnect();
  app.Frame(tooltest::Now());
  SND_CHECK(!doc.InGroup());
  while (doc.Undo()) {
  }
  SND_CHECK(!doc.IsDirty());
  SND_CHECK(frame > 50u);
}
