// The screens over a live connection (the preview engine as the game, in-process): the Link screen
// (matching, the mismatch question answered from the header and from the screen, the counters),
// the profiler's tabs (CPU, memory, streams, LOD, event log with a filter), and the replay screen
// (record, save, load, describe, replay twice with the real replay service, WAV, bug report).
#include <cmath>
#include <filesystem>

#include "live_fixture.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/mixer_editor.hpp"
#include "snd/tool/ui/services.hpp"
#include "ui_editor_helpers.hpp"

using namespace snd::tool;
using namespace edtest;
namespace fs = std::filesystem;

namespace {

struct LiveUi {
  ProjectDocument doc;
  DocBuild gameBuild;
  std::unique_ptr<preview::PreviewEngine> engine;
  Session s;
  AuditionRouter router;
  std::unique_ptr<ui::ReplayService> replay = ui::MakeReplayService();
  std::unique_ptr<ui::App> app;
  uitest::Pointer* ptr = nullptr;
  std::unique_ptr<uitest::Pointer> pointer;
  bool ok = false;

  // `edit` changes the document after the game was started (a build mismatch).
  explicit LiveUi(bool mismatch) {
    livetest::LoadLive(doc);
    gameBuild = BuildDocument(doc);
    std::string err;
    engine = preview::PreviewEngine::Create(preview::PreviewEngineDesc{}, &err);
    if (engine == nullptr || !engine->LoadBuild(gameBuild, &err)) {
      std::printf("engine: %s\n", err.c_str());
      return;
    }
    if (mismatch) MixerEditor(doc).SetVolume(livetest::SfxGuid(), -6.0);
    Project p;
    p.Load(doc);
    s.UseProject(p);
    LiveSyncOptions o = livetest::MakeOptions(LiveDecision::Ask);
    preview::PreviewEngine* e = engine.get();
    o.writer = [e](const BankImage& b, std::string* path, std::string*) {
      *path = e->PutRebuiltBank(b.file, b.bytes);
      return true;
    };
    s.AttachDocument(&doc, o);
    router.Bind(&s.connection(), engine.get());
    ui::AppDesc d;
    d.connection.connector = engine->connector();
    d.workspace.document = &doc;
    d.workspace.preview = engine.get();
    d.workspace.audition = &router;
    d.workspace.replay = replay.get();
    app = std::make_unique<ui::App>(s, d);
    pointer = std::make_unique<uitest::Pointer>(*app);
    ptr = pointer.get();
    ok = s.Connect(d.connection) == SND_OK;
  }
  ~LiveUi() {
    if (replay != nullptr) replay->StopListening();
    s.Disconnect();
    s.AttachDocument(nullptr);
  }
  void Frame() {
    engine->Render(1, nullptr);
    app->Frame(livetest::Now());
  }
  // Frames until `pred`, at most 30 s of wall time (some reports are periodic in real time).
  template <class Pred>
  bool Until(Pred pred) {
    const std::uint64_t end = livetest::Now() + 30000000000ull;
    while (livetest::Now() < end) {
      Frame();
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return false;
  }
  const tela::PlacedElement* El(const std::string& id) const { return Find(app->runtime(), id); }
  bool Click(const std::string& id) {
    const tela::PlacedElement* e = El(id);
    if (e == nullptr) return false;
    const tela::Point c = Center(e);
    ptr->Click(c.x, c.y);
    Frame();
    return true;
  }
  std::string Label(const std::string& id) const {
    const tela::PlacedElement* e = El(id);
    return e != nullptr ? e->element.label : std::string();
  }
};

}  // namespace

SND_TEST(ToolUiLive, LinkScreenMatchesAndShowsTheState) {
  LiveUi t(false);
  SND_REQUIRE(t.ok);
  t.app->view().screen = ui::Screen::Link;
  SND_REQUIRE(t.Until([&] { return t.s.live()->phase() == LivePhase::Live; }));
  SND_CHECK(t.Label("ln.state").find("live: live") != std::string::npos);
  SND_CHECK(t.El("ln.target") != nullptr);  // in-process: no address to type
  SND_CHECK(t.El("ln.stats") != nullptr && t.El("ln.resync") != nullptr && t.El("ln.reserve.0.bar") != nullptr);
  // An edit goes live and shows up in the counters and the items tab.
  MixerEditor(t.doc).SetVolume(livetest::SfxGuid(), -3.0);
  t.app->Edited();
  SND_REQUIRE(t.Until([&] { return t.s.live()->stats().acked > 0u && t.s.live()->phase() == LivePhase::Live; }));
  SND_REQUIRE(t.Click("ln.tab.1"));
  SND_CHECK(t.El("ln.item.0") != nullptr);
  SND_REQUIRE(t.Click("ln.tab.2"));  // problems: the one list
  SND_CHECK(t.app->view().screen == ui::Screen::Problems);
}

SND_TEST(ToolUiLive, MismatchQuestionFromTheHeaderAndTheScreen) {
  {
    LiveUi t(true);
    SND_REQUIRE(t.ok);
    t.app->view().screen = ui::Screen::Link;
    // The question waits; the game's bank list comes with it (1.1).
    SND_REQUIRE(t.Until([&] { return t.s.live()->pending() && t.s.live()->check().banksKnown; }));
    t.Frame();
    SND_CHECK(t.El("link.ask.send") != nullptr && t.El("link.ask.refuse") != nullptr);  // the header asks too
    SND_REQUIRE(t.El("ln.ask.send") != nullptr);
    SND_CHECK(t.El("ln.bank.0") != nullptr);
    SND_CHECK(t.Label("ln.bank.0").find("differs") != std::string::npos);
    SND_REQUIRE(t.Click("ln.ask.send"));
    SND_REQUIRE(t.Until([&] { return t.s.live()->phase() == LivePhase::Live; }));
    SND_CHECK(t.El("ln.ask.send") == nullptr);
  }
  {
    LiveUi t(true);
    SND_REQUIRE(t.ok);
    SND_REQUIRE(t.Until([&] { return t.s.live()->pending(); }));
    t.Frame();
    SND_REQUIRE(t.Click("link.ask.refuse"));  // from the header on any screen
    SND_REQUIRE(t.Until([&] { return t.s.live()->phase() == LivePhase::Refused; }));
  }
}

SND_TEST(ToolUiLive, ProfilerTabs) {
  LiveUi t(false);
  SND_REQUIRE(t.ok);
  t.app->view().screen = ui::Screen::Profiler;
  SND_REQUIRE(t.Until([&] { return t.s.connection().state() == LinkState::Online; }));
  t.engine->Play(snd::HashId("Tone"), nullptr);
  SND_REQUIRE(t.Click("prof.tab.1"));
  SND_CHECK(t.app->view().profiler.tab == 1u);
  SND_REQUIRE(t.Until([&] { return t.s.streams().cpu().size() > 4u && !t.s.streams().cpuDetail().empty(); }));
  SND_CHECK(t.El("prof.cpu.graph") != nullptr && t.El("prof.cpu.row.0") != nullptr);
  SND_REQUIRE(t.Click("prof.tab.2"));
  SND_REQUIRE(t.Until([&] { return t.s.streams().hasMemory(); }));
  SND_CHECK(t.El("prof.mem.row.0") != nullptr && t.El("prof.mem.row.0.bar") != nullptr);
  SND_REQUIRE(t.Click("prof.tab.3"));
  SND_CHECK(t.El("prof.st.sum") != nullptr);
  SND_REQUIRE(t.Click("prof.tab.4"));
  SND_CHECK(t.El("prof.lod.level") != nullptr && t.El("prof.lod.graph") != nullptr);
  SND_REQUIRE(t.Click("prof.tab.5"));
  t.engine->Play(snd::HashId("Pad"), nullptr);
  SND_REQUIRE(t.Until([&] { return t.El("prof.log.row.0") != nullptr; }));
  // Filter: no commands, no events, no ends -> nothing shown.
  SND_REQUIRE(t.Click("prof.log.f.cmd"));
  SND_REQUIRE(t.Click("prof.log.f.ev"));
  SND_REQUIRE(t.Click("prof.log.f.end"));
  SND_CHECK(t.El("prof.log.row.0") == nullptr);
  SND_REQUIRE(t.Click("prof.tab.0"));
  SND_CHECK(t.El("prof.graph") != nullptr);
}

SND_TEST(ToolUiLive, RecordReplayTwiceWavAndBugReport) {
  LiveUi t(false);
  SND_REQUIRE(t.ok);
  t.app->view().screen = ui::Screen::Replay;
  SND_REQUIRE(t.Until([&] { return t.s.live()->phase() == LivePhase::Live; }));
  SND_CHECK(t.Label("rp.twice") == "(Replay twice and compare)");  // nothing loaded yet
  SND_REQUIRE(t.Click("rp.record"));
  SND_REQUIRE(t.Until([&] { return t.s.profiler().recordingState() == RecordingState::Recording; }));
  for (int i = 0; i < 4; ++i) t.Frame();
  const std::uint64_t tone = t.engine->Play(snd::HashId("Tone"), nullptr);
  for (int i = 0; i < 20; ++i) t.Frame();
  t.engine->Stop(tone);
  for (int i = 0; i < 10; ++i) t.Frame();
  SND_REQUIRE(t.Click("rp.record"));  // stop
  SND_REQUIRE(t.Until([&] { return t.s.profiler().recordingState() == RecordingState::Ready; }));
  doctest::TempDir dir("ui-replay");
  const std::string rec = (dir.path() / "take.snrec").string();
  t.app->SetRecordingPath(rec);
  SND_REQUIRE(t.Click("rp.save"));
  SND_REQUIRE(fs::exists(rec));
  SND_REQUIRE(t.Click("rp.load"));
  // The recordings list of the folder loads the picked file too.
  SND_REQUIRE(t.Click("rp.list"));
  SND_REQUIRE(t.app->picker() != nullptr);
  SND_REQUIRE_EQ(t.app->picker()->items.size(), 1u);
  SND_CHECK_EQ(t.app->picker()->items[0].label, std::string("take.snrec"));
  SND_REQUIRE(t.Click("picker.item.0"));
  SND_CHECK(t.Label("rp.desc.0").find("loaded") == 0u);
  SND_CHECK(t.El("rp.desc.3") != nullptr);  // the description, line by line
  // Replay twice with the real service.
  SND_REQUIRE(t.Click("rp.twice"));
  const ui::ReplayViewState& rv = t.app->view().replay;
  SND_REQUIRE(!rv.result.empty());
  bool decisions = false;
  for (const std::string& l : rv.result) decisions = decisions || l.find("decision") != std::string::npos;
  SND_CHECK(decisions);
  SND_REQUIRE(rv.lastRun != nullptr);
  SND_CHECK(rv.lastRun->ok);
  SND_CHECK(t.El("rp.result.0") != nullptr);
  // WAV of the first replay.
  t.app->view().replay.wavPath = (dir.path() / "replay.wav").string();
  t.Frame();
  SND_REQUIRE(t.Click("rp.wav"));
  SND_CHECK(fs::exists(dir.path() / "replay.wav"));
  // The bug report, written and opened again.
  t.app->view().replay.reportDir = (dir.path() / "report").string();
  t.Frame();
  SND_REQUIRE(t.Click("rp.report.write"));
  SND_CHECK(fs::exists(dir.path() / "report" / "report.txt"));
  SND_REQUIRE(t.Click("rp.report.open"));
  SND_CHECK(t.app->view().replay.result.front().find("report:") == 0u);
  bool expectation = false;
  for (const std::string& l : t.app->view().replay.result) expectation = expectation || l == "expectation: met";
  SND_CHECK(expectation);
}
