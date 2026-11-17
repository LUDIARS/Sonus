// The profiler model against a real game: what it shows equals what the game
// rendered (meters vs the output samples), timings / voices / memory / event
// log arrive, and a recording survives the save / load round trip.
#include <cmath>
#include <chrono>
#include <cstdio>
#include <thread>

#include "snd/livelink/client.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/names.hpp"
#include "snd/tool/session.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
using tooltest::Bus;
using tooltest::LiveRig;
using tooltest::ToolProject;
namespace ll = snd::livelink;

namespace {

struct Setup {
  LiveRig rig;
  ToolProject tool;
  Session s;
  Setup() {
    SND_REQUIRE(rig.ok && tool.ok);
    s.UseProject(tool.project);
    ConnectionDesc d = rig.Conn(tool.project.buildHash());
    d.meterHz = 200;
    SND_REQUIRE_EQ(s.Connect(d), SND_OK);
    SND_REQUIRE(rig.Run(s, [&] { return s.connection().state() == LinkState::Online; }));
  }
};

// Peak and RMS of the left channel of the last rendered block.
void BlockLevels(const std::vector<float>& out, double* peak, double* rms) {
  double p = 0, sum = 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < out.size(); i += 2) {
    p = std::max(p, std::fabs(static_cast<double>(out[i])));
    sum += static_cast<double>(out[i]) * out[i];
    ++n;
  }
  *peak = p;
  *rms = std::sqrt(sum / static_cast<double>(n));
}

}  // namespace

SND_TEST(ToolLiveProfiler, MetersEqualTheRenderedOutput) {
  Setup t;
  t.rig.Play("Tone");
  t.rig.Play("Pad");
  t.rig.Settle(t.s, 32);
  double peak = 0, rms = 0;
  BlockLevels(t.rig.f.out, &peak, &rms);
  SND_REQUIRE(peak > 0.1);
  // Master is reported with id 0 (SND_ID_NONE): the output.
  SND_REQUIRE(t.rig.Run(t.s, [&] {
    const BusMeter* m = t.s.profiler().FindMeter(0u);
    return m != nullptr && std::fabs(m->rms[0] - peak) < peak * 1e-4;
  }));
  const BusMeter* master = t.s.profiler().FindMeter(0u);
  SND_CHECK_NEAR(master->peak[0], peak, peak * 1e-5);
  SND_CHECK_NEAR(master->rms[0], rms, rms * 1e-4);
  SND_CHECK_EQ(master->channels, 2u);
  SND_CHECK_NEAR(master->holdDb[0], 20.0 * std::log10(peak), 1e-3);
  SND_CHECK(!master->clipped);
  // Each bus carries its event's half (same DC, same pan); Sfx has the -96 dB wet path on top.
  const BusMeter* music = t.s.profiler().FindMeter(snd::HashId("Music"));
  const BusMeter* sfx = t.s.profiler().FindMeter(snd::HashId("Sfx"));
  SND_REQUIRE(music != nullptr && sfx != nullptr);
  SND_CHECK_NEAR(music->peak[0] + sfx->peak[0], peak, peak * 1e-4);
  // A fader move shows in the meters as in the output.
  t.s.mixer().Set(Target{Control::BusFader, Bus(t.tool.project, "Music"), 0, 0}, -20.0f);
  SND_REQUIRE(t.rig.Run(t.s, [&] { return t.s.sync().Count(CellState::Synced) == t.s.sync().cells().size() + 1u; }));
  t.rig.Settle(t.s, 32);
  BlockLevels(t.rig.f.out, &peak, &rms);
  SND_REQUIRE(t.rig.Run(t.s, [&] {
    const BusMeter* m = t.s.profiler().FindMeter(0u);
    return m != nullptr && std::fabs(m->peak[0] - peak) < peak * 1e-5 && std::fabs(m->rms[0] - peak) < peak * 1e-4;
  }));
  music = t.s.profiler().FindMeter(snd::HashId("Music"));
  sfx = t.s.profiler().FindMeter(snd::HashId("Sfx"));
  SND_CHECK_NEAR(music->peak[0] / sfx->peak[0], std::pow(10.0, -1.0) / (1.0 + 1.5848931924611107e-05), 1e-4);
}

SND_TEST(ToolLiveProfiler, TimingsVoicesMemoryAndEventLog) {
  Setup t;
  // The profiler mask is applied by the game shortly after the handshake: commands recorded
  // before that are not logged. Play once the game streams (the mask is in effect).
  SND_REQUIRE(t.rig.Run(t.s, [&] { return t.s.profiler().received(ll::MessageType::BlockTiming) > 0u; }));
  t.rig.Play("Tone");
  SND_REQUIRE(t.rig.Run(t.s, [&] {
    return t.s.profiler().timings().size() > 4u && !t.s.profiler().voices().empty() && t.s.profiler().hasMemory() &&
           !t.s.profiler().log().empty();
  }));
  const auto& ts = t.s.profiler().timings();
  for (std::size_t i = 1; i < ts.size(); ++i) SND_CHECK(ts[i].timing.blockIndex > ts[i - 1].timing.blockIndex);
  const TimingSummary sum = t.s.profiler().Summary();
  SND_CHECK_EQ(sum.samples, static_cast<std::uint32_t>(ts.size()));
  SND_CHECK(sum.budgetUs > 0.0f && sum.budgetUs <= 256.0f / 48000.0f * 1e6f);  // a share of the block period
  SND_CHECK_EQ(sum.budgetUs, ts.back().timing.budgetMicroseconds);
  SND_CHECK(sum.maxRenderUs >= sum.avgRenderUs);
  SND_CHECK_EQ(sum.lastBlock, ts.back().timing.blockIndex);
  const auto& v = t.s.profiler().voices();
  SND_CHECK_EQ(v[0].eventId, snd::HashId("Tone"));
  SND_CHECK_EQ(v[0].busId, snd::HashId("Sfx"));
  SND_CHECK_EQ(t.s.profiler().realVoices(), 1u);
  SND_CHECK(t.s.profiler().memory().used[SND_MEM_TABLES] > 0u);
  bool sawPlay = false;
  for (const LogLine& l : t.s.profiler().log()) {
    if (LogKindName(l.entry.kind) == "cmd Play" && l.entry.id == snd::HashId("Tone")) sawPlay = true;
  }
  SND_CHECK(sawPlay);
  SND_CHECK(t.s.profiler().received(ll::MessageType::BlockTiming) > 0u);
}

SND_TEST(ToolLiveProfiler, RecordingSaveAndLoadRoundTrip) {
  Setup t;
  t.rig.Play("Tone");
  Profiler& p = t.s.profiler();
  SND_REQUIRE(p.StartRecording(t.s.connection().client()));
  SND_REQUIRE(t.rig.Run(t.s, [&] { return p.recordingState() == RecordingState::Recording; }));
  // The game starts recording at a block boundary after the Ack: its first record (LOD) has
  // arrived once the recording is really running. Commands after that are recorded.
  SND_REQUIRE(t.rig.Run(t.s, [&] { return p.recordedBytes() > 0u; }));
  t.s.mixer().Set(Target{Control::BusFader, Bus(t.tool.project, "Sfx"), 0, 0}, -6.0f);
  t.rig.f.sys->Play(snd::HashId("Blip"), snd_emitter_handle{0}, nullptr);
  t.rig.Settle(t.s, 32);
  SND_REQUIRE(p.StopRecording(t.s.connection().client()));
  SND_REQUIRE(t.rig.Run(t.s, [&] { return p.recordingState() == RecordingState::Ready; }));
  SND_CHECK(p.recordedBytes() > 0u);
  tooltest::ProjectDir dir("rec");
  const std::string path = (dir.root() / "take.snrec").string();
  SND_REQUIRE(t.s.SaveRecording(path));
  std::vector<std::uint8_t> live(t.s.connection().client()->RecordingBytes());
  t.s.connection().client()->CopyRecording(live.data(), live.size());
  SND_REQUIRE(t.s.LoadRecording(path));
  SND_CHECK(p.loaded().records == live);
  const RecordingInfo& info = p.loaded().info;
  SND_CHECK_EQ(info.projectBuildHash, t.tool.project.buildHash());
  SND_CHECK_EQ(info.runtimeBuildHash, t.rig.gameProj.buildHash());
  SND_CHECK_EQ(info.sampleRate, 48000u);
  SND_CHECK_EQ(info.blockSize, 256u);
  SND_CHECK_EQ(info.flags, 0u);
  const RecordingSummary& s = p.loadedSummary();
  SND_CHECK(s.valid);
  SND_CHECK(s.arenas > 0u);
  SND_CHECK(s.lods >= 1u);
  SND_CHECK(s.bankMaps >= 1u);
  SND_CHECK(s.lastBlock >= s.firstBlock);  // commands may all fall into one block under load
  // A damaged file is refused and the previous load stays.
  std::vector<std::uint8_t> bytes = tooltest::ReadAll(path);
  bytes[bytes.size() / 2] ^= 0x40u;
  {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
  }
  SND_CHECK(!t.s.LoadRecording(path));
  SND_CHECK(p.loaded().records == live);
}

SND_TEST(ToolLiveProfiler, StaleMetersFallToMinusInfinity) {
  Setup t;
  t.rig.Play("Tone");
  SND_REQUIRE(t.rig.Run(t.s, [&] {
    const BusMeter* m = t.s.profiler().FindMeter(0u);
    return m != nullptr && m->peak[0] > 0.1f;
  }, 20000));
  // The game is no longer rendered: wait until meters already in flight have arrived
  // (the count stays the same for 300 ms), then poll at analytic times.
  ll::Client* c = t.s.connection().client();
  std::uint64_t count = c->Received(ll::MessageType::BusMeters);
  for (auto quiet = std::chrono::steady_clock::now();
       std::chrono::steady_clock::now() - quiet < std::chrono::milliseconds(300);) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::uint64_t n = c->Received(ll::MessageType::BusMeters);
    if (n != count) {
      count = n;
      quiet = std::chrono::steady_clock::now();
    }
  }
  t.s.profiler().Poll(c, tooltest::Now());  // takes the last meter (fresh: restarts the hold)
  const std::uint64_t now = t.s.profiler().FindMeter(0u)->receivedNs;  // ages count from its arrival
  // No new meters: 400 ms later still fresh, 10 s later gone.
  t.s.profiler().Poll(t.s.connection().client(), now + 400000000ull);
  const BusMeter* m = t.s.profiler().FindMeter(0u);
  SND_CHECK(!m->stale && m->peak[0] > 0.1f);
  t.s.profiler().Poll(t.s.connection().client(), now + 10000000000ull);
  m = t.s.profiler().FindMeter(0u);
  SND_CHECK(m->stale);
  SND_CHECK_EQ(m->peak[0], 0.0f);
  SND_CHECK_EQ(m->rms[0], 0.0f);
  SND_CHECK_EQ(m->holdDb[0], kMeterFloorDb);
}
