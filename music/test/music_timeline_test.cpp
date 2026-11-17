// Time line of a transport (8.1, 8.2): clock <-> tick conversion across tempo
// points, signatures, grids, overrides, pause / seek / stall delay. Analytic
// expectations at 48 kHz (120 BPM: 24000 samples per quarter = 960 ticks).
#include <cmath>

#include "snd/music/music_system.hpp"
#include "snd/test/test.hpp"
#include "timeline.hpp"

using snd::music::detail::Timeline;

namespace {
double ClockOf(const Timeline& t, double tick) {
  double c = -1.0;
  return t.ClockAtTick(tick, &c) ? c : -1.0;
}
}  // namespace

SND_TEST(MusicTimeline, ConstantTempoGrids) {
  Timeline t;
  t.Reset(48000.0, 120.0, 4, 4);
  t.StartAt(1000, 0.0);
  SND_CHECK_NEAR(ClockOf(t, 960.0), 25000.0, 1e-6);
  SND_CHECK_NEAR(t.TickAtClock(49000.0), 1920.0, 1e-9);
  SND_CHECK_NEAR(t.TickAtClock(0.0), -40.0, 1e-9);  // before the anchor
  SND_CHECK_EQ(t.NextBar(1.0), 3840.0);
  SND_CHECK_EQ(t.NextBar(3840.0), 3840.0);  // on the grid: itself
  SND_CHECK_EQ(t.NextBeat(960.0), 960.0);
  SND_CHECK_EQ(t.NextBeat(961.0), 1920.0);
  SND_CHECK_EQ(t.NextGrid(100.0, 8), 480.0);
  SND_CHECK_EQ(t.NextGrid(3841.0, 16), 3840.0 + 240.0);
  SND_CHECK_EQ(t.BarsAfter(5000.0, 2), 3 * 3840.0);
  SND_CHECK_EQ(t.NextOfKind(SND_WHEN_NEXT_BAR, 0, 3840.0), 7680.0);  // strictly after
  const snd_musical_time m = t.Musical(2 * 3840.0 + 3 * 960.0 + 5.0);
  SND_CHECK_EQ(m.bar, 2);
  SND_CHECK_EQ(m.beat, 3);
  SND_CHECK_EQ(m.tick, 5);
  SND_CHECK_EQ(t.TickOfMusical(snd_musical_time{2, 3, 5}), 2 * 3840.0 + 3 * 960.0 + 5.0);
}

SND_TEST(MusicTimeline, TempoPointsAndSignatureChanges) {
  Timeline t;
  t.Reset(48000.0, 120.0, 4, 4);
  t.StartAt(0, 0.0);
  SND_REQUIRE(t.AddPoint(3840, 60.0, 3, 4));  // bar 1: 60 BPM, 3/4
  SND_CHECK_NEAR(ClockOf(t, 3840.0), 96000.0, 1e-6);
  SND_CHECK_NEAR(ClockOf(t, 3840.0 + 960.0), 96000.0 + 48000.0, 1e-6);
  SND_CHECK_NEAR(t.TickAtClock(96000.0 + 96000.0), 3840.0 + 1920.0, 1e-9);
  const snd_musical_time m = t.Musical(3840.0 + 2 * 960.0);
  SND_CHECK_EQ(m.bar, 1);
  SND_CHECK_EQ(m.beat, 2);
  SND_CHECK_EQ(t.NextBar(3841.0), 3840.0 + 2880.0);  // a 3/4 bar
  SND_CHECK_EQ(t.Musical(3840.0 + 2880.0).bar, 2);
  // Backward across the point (anchor after it).
  t.AnchorAt(200000);
  SND_CHECK_NEAR(ClockOf(t, 960.0), 24000.0, 1e-6);
  SND_CHECK_NEAR(t.TickAtClock(24000.0), 960.0, 1e-9);
  // Equal tempo / signature: nothing added; truncation keeps the first point.
  const std::uint32_t n = t.count;
  SND_CHECK(t.AddPoint(5000, 60.0, 3, 4));
  SND_CHECK_EQ(t.count, n);
  t.TruncateFrom(0);
  SND_CHECK_EQ(t.count, 1u);
}

SND_TEST(MusicTimeline, CompoundMeterBeatsAreTheBeatUnit) {
  Timeline t;
  t.Reset(48000.0, 120.0, 6, 8);  // 6/8: eighths of 480 ticks, bars of 2880
  t.StartAt(0, 0.0);
  SND_CHECK_EQ(t.NextBeat(1.0), 480.0);
  SND_CHECK_EQ(t.NextBar(1.0), 2880.0);
  SND_CHECK_EQ(t.Musical(2880.0 + 480.0 * 5).beat, 5);
}

SND_TEST(MusicTimeline, OverridePauseSeekDelay) {
  Timeline t;
  t.Reset(48000.0, 120.0, 4, 4);
  t.StartAt(0, 0.0);
  t.SetTempoAt(48000, 60.0);  // tick 1920, then half speed
  SND_CHECK_NEAR(ClockOf(t, 1920.0 + 960.0), 48000.0 + 48000.0, 1e-6);
  t.PauseAt(96000);  // tick 2880
  SND_CHECK(!t.Playing());
  double c = 0.0;
  SND_CHECK(!t.ClockAtTick(3000.0, &c));
  SND_CHECK_NEAR(t.TickAtClock(500000.0), 2880.0, 1e-9);
  t.ResumeAt(200000);
  SND_CHECK_NEAR(ClockOf(t, 2880.0 + 960.0), 200000.0 + 48000.0, 1e-6);
  t.Delay(1000);  // sound-priority stall
  SND_CHECK_NEAR(ClockOf(t, 2880.0 + 960.0), 201000.0 + 48000.0, 1e-6);
  t.SeekAt(300000, 7680.0);
  SND_CHECK_NEAR(t.TickAtClock(300000.0), 7680.0, 1e-9);
  t.StopAt(400000);
  SND_CHECK_NEAR(t.TickAtClock(400000.0), 0.0, 1e-9);
}

SND_TEST(MusicTimeline, AdvanceDropsPassedPoints) {
  Timeline t;
  t.Reset(48000.0, 120.0, 4, 4);
  t.StartAt(0, 0.0);
  t.AddPoint(3840, 90.0, 4, 4);
  t.AddPoint(7680, 60.0, 4, 4);
  t.AdvanceTo(100000);  // past the first change
  SND_CHECK_EQ(t.count, 2u);
  SND_CHECK_EQ(t.points[0].tick, 3840);
  SND_CHECK_NEAR(t.BpmAt(t.anchorTick), 90.0, 1e-9);
  SND_CHECK_NEAR(t.BarPosition(7680.0), 2.0, 1e-9);
}

SND_TEST(MusicSystemUnit, InitRejectsBadPoolsAndConvertsOnlyLiveTransports) {
  snd::music::MusicSystem m;
  snd::music::MusicLimits l{4, 16, 32, 64, 48000, 256};
  snd::music::MusicServices s{};
  const std::size_t bytes = m.PoolBytes(l);
  SND_CHECK(bytes > 0u);
  SND_CHECK(!m.Init(nullptr, bytes, l, s));
  static unsigned char small[64];
  SND_CHECK(!m.Init(small, sizeof small, l, s));
  unsigned char* pool = new unsigned char[bytes];
  SND_CHECK(m.Init(pool, bytes, l, s));
  snd::ClockSample c = 0;
  snd::MusicalTicks t = 0;
  SND_CHECK(!m.TicksToClock(0, 960, &c));
  SND_CHECK(!m.ClockToTicks(9, 0, &t));
  snd::notify::EventRecord rec[4];
  SND_CHECK_EQ(m.CollectNotifications(rec, 4), 0u);
  m.Shutdown();
  delete[] pool;
}
