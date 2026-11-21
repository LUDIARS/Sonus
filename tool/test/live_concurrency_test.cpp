// Threads (group P; TSan and Release, repeated): the preview engine renders in real time on its
// own audio thread (Silent output), the live link server and client run their threads, the IO
// thread and workers load banks, while the UI thread edits the document, ticks LiveSync, polls the
// profiler streams, pumps the engine, auditions and reads every model. A second test replays one
// recording on two threads at once (each replay owns its runtime) and compares them.
#include <atomic>
#include <thread>

#include "live_fixture.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/profiler.hpp"
#include "snd/tool/profiler_streams.hpp"

using namespace snd::tool;
using namespace snd::tool::preview;

SND_TEST(ToolLiveThreads, UiThreadWithRealTimeEngineAndLiveLink) {
  ProjectDocument doc;
  livetest::LoadLive(doc);
  const DocBuild build = BuildDocument(doc);
  SND_REQUIRE(build.ok);
  PreviewEngineDesc ed;
  ed.output = PreviewOutput::Silent;
  std::string err;
  std::unique_ptr<PreviewEngine> engine = PreviewEngine::Create(ed, &err);
  SND_REQUIRE_MSG(engine != nullptr, "%s", err.c_str());
  SND_REQUIRE_MSG(engine->LoadBuild(build, &err), "%s", err.c_str());
  LiveSyncOptions o = livetest::MakeOptions(LiveDecision::SendDiffs);
  o.throttleMs = 10;
  PreviewEngine* e = engine.get();
  o.writer = [e](const BankImage& b, std::string* path, std::string*) {
    *path = e->PutRebuiltBank(b.file, b.bytes);
    return true;
  };
  LiveSync sync(doc, o);
  sync.SetBaseline(build);
  Connection conn;
  ConnectionDesc d;
  d.connector = engine->connector();
  d.reconnectDelayMs = 10;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);
  ProfilerStreams ps;
  ps.SetViews(kViewAll);
  Profiler prof;
  AuditionRouter router;
  router.Bind(&conn, engine.get());
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  std::uint32_t frames = 0, audition = 0;
  float db = 0.0f;
  bool dropped = false;
  while (std::chrono::steady_clock::now() < end) {
    ++frames;
    engine->Pump();
    const ConnectionEvents ev = conn.Poll();
    sync.Tick(conn, ev, livetest::Now());
    ps.Poll(conn.client(), livetest::Now());
    prof.Poll(conn.client(), livetest::Now());
    if (frames % 3u == 0u) {  // a drag on the Sfx fader
      db = db <= -24.0f ? 0.0f : db - 0.5f;
      doc.SetNumber(livetest::SfxGuid(), "volumeDb", db, 7u);
    }
    if (frames % 50u == 0u) {
      router.SetTarget((audition++ % 2u) == 0u ? AuditionTarget::Preview : AuditionTarget::Game);
      router.PlayEvent(SpatialPreview(doc, livetest::ToneGuid()).Audition());
    }
    if (frames % 120u == 0u) router.StopAll();
    if (!dropped && frames == 200u) {
      conn.Drop();  // a reconnect in the middle
      dropped = true;
    }
    // Read everything the UI shows.
    (void)sync.items();
    (void)sync.reserve();
    (void)ps.Memory();
    (void)ps.Log(LogFilter{});
    (void)engine->Status();
    (void)engine->RecentOutput(4);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  sync.FlushNow();
  const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool live = false;
  while (!live && std::chrono::steady_clock::now() < settle) {
    engine->Pump();
    const ConnectionEvents ev = conn.Poll();
    sync.Tick(conn, ev, livetest::Now());
    live = sync.phase() == LivePhase::Live;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_CHECK(live);
  SND_CHECK(frames > 100u);
  SND_CHECK(engine->Status().block > 0u);
  conn.Close();
}

SND_TEST(ToolLiveThreads, TwoReplaysOnTwoThreads) {
  // A recording of a short session made on a Manual engine (the test thread renders).
  livetest::Rig r;
  SND_REQUIRE(r.ok);
  SND_REQUIRE(r.Connect());
  SND_REQUIRE(r.Settled());
  snd::livelink::Client* c = r.conn.client();
  Profiler prof;
  SND_REQUIRE(prof.StartRecording(c));
  SND_REQUIRE(r.Run([&] {
    prof.Poll(c, livetest::Now());
    return prof.recordingState() == RecordingState::Recording;
  }));
  r.Frames(4);
  r.engine->Play(snd::HashId("Tone"), nullptr);
  r.Frames(20);
  SND_REQUIRE(prof.StopRecording(c));
  SND_REQUIRE(r.Run([&] {
    prof.Poll(c, livetest::Now());
    return prof.recordingState() == RecordingState::Ready;
  }));
  RecordingInfo info;
  info.sampleRate = 48000;
  info.blockSize = 256;
  Recording rec;
  SND_REQUIRE(prof.TakeRecording(c, info, &rec));
  const std::vector<ReportBank> banks = BanksOf(r.gameBuild);
  ReplayOptions o;
  o.haveConfig = true;
  o.config = r.engine->config();
  ReplayRun a, b;
  std::thread t1([&] { Replay(rec, banks, o, &a); });
  std::thread t2([&] { Replay(rec, banks, o, &b); });
  t1.join();
  t2.join();
  const ReplayComparison cmp = CompareRuns(a, b);
  SND_CHECK_MSG(cmp.ok, "%s", cmp.Text().c_str());
  SND_CHECK(cmp.bitsMatch);
}
