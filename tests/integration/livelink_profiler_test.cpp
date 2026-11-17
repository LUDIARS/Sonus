// Live link profiler stream (11.1 profiler, 11.2 game -> tool) and its
// back-pressure: a full channel or a stalled tool drops (and counts) profiler
// data; the audio thread and the game never wait.
#include <algorithm>
#include <chrono>

#include "livelink_fixture.hpp"

using namespace lltest;
using snd::HashId;

namespace {
constexpr std::uint32_t kAll = ll::kProfVoices | ll::kProfTiming | ll::kProfMemory | ll::kProfStreams | ll::kProfLod |
                               ll::kProfEventLog | ll::kProfMeters;
}  // namespace

SND_TEST(LivelinkProfiler, StreamsVoicesMetersTimingMemoryAndTheEventLog) {
  Fixture f;
  f.Load(DevBank());
  ll::ServerDesc sd;
  sd.memoryHz = 20;
  SND_REQUIRE_EQ(f.StartServer(sd), SND_OK);
  ll::Client* c = nullptr;
  SND_REQUIRE_EQ(ll::Client::Create(MemoryClient(f.link), &c), SND_OK);
  c->SetProfilerMask(kAll, 60);  // remembered: sent once connected
  c->Start();
  SND_REQUIRE(f.StepUntil([&] { return c->State() == ll::ClientState::Ready; }));
  f.StepUntil([&] { return f.rt->ProfilerMask() == kAll; });
  const snd_playback_handle p = f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  f.Steps(8);
  const float level = f.LastLeft();
  SND_CHECK(f.StepUntil([&] {
    return c->Received(ll::MessageType::VoiceList) > 2 && c->Received(ll::MessageType::BusMeters) > 2 &&
           c->Received(ll::MessageType::BlockTiming) > 2 && c->Received(ll::MessageType::MemoryUsage) > 0 &&
           c->Received(ll::MessageType::EventLog) > 0;
  }));
  ll::VoiceInfo v[8];
  const std::uint32_t nv = c->CopyVoices(v, 8);
  SND_CHECK_EQ(nv, 1u);
  if (nv == 1u) {
    SND_CHECK_EQ(v[0].eventId, HashId("Tone"));
    SND_CHECK_EQ(v[0].playbackSlot, static_cast<std::uint32_t>(p.value & 0xFFFFFFFFu));
    SND_CHECK_EQ(v[0].busId, HashId("Sfx"));
    SND_CHECK_EQ(v[0].isVirtual, 0u);
    SND_CHECK_EQ(v[0].playbackState, static_cast<std::uint32_t>(SND_PLAYBACK_STATE_PLAYING));
  }
  ll::MeterEntry m[8];
  const std::uint32_t nm = c->CopyMeters(m, 8);
  SND_CHECK_EQ(nm, 2u);  // master + Sfx
  bool sawMaster = false;
  for (std::uint32_t i = 0; i < nm; ++i) {
    if (m[i].busId == 0u) {  // master (SND_ID_NONE): the output level
      sawMaster = true;
      SND_CHECK_NEAR(m[i].peak[0], level, 1e-5);
      SND_CHECK_NEAR(m[i].rms[0], level, level * 0.01f);  // RMS window still settling
    }
  }
  SND_CHECK(sawMaster);
  std::vector<ll::MeterSample> hist(512);
  SND_CHECK(c->CopyMeterHistory(HashId("Sfx"), hist.data(), 512) >= 2u);
  std::vector<ll::TimingSample> ts(512);
  const std::uint32_t nt = c->CopyTimings(ts.data(), 512);
  SND_CHECK(nt >= 2u);
  for (std::uint32_t i = 1; i < nt; ++i) SND_CHECK(ts[i].timing.blockIndex > ts[i - 1].timing.blockIndex);
  SND_CHECK(nt == 0u || ts[nt - 1].timing.budgetMicroseconds > 0.0f);
  ll::MemoryUsage mu{};
  SND_CHECK(c->LatestMemory(&mu));
  SND_CHECK(mu.used[SND_MEM_TABLES] > 0u);
  std::vector<ll::LogSample> log(4096);
  const std::uint32_t nl = c->CopyEventLog(log.data(), 4096);
  bool sawPlay = false;
  for (std::uint32_t i = 0; i < nl; ++i) {
    const ll::LogEntry& e = log[i].entry;
    if (e.kind == (ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play)) && e.id == HashId("Tone")) {
      sawPlay = true;
    }
  }
  SND_CHECK(sawPlay);
  // The playback's end is logged with its reason.
  f.sys->Stop(p, 0.0f, nullptr);
  SND_CHECK(f.StepUntil([&] {
    const std::uint32_t n = c->CopyEventLog(log.data(), 4096);
    for (std::uint32_t i = 0; i < n; ++i) {
      if (log[i].entry.kind == ll::kLogPlaybackEnded && log[i].entry.handleSlot == (p.value & 0xFFFFFFFFu)) return true;
    }
    return false;
  }));
  ll::Client::Destroy(c);
}

// The audio-side channels are bounded: with nobody draining them the hooks drop and
// count exactly what did not fit, and RenderBlock never waits.
SND_TEST(LivelinkProfiler, FullChannelsDropAndCountExactly) {
  ll::RuntimeDesc rd;
  rd.timingEntries = 4;  // rounded to a power of two (4)
  rd.logEntries = 8;
  Fixture f(Hooks::Live, Config(), nullptr, rd);
  f.Load(DevBank());
  f.rt->SetProfiler(ll::kProfTiming | ll::kProfEventLog, 30);
  const ll::RuntimeDrops before = f.rt->Drops();
  for (int i = 0; i < 20; ++i) f.sys->Play(HashId("Blip"), snd_emitter_handle{0}, nullptr);
  f.Steps(100);
  const ll::RuntimeDrops d = f.rt->Drops();
  SND_CHECK_EQ(d.timing - before.timing, 100u - 4u);  // 4 fit, the rest is counted
  // log: 20 Play commands + 20 ends (+ warnings: none expected) into 8 slots
  SND_CHECK_EQ(d.log - before.log + 8u, 40u);
  // Draining makes room again.
  ll::BlockTiming t{};
  std::uint32_t n = 0;
  while (f.rt->timing().TryPop(t)) ++n;
  SND_CHECK_EQ(n, 4u);
  f.Steps(2);
  while (f.rt->timing().TryPop(t)) ++n;
  SND_CHECK_EQ(n, 6u);
}

// A tool that stops reading: the server's queues fill, profiler messages are dropped
// and counted, the game keeps rendering at full speed.
SND_TEST(LivelinkProfiler, StalledToolDropsProfilerDataWithoutStallingTheGame) {
  Fixture f;
  f.link.Init(snd::base::DefaultAllocator(), 4096);  // a tiny pipe
  f.Load(DevBank());
  ll::ServerDesc sd;
  sd.acceptor = &f.link.acceptor();
  sd.sendBufferBytes = 16u << 10;
  sd.meterHz = 200;
  sd.timingHz = 500;
  sd.timeoutMs = 60000;
  sd.system = f.sys;
  sd.runtime = f.rt;
  sd.allocator = &snd::base::DefaultAllocator();
  SND_REQUIRE_EQ(ll::Server::Create(sd, &f.server), SND_OK);
  SND_REQUIRE_EQ(f.server->Start(), SND_OK);
  RawPeer p;
  SND_REQUIRE(p.Connect(f.link.connector()));
  SND_REQUIRE(p.Hello());
  SND_REQUIRE(p.WaitFor(ll::MessageType::HelloReply) != nullptr);
  const ll::SetProfilerMask m{kAll, 200};
  SND_REQUIRE(p.Send(ll::MessageType::SetProfilerMask, &m, sizeof(m)));
  for (int i = 0; i < 8; ++i) f.sys->Play(HashId("Tone"), snd_emitter_handle{0}, nullptr);
  // From now on the peer never reads.
  double worstMs = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500)) {
    const auto a = std::chrono::steady_clock::now();
    f.Step();
    worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  const ll::ServerStats st = f.server->Stats();
  SND_CHECK(st.profilerDropped > 0u);
  SND_CHECK(st.profilerSent > 0u);
  SND_CHECK(f.server->Connected());  // still attached: dropping is not an error
  SND_CHECK_MSG(worstMs < 50.0, "a block took %.2f ms", worstMs);
  // The peer reads again: the stream resumes on frame boundaries.
  std::size_t before = p.frames.size();
  SND_CHECK(f.StepUntil([&] {
    p.Pump(1);
    return p.frames.size() > before + 20u;
  }));
  for (const RawPeer::Frame& fr : p.frames) SND_CHECK(ll::IsKnown(fr.header.type));
}
