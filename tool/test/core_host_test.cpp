// The tool's core used the way an outside host (a game engine's editor) uses it
// (D-101): only snd/tool/session.hpp, no Tela, no live link header. The Session is
// created on one thread and driven from another (the host's own loop) with Tick();
// the game renders on a third thread meanwhile. The host reads meters, voices and
// timings, mutes / solos a bus, sees the override and reverts it, and records.
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include "snd/test/test.hpp"
#include "snd/tool/session.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using tooltest::Bus;
using tooltest::LiveRig;
using tooltest::ToolProject;

namespace {

// The host's polling loop: Tick() until pred() or 60 s.
template <class Pred>
bool Poll(Session& s, Pred&& pred) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    s.Tick();
    if (pred()) return true;
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

float MasterPeak(const Session& s) {
  const BusMeter* m = s.profiler().FindMeter(0u);  // 0: the output (protocol)
  return m != nullptr && !m->stale ? m->peak[0] : -1.0f;
}

struct HostResult {
  bool online = false;
  bool meters = false;
  bool voices = false;
  bool timings = false;
  bool muted = false;
  bool overrideShown = false;
  bool reverted = false;
  bool soloed = false;
  bool recorded = false;
  bool saved = false;
  bool loaded = false;
  std::uint32_t realVoices = 0;
  std::uint32_t timingSamples = 0;
  float before = 0.0f, mutedPeak = 0.0f, soloPeak = 0.0f, after = 0.0f;
};

}  // namespace

SND_TEST(ToolCoreHost, AHostThreadPollsTheSessionWhileTheGameRuns) {
  LiveRig rig;
  ToolProject tool;
  SND_REQUIRE(rig.ok && tool.ok);
  rig.Play("Tone");
  rig.Play("Pad");
  rig.f.Steps(32);
  const float level = rig.Level();
  SND_REQUIRE(level > 0.1f);

  // Created here, used only by the host thread below.
  Session s;
  s.UseProject(tool.project);
  const std::int32_t sfx = Bus(tool.project, "Sfx");
  const std::int32_t music = Bus(tool.project, "Music");
  SND_REQUIRE(sfx >= 0 && music >= 0);
  const std::string path = (tool.dir.root() / "host.snrc").string();
  ConnectionDesc desc = rig.Conn(tool.project.buildHash());
  desc.meterHz = 200;

  std::atomic<bool> done{false};
  HostResult r;
  std::thread host([&] {
    const auto finish = [&] { done.store(true, std::memory_order_release); };
    if (s.Connect(desc) != SND_OK) return finish();
    r.online = Poll(s, [&] { return s.connection().state() == LinkState::Online; });
    if (!r.online) return finish();
    // Profiler: the master meter shows the steady output; voices and timings arrive.
    r.meters = Poll(s, [&] { return std::fabs(MasterPeak(s) - level) < level * 1e-3f; });
    r.before = MasterPeak(s);
    r.voices = Poll(s, [&] { return s.profiler().realVoices() >= 2u; });
    r.realVoices = s.profiler().realVoices();
    r.timings = Poll(s, [&] { return s.profiler().Summary().samples > 0u; });
    r.timingSamples = s.profiler().Summary().samples;
    // Mixer: muting Sfx leaves Music's half on the master.
    s.mixer().Set(Target{Control::BusMute, sfx, 0, 0}, 1.0f);
    r.muted = Poll(s, [&] { return std::fabs(MasterPeak(s) - level * 0.5f) < level * 1e-3f; });
    r.mutedPeak = MasterPeak(s);
    for (const OverrideItem& o : s.mixer().Overrides()) {
      if (o.target == Target{Control::BusFader, sfx, 0, 0} && o.muted) r.overrideShown = true;
    }
    // Revert everything: the full output returns, nothing is overridden.
    s.mixer().RevertAll();
    r.reverted = Poll(s, [&] { return std::fabs(MasterPeak(s) - level) < level * 1e-3f; }) &&
                 s.mixer().Overrides().empty();
    // Solo Music: Sfx is solo-muted.
    s.mixer().Set(Target{Control::BusSolo, music, 0, 0}, 1.0f);
    r.soloed = s.mixer().SoloMuted(sfx) &&
               Poll(s, [&] { return std::fabs(MasterPeak(s) - level * 0.5f) < level * 1e-3f; });
    r.soloPeak = MasterPeak(s);
    s.mixer().Set(Target{Control::BusSolo, music, 0, 0}, 0.0f);
    r.after = Poll(s, [&] { return std::fabs(MasterPeak(s) - level) < level * 1e-3f; }) ? MasterPeak(s) : 0.0f;
    // Recording through the Session (no live link client in the host's code).
    if (s.StartRecording() &&
        Poll(s, [&] { return s.profiler().recordingState() == RecordingState::Recording; }) &&
        Poll(s, [&] { return s.profiler().recordedBytes() > 0u; }) && s.StopRecording() &&
        Poll(s, [&] { return s.profiler().recordingState() == RecordingState::Ready; })) {
      r.recorded = true;
      r.saved = s.SaveRecording(path);
      r.loaded = r.saved && s.LoadRecording(path) && s.profiler().loadedSummary().valid &&
                 s.profiler().loaded().info == s.CurrentRecordingInfo();
    }
    s.Disconnect();
    finish();
  });
  // The game: renders until the host is done.
  const bool finished = rig.f.StepUntil([&] { return done.load(std::memory_order_acquire); }, 600000);
  host.join();
  SND_REQUIRE(finished);

  SND_CHECK(r.online);
  SND_CHECK(r.meters);
  SND_CHECK_NEAR(r.before, level, level * 1e-3);
  SND_CHECK(r.voices);
  SND_CHECK(r.realVoices >= 2u);
  SND_CHECK(r.timings);
  SND_CHECK(r.muted);
  SND_CHECK_NEAR(r.mutedPeak, level * 0.5f, level * 1e-3);
  SND_CHECK(r.overrideShown);
  SND_CHECK(r.reverted);
  SND_CHECK(r.soloed);
  SND_CHECK_NEAR(r.soloPeak, level * 0.5f, level * 1e-3);
  SND_CHECK_NEAR(r.after, level, level * 1e-3);
  SND_CHECK(r.recorded);
  SND_CHECK(r.saved);
  SND_CHECK(r.loaded);
  SND_CHECK_EQ(s.connection().state(), LinkState::Offline);
  // The game's output is back to the project's mix.
  rig.f.Steps(16);
  SND_CHECK_NEAR(rig.Level(), level, level * 1e-4);
}

SND_TEST(ToolCoreHost, NowNsIsTheClockOfTick) {
  const std::uint64_t a = Session::NowNs();
  const std::uint64_t b = tooltest::Now();  // platform::MonotonicNanoseconds
  const std::uint64_t c = Session::NowNs();
  SND_CHECK(a <= b && b <= c);
  // Without a connection Tick() only counts and the recording calls refuse.
  Session s;
  s.Tick();
  s.Tick(Session::NowNs());
  SND_CHECK_EQ(s.ticks(), 2u);
  SND_CHECK(!s.StartRecording());
  SND_CHECK(!s.StopRecording());
  SND_CHECK_EQ(s.profiler().recordingState(), RecordingState::Idle);
}
