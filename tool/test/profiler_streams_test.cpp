// The rest of the profiler (group P, design.md 11.1): CPU and its detail, memory tags, streams,
// LOD, the event log with its filter, read through the client's update numbers, against a real
// core and live link server (tool_fixture.hpp).
#include <algorithm>

#include "snd/internal/commands.hpp"
#include "snd/livelink/runtime.hpp"
#include "snd/test/test.hpp"
#include "snd/tool/profiler_streams.hpp"
#include "snd/tool/session.hpp"
#include "tool_fixture.hpp"

using namespace snd::tool;
namespace ll = snd::livelink;

SND_TEST(ToolProfilerStreams, MaskFollowsTheViewsAndTheGamesVersion) {
  const std::uint32_t all = kViewAll;
  SND_CHECK_EQ(ProfilerMaskFor(all, 1), ll::ProfilerMaskOf(1));
  SND_CHECK_EQ(ProfilerMaskFor(all, 0), ll::ProfilerMaskOf(0));  // no StreamList / BlockDetail for 1.0
  SND_CHECK_EQ(ProfilerMaskFor(kViewMemory, 1), static_cast<std::uint32_t>(ll::kProfMemory));
  SND_CHECK_EQ(ProfilerMaskFor(kViewCpuDetail, 1), static_cast<std::uint32_t>(ll::kProfTiming | ll::kProfBlockDetail));
  SND_CHECK_EQ(ProfilerMaskFor(kViewCpuDetail, 0), static_cast<std::uint32_t>(ll::kProfTiming));
  ProfilerStreams p;
  std::string why;
  SND_CHECK(!p.Available(kViewCpuDetail, &why));  // nothing connected yet: minor 0
  SND_CHECK(!why.empty());
  SND_CHECK(p.Available(kViewMemory));
  SND_CHECK(std::string(MemTagName(SND_MEM_BANK)) == "bank");
  SND_CHECK(std::string(MemTagName(99)) == "?");
  SND_CHECK(LogEntryKindName(ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play)) == "cmd Play");
  SND_CHECK(LogEntryKindName(ll::kLogPlaybackEnded) == "playback ended");
  SND_CHECK(LogEntryKindName(2) == "marker");
  LogFilter f;
  ll::LogEntry e{};
  e.kind = ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play);
  e.id = 7;
  SND_CHECK(f.Matches(e));
  f.commands = false;
  SND_CHECK(!f.Matches(e));
  f.commands = true;
  f.id = 8;
  SND_CHECK(!f.Matches(e));
  f.id = 7;
  f.kinds = {ll::kLogPlaybackEnded};
  SND_CHECK(!f.Matches(e));
}

SND_TEST(ToolProfilerStreams, DetailViewsFillFromTheGame) {
  tooltest::LiveRig rig;
  tooltest::ToolProject tool;
  SND_REQUIRE(rig.ok && tool.ok);
  Session s;
  s.UseProject(tool.project);
  SND_REQUIRE_EQ(s.Connect(rig.Conn(tool.project.buildHash())), SND_OK);
  SND_REQUIRE(rig.Run(s, [&] { return s.connection().state() == LinkState::Online; }));
  ProfilerStreams p;
  p.SetViews(kViewCpu | kViewCpuDetail | kViewMemory | kViewStreams | kViewLod | kViewEventLog);
  // The subscription first (the game streams the event log only once the mask is in), then the
  // sounds: otherwise, under load, Blip can end before the log is subscribed.
  SND_REQUIRE(rig.Run(s, [&] {
    p.Poll(s.connection().client(), tooltest::Now());
    return p.subscribedMask() == ProfilerMaskFor(p.views(), 1) && p.cpu().size() >= 1u;
  }));
  rig.f.sys->Play(snd::HashId("Tone"), snd_emitter_handle{0}, nullptr);
  const snd::Id blip = snd::HashId("Blip");
  rig.f.sys->Play(blip, snd_emitter_handle{0}, nullptr);
  const bool filled = rig.Run(s, [&] {
    p.Poll(s.connection().client(), tooltest::Now());
    return p.cpu().size() >= 3u && !p.cpuDetail().empty() && p.hasMemory() && !p.streams().empty() &&
           !p.Log(LogFilter{{}, true, false, true, blip, ~0u}).empty();
  });
  SND_REQUIRE(filled);
  SND_CHECK_EQ(p.peerMinor(), 1u);
  SND_CHECK_EQ(p.subscribedMask(), ProfilerMaskFor(p.views(), 1));
  for (const CpuPoint& c : p.cpu()) {
    SND_CHECK(c.timing.budgetMicroseconds > 0.0f);
    SND_CHECK_NEAR(c.load, (c.timing.logicMicroseconds + c.timing.renderMicroseconds) / c.timing.budgetMicroseconds, 1e-6);
  }
  SND_CHECK(p.cpuDetail().back().detail.budgetMicroseconds > 0.0f);
  SND_CHECK_EQ(p.Memory().size(), static_cast<std::size_t>(SND_MEM_TAG_COUNT));
  SND_CHECK(p.totalUsed() > 0u);
  SND_CHECK_EQ(p.streams().size(), static_cast<std::size_t>(rig.f.cfg_.max_streams));
  SND_CHECK_EQ(p.lodLevel(), 0u);  // the fixture runs without load LOD
  // The log: Blip's Play (a command the audio thread took) and, after its 2400 frames, its end.
  const std::vector<LogPoint> plays = p.Log(LogFilter{{}, true, false, false, blip, ~0u});
  SND_REQUIRE(!plays.empty());
  SND_CHECK_EQ(plays[0].entry.kind, ll::kLogCommandBit | static_cast<std::uint32_t>(snd::cmd::CommandType::Play));
  SND_CHECK(rig.Run(s, [&] {
    p.Poll(s.connection().client(), tooltest::Now());
    LogFilter ends;
    ends.commands = false;
    ends.events = false;
    ends.slot = plays[0].entry.handleSlot;
    return !p.Log(ends).empty();
  }));
  SND_CHECK(p.cpuCount().received >= p.cpu().size());
  SND_CHECK(p.logCount().received >= 1u);
  SND_CHECK_EQ(p.logCount().missed, 0u);  // polled every frame: nothing pushed out unread
  // Fewer views: the mask follows.
  p.SetViews(kViewMemory);
  p.Poll(s.connection().client(), tooltest::Now());
  SND_CHECK_EQ(p.subscribedMask(), static_cast<std::uint32_t>(ll::kProfMemory));
  s.Disconnect();
  p.Poll(nullptr, tooltest::Now());  // offline: nothing changes, no crash
  SND_CHECK(!p.cpu().empty());
}
