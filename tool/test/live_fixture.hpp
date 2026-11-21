// Group P test fixture: an in-memory project with analytic material, the preview engine
// (Sonus::tool_preview, Manual output: the test renders the blocks), a Connection to it over the
// in-process transport and LiveSync on the document.
//
// Project "Live" (bank "Main"):
//   buses     Master (master), Sfx (parent Master, 0 dB), Music (parent Master, -3 dB: the BUSS
//             volume column is in the bank, so a Sfx volume edit is a cell edit)
//   category  SE; parameter Knob (0..1, default 0.25)
//   asset     dc: mono 48 kHz PCMF, 4800 frames of kDc (0.25), looped
//   events    Tone (Sfx, loops forever), Pad (Music, loops), Dc3D (master, 3D, maxDistance 12,
//             inverse distance)
// The DC signal makes every level analytic: bus volumes scale it exactly.
// Music (WithMusic): assets ma (kDc) and mb (2 kDc), segments SegA / SegB (4 bars of 4/4 at
// 120 BPM, 8 s each), playlists PA / PB, music event Song (Main, bus Music): Explore -> PA,
// Battle -> PB, one rule "* -> *" next bar without fades.
#ifndef SND_TOOL_TEST_LIVE_FIXTURE_HPP
#define SND_TOOL_TEST_LIVE_FIXTURE_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doc_fixture.hpp"
#include "snd/livelink/client.hpp"
#include "snd/livelink/struct_edit.hpp"
#include "snd/platform/clock.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/connection.hpp"
#include "snd/tool/doc_build.hpp"
#include "snd/tool/document.hpp"
#include "snd/tool/live_sync.hpp"
#include "snd/tool/preview/preview_engine.hpp"

namespace livetest {

using doctest::G;
inline constexpr float kDc = 0.25f;
inline double Ratio(double db) { return std::pow(10.0, db / 20.0); }
inline std::uint64_t Now() { return snd::platform::MonotonicNanoseconds(); }

inline std::string SfxGuid() { return G(3); }
inline std::string ToneGuid() { return G(10); }

inline std::map<std::string, std::string> LiveFiles(bool withMusic = false) {
  std::map<std::string, std::string> f;
  f["project.json"] = "{\"type\": \"project\", \"guid\": \"" + G(100) + "\", \"name\": \"Live\", \"formatVersion\": 1}";
  f["banks/Main.json"] = "{\"type\": \"bank\", \"guid\": \"" + G(1) + "\", \"name\": \"Main\"}";
  f["buses/Master.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(2) + "\", \"name\": \"Master\", \"bank\": \"Main\", \"master\": true}";
  f["buses/Sfx.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(3) + "\", \"name\": \"Sfx\", \"bank\": \"Main\", \"parent\": \"Master\"}";
  f["buses/Music.json"] = "{\"type\": \"bus\", \"guid\": \"" + G(4) + "\", \"name\": \"Music\", \"bank\": \"Main\", \"parent\": \"Master\", \"volumeDb\": -3}";
  f["categories/SE.json"] = "{\"type\": \"category\", \"guid\": \"" + G(5) + "\", \"name\": \"SE\", \"bank\": \"Main\"}";
  f["parameters/Knob.json"] = "{\"type\": \"parameter\", \"guid\": \"" + G(6) +
                              "\", \"name\": \"Knob\", \"bank\": \"Main\", \"min\": 0, \"max\": 1, \"default\": 0.25}";
  f["assets/dc.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(7) + "\", \"name\": \"dc\", \"bank\": \"Main\", \"file\": \"audio/dc.wav\", "
                        "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}, \"loop\": {\"start\": 0, \"end\": 4800}}";
  f["events/Tone.json"] = "{\"type\": \"event\", \"guid\": \"" + G(10) + "\", \"name\": \"Tone\", \"bank\": \"Main\", \"bus\": \"Sfx\", "
                          "\"category\": \"SE\", \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/Pad.json"] = "{\"type\": \"event\", \"guid\": \"" + G(11) + "\", \"name\": \"Pad\", \"bank\": \"Main\", \"bus\": \"Music\", "
                         "\"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  f["events/Dc3D.json"] = "{\"type\": \"event\", \"guid\": \"" + G(12) + "\", \"name\": \"Dc3D\", \"bank\": \"Main\", \"is3D\": true, "
                          "\"maxDistance\": 12, \"tracks\": [{\"clips\": [{\"asset\": \"dc\", \"loopCount\": 0}]}]}";
  if (withMusic) {
    f["assets/ma.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(20) + "\", \"name\": \"ma\", \"bank\": \"Main\", \"file\": \"audio/ma.wav\", "
                          "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}}";
    f["assets/mb.json"] = "{\"type\": \"asset\", \"guid\": \"" + G(21) + "\", \"name\": \"mb\", \"bank\": \"Main\", \"file\": \"audio/mb.wav\", "
                          "\"convert\": {\"default\": {\"codec\": \"PCMF\"}}}";
    const char* tempo = "\"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 4, \"beatUnit\": 4}]";
    f["segments/SegA.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(22) + "\", \"name\": \"SegA\", \"bank\": \"Main\", \"asset\": \"ma\", "
                              "\"bars\": 4, " + tempo + "}";
    f["segments/SegB.json"] = "{\"type\": \"segment\", \"guid\": \"" + G(23) + "\", \"name\": \"SegB\", \"bank\": \"Main\", \"asset\": \"mb\", "
                              "\"bars\": 4, " + tempo + "}";
    f["playlists/PA.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(24) + "\", \"name\": \"PA\", \"bank\": \"Main\", "
                             "\"entries\": [{\"segment\": \"SegA\"}]}";
    f["playlists/PB.json"] = "{\"type\": \"playlist\", \"guid\": \"" + G(25) + "\", \"name\": \"PB\", \"bank\": \"Main\", "
                             "\"entries\": [{\"segment\": \"SegB\"}]}";
    f["events/Song.json"] =
        "{\"type\": \"event\", \"guid\": \"" + G(26) + "\", \"name\": \"Song\", \"bank\": \"Main\", \"bus\": \"Music\", \"music\": {"
        "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PA\"}, {\"name\": \"Battle\", \"playlist\": \"PB\"}],"
        "\"transitions\": [{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBar\"}]}}";
  }
  return f;
}

inline void LoadLive(snd::tool::ProjectDocument& doc, bool withMusic = false) {
  doc.LoadFiles(LiveFiles(withMusic));
  doc.SetBinaryFile("audio/dc.wav", doctest::ToneWav(4800, 48000, 1, kDc));
  if (withMusic) {
    doc.SetBinaryFile("audio/ma.wav", doctest::ToneWav(8 * 48000, 48000, 1, kDc));
    doc.SetBinaryFile("audio/mb.wav", doctest::ToneWav(8 * 48000, 48000, 1, 2.0f * kDc));
  }
  doc.SetRandom(doctest::Counter{});
}

// A test double for core's structural editor (request C-01): says yes, changes nothing.
class YesEditor final : public snd::livelink::IStructEditor {
 public:
  snd_result Apply(snd::core::System&, const snd::livelink::StructEditRequest&) noexcept override {
    ++applied;
    return SND_OK;
  }
  snd_result Undo(snd::core::System&, const snd::livelink::StructEditRequest&) noexcept override {
    ++undone;
    return SND_OK;
  }
  std::uint32_t applied = 0, undone = 0;
};

inline snd::tool::LiveSyncOptions MakeOptions(snd::tool::LiveDecision d = snd::tool::LiveDecision::Ask) {
  snd::tool::LiveSyncOptions o;
  o.decision = d;
  o.throttleMs = 0;
  return o;
}

// The preview engine running the document's build, a Connection to it and LiveSync.
struct Rig {
  snd::tool::ProjectDocument doc;
  snd::tool::DocBuild gameBuild;  // what the engine was started with
  std::unique_ptr<snd::tool::preview::PreviewEngine> engine;
  snd::tool::Connection conn;
  std::unique_ptr<snd::tool::LiveSync> sync;
  std::vector<float> out;  // the last rendered block
  bool ok = false;

  explicit Rig(bool withMusic = false, snd::tool::preview::PreviewEngineDesc ed = {},
               snd::tool::LiveSyncOptions so = MakeOptions()) {
    LoadLive(doc, withMusic);
    gameBuild = snd::tool::BuildDocument(doc);
    if (!gameBuild.ok) {
      for (const auto& i : gameBuild.issues) std::printf("build: %s\n", i.text.c_str());
      return;
    }
    std::string err;
    engine = snd::tool::preview::PreviewEngine::Create(ed, &err);
    if (engine == nullptr) {
      std::printf("engine: %s\n", err.c_str());
      return;
    }
    if (!engine->LoadBuild(gameBuild, &err)) {
      std::printf("load: %s\n", err.c_str());
      return;
    }
    snd::tool::preview::PreviewEngine* e = engine.get();
    if (!so.writer) {
      so.writer = [e](const snd::tool::BankImage& b, std::string* path, std::string*) {
        *path = e->PutRebuiltBank(b.file, b.bytes);
        return true;
      };
    }
    sync = std::make_unique<snd::tool::LiveSync>(doc, so);
    sync->SetBaseline(gameBuild);
    ok = true;
  }
  snd::tool::ConnectionDesc Desc() {
    snd::tool::ConnectionDesc d;
    d.connector = engine->connector();
    d.reconnectDelayMs = 20;
    d.name = "live-test";
    d.profilerMask = 0;
    return sync->Prepare(d);
  }
  bool Connect() { return conn.Open(Desc()) == SND_OK; }
  // One frame: render a block, poll the connection, tick LiveSync.
  void Frame() {
    out.clear();
    engine->Render(1, &out);
    const snd::tool::ConnectionEvents ev = conn.Poll();
    sync->Tick(conn, ev, Now());
  }
  template <class Pred>
  bool Run(Pred&& pred, std::uint32_t timeoutMs = 60000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > end) return false;
      Frame();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
  }
  void Frames(std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i) Frame();
  }
  bool Live() {
    Frame();  // the document may have changed since the last Tick
    return Run([&] { return sync->phase() == snd::tool::LivePhase::Live; });
  }
  // The game holds the document's build (nothing queued, nothing in flight) and the output settled.
  bool Settled() {
    const bool r = Live();
    Frames(6);
    return r;
  }
  float Left() const { return out.empty() ? 0.0f : out[out.size() - 2]; }
  float Right() const { return out.empty() ? 0.0f : out[out.size() - 1]; }
};

}  // namespace livetest

#endif  // SND_TOOL_TEST_LIVE_FIXTURE_HPP
