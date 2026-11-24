// Independent tests of group P: threads (tool/test/indep_s7_live_properties.md F; run under TSan
// and Release, repeated 20 times). Who touches what:
//   owner (this test's thread, the "UI"): the document, LiveSync::Tick and its readers, the
//     preview engine's owner functions (Play, MoveEmitter, StopAll, Pump, Status), hot reloads
//     (the writer is PutRebuiltBank, owner), Connection::Poll / Drop;
//   the engine's audio thread (Silent output): renders, writes the recent-output ring;
//   the live link server thread (in the engine) and the client's network thread;
//   the engine's IO thread and worker (bank loading of the hot reloads);
//   a reader thread: RecentOutput and connector() (documented "any thread");
//   a replay thread: Replay of a small recording in its own runtime, over and over.
#include <atomic>
#include <cmath>
#include <thread>

#include "indep_s7_live_fixture.hpp"
#include "snd/livelink/record_format.hpp"
#include "snd/tool/preview.hpp"
#include "snd/tool/preview/replay_host.hpp"
#include "snd/tool/spatial_preview.hpp"

using namespace snd::tool;
using namespace ilive;

SND_TEST(IndepS7LiveThreads, OwnerEditsWhileAudioReaderAndReplayRun) {
  ProjectDocument doc;
  Load(doc);
  const DocBuild build = BuildDocument(doc);
  SND_REQUIRE(build.ok);
  preview::PreviewEngineDesc ed;
  ed.output = PreviewOutput::Silent;
  std::string err;
  std::unique_ptr<preview::PreviewEngine> engine = preview::PreviewEngine::Create(ed, &err);
  SND_REQUIRE_MSG(engine != nullptr, "%s", err.c_str());
  SND_REQUIRE_MSG(engine->LoadBuild(build, &err), "%s", err.c_str());
  LiveSyncOptions o = Options(LiveDecision::SendDiffs);
  o.throttleMs = 8;
  preview::PreviewEngine* e = engine.get();
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
  d.profilerMask = 0x7Fu;
  SND_REQUIRE_EQ(conn.Open(sync.Prepare(d)), SND_OK);

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0}, nonFinite{0}, replays{0}, replayFailures{0};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const std::vector<float> v = engine->RecentOutput(1u + static_cast<std::uint32_t>(reads.load() % 20u));
      for (float x : v) nonFinite += std::isfinite(x) ? 0u : 1u;
      (void)engine->connector();
      ++reads;
      std::this_thread::yield();
    }
  });
  std::thread replayer([&] {
    Recording r;
    r.info.sampleRate = 48000;
    r.info.blockSize = 256;
    const ll::RecordLod lod{static_cast<std::uint32_t>(ll::RecordKind::Lod), 0u, 2u};
    const ll::RecordArena a{static_cast<std::uint32_t>(ll::RecordKind::Arena), 0u, 2u, 16u, 0u};
    const ll::ArenaSections s{0u, 0u, 0u, 0u};
    auto put = [&](const void* p, std::size_t n) {
      r.records.insert(r.records.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
    };
    put(&lod, sizeof lod);
    put(&a, sizeof a);
    put(&s, sizeof s);
    const std::vector<ReportBank> banks = preview::BanksOf(build);
    std::uint64_t first = 0;
    while (!stop.load(std::memory_order_acquire)) {
      ReplayRun run;
      if (!preview::Replay(r, banks, preview::ReplayOptions{}, &run)) {
        ++replayFailures;
      } else if (replays.load() == 0u) {
        first = run.digest;
      } else if (run.digest != first) {
        ++replayFailures;
      }
      ++replays;
    }
  });

  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  std::uint32_t frames = 0;
  std::uint64_t spot = 0;
  bool created = false;
  while (std::chrono::steady_clock::now() < end) {
    ++frames;
    engine->Pump();
    sync.Tick(conn, conn.Poll(), Now());
    if (frames % 2u == 0u) doc.SetNumber(SfxG(), "volumeDb", -0.25 * static_cast<double>(frames % 80u), 5u);
    if (frames % 40u == 0u) doc.Undo();
    if (frames % 25u == 0u) {
      EmitterPose p;
      p.position = Vec3f{1.0f, 0.0f, 2.0f};
      spot = engine->Play(snd::HashId("Spot"), &p);
      engine->Play(snd::HashId("Tone"), nullptr);
    }
    if (spot != 0u) {
      EmitterPose p;
      p.position = Vec3f{std::sin(static_cast<float>(frames) * 0.1f) * 4.0f, 0.0f, 3.0f};
      engine->MoveEmitter(spot, p);
    }
    if (frames % 90u == 0u) engine->StopAll();
    if (!created && frames == 150u) {  // a hot reload while everything runs
      const json::Value init = Json("{\"bank\": \"Main\", \"bus\": \"Amb\"}");
      created = !doc.Create("event", "Fresh", &init).empty();
    }
    if (frames == 400u) conn.Drop();
    (void)sync.items();
    (void)sync.reloads();
    (void)sync.check();
    (void)engine->Status();
    std::this_thread::sleep_for(std::chrono::microseconds(400));
  }
  // Everything settles: the game holds the document.
  sync.FlushNow();
  const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (sync.phase() != LivePhase::Live && std::chrono::steady_clock::now() < settle) {
    engine->Pump();
    sync.Tick(conn, conn.Poll(), Now());
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  stop.store(true, std::memory_order_release);
  reader.join();
  replayer.join();
  SND_CHECK_MSG(sync.phase() == LivePhase::Live, "phase %s", LivePhaseName(sync.phase()));
  SND_CHECK(reads.load() > 0u);
  SND_CHECK_EQ(nonFinite.load(), 0u);
  SND_CHECK(replays.load() > 0u);
  SND_CHECK_EQ(replayFailures.load(), 0u);
  SND_CHECK(created);
  for (const LiveProblem& p : sync.problems()) std::printf("problem: %s\n", p.message.c_str());
  SND_CHECK(sync.problems().empty());
  conn.Close();
  engine.reset();
}
