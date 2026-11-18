// Independent tests of the music module (stage 4), written by a tester other
// than the implementer. The properties come from design 8 / 2.5 and the
// contract (tests/integration/music_indep_properties.md); only those the
// implementer's tests did not cover are here. Expected values are computed
// from the design, never read back from the implementation.
#include <algorithm>
#include <atomic>
#include <cstring>

#include "music_indep_util.hpp"

using namespace mi;

namespace {

// Events H0..H3 (asset h0..h3, DC 0.5, 2400 frames) and Long (DC 0.5, 4 bars).
std::vector<std::uint8_t> PlainBank() {
  MusicProject p("indep-plain");
  for (int i = 0; i < 4; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  p.Asset("long", Material(4 * kBar, 0.5f));
  p.Event("Long", "\"tracks\": [{\"clips\": [{\"asset\": \"long\"}]}]");
  p.Asset("m", Material(8 * kBar, 0.2f));
  p.Segment("M", "m", 8);
  p.Playlist("PL", "{\"segment\": \"M\"}");
  p.MusicEvent("Music", kOneState);
  return BuildBank(p);
}

// A standalone transport created at block start t0 (Playing unless `playing` = 0).
struct Standalone {
  Rig r;
  snd_transport_handle t{};
  std::uint64_t t0 = 0;
  explicit Standalone(float bpm = 120.0f, std::uint32_t playing = 1, const snd_config& cfg = Config()) : r(cfg) {
    if (r.sys == nullptr || !r.Load(PlainBank())) return;
    snd_transport_desc d{bpm, 4, 4, playing, 0};
    t = r.sys->CreateTransport(&d);
    t0 = r.Clock();
    r.Step();
  }
  bool ok() const { return t.value != 0u; }
  snd_playback_handle Play(const char* ev, const snd_when& w) { return r.sys->Play(HashId(ev), snd_emitter_handle{0}, &w); }
  std::uint64_t FirstStart(const char* asset) const {
    const std::vector<StartLog> s = StartsOf(r, asset);
    return s.empty() ? UINT64_MAX : s[0].clock;
  }
};

}  // namespace

// --- T: transport and beats (8.1) ------------------------------------------------------

SND_TEST(MusicIndep, BeatsStayExactAtAFractionalBeatLength) {
  // 127 BPM: a beat is 48000 * 60 / 127 = 22677.165... samples. Beat n must be at
  // round(t0 + n * that) for every n (no drift from per-block accumulation).
  Standalone x(127.0f);
  SND_REQUIRE(x.ok());
  const double beat = Quarter(127.0);
  const std::uint32_t n = 200;
  x.r.RunTo(x.t0 + Round(n * beat) + 1);
  x.r.sys->Update();
  const std::vector<Beat> b = BeatsOf(x.r, x.t);
  SND_REQUIRE(b.size() >= n);
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::uint64_t want = x.t0 + Round(k * beat);
    if (b[k].clock != want || b[k].bar != static_cast<std::int32_t>(k / 4) || b[k].beat != static_cast<std::int32_t>(k % 4)) {
      if (bad++ < 5) {
        std::fprintf(stderr, "beat %u: %d.%d at +%llu, want %u.%u at +%llu\n", k, b[k].bar, b[k].beat,
                     static_cast<unsigned long long>(b[k].clock - x.t0), k / 4, k % 4,
                     static_cast<unsigned long long>(want - x.t0));
      }
    }
  }
  SND_CHECK_EQ(bad, 0u);
  // A quantised start far from the origin lands on the same exact sample.
  const std::uint64_t c = x.r.Clock();
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.r.RunTo(c + Round(5 * beat));
  const double tickNow = static_cast<double>(c - x.t0) / beat * 960.0;
  const double barTick = std::ceil(tickNow / 3840.0) * 3840.0;
  SND_CHECK_EQ(x.FirstStart("h0"), x.t0 + Round(barTick / 960.0 * beat));
}

SND_TEST(MusicIndep, BeatsOnBlockEdgesAreSentOnce) {
  // 112.5 BPM: a beat is exactly 25600 samples = 100 blocks of 256, so every beat
  // falls on a block edge (regression: a beat on a block edge was sent twice).
  Standalone x(112.5f);
  SND_REQUIRE(x.ok());
  SND_REQUIRE_EQ(x.r.sys->BlockFrames(), 256u);
  x.r.RunTo(x.t0 + 20 * 25600 + 512);
  x.r.sys->Update();
  std::uint32_t k = 0;
  for (const Beat& b : BeatsOf(x.r, x.t)) {
    if (b.clock > x.t0 + 20 * 25600) break;
    SND_CHECK_EQ(b.clock, x.t0 + std::uint64_t{k} * 25600u);
    SND_CHECK_EQ(b.bar, static_cast<std::int32_t>(k / 4));
    SND_CHECK_EQ(b.beat, static_cast<std::int32_t>(k % 4));
    ++k;
  }
  SND_CHECK_EQ(k, 21u);
}

SND_TEST(MusicIndep, TwoTempoChangesInOneBlockLastWins) {
  // Two immediate SetTempo in one Update: applied in recording order, the last wins (2.5).
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 30000);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTempo(x.t, 60.0f, nullptr);
  x.r.sys->SetTempo(x.t, 240.0f, nullptr);
  x.r.RunTo(c + 100000);
  x.r.sys->Update();
  const double tick = static_cast<double>(c - x.t0) * 0.04;  // 120 BPM: 0.04 ticks per sample
  const std::uint64_t first = c + Round((std::ceil(tick / 960.0) * 960.0 - tick) / 0.08);
  std::vector<std::uint64_t> after;
  for (const Beat& b : BeatsOf(x.r, x.t)) {
    if (b.clock >= c) after.push_back(b.clock);
  }
  SND_REQUIRE(after.size() >= 4u);
  SND_CHECK_EQ(after[0], first);
  for (std::size_t i = 1; i < 4; ++i) SND_CHECK_EQ(after[i] - after[i - 1], 12000u);
}

SND_TEST(MusicIndep, TransportChangesInOneBlockApplyInClockOrder) {
  // Recorded: SetTempo(240) at clock c + 100, then Seek(bar 2) now. Applied in clock
  // order: the seek at c, the tempo at c + 100. Bar 2 beat 0 sounds at c; the next
  // beat after 100 samples at 120 BPM (4 ticks) and 956 ticks at 240 BPM (11950).
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 30000);
  const std::uint64_t c = x.r.Clock();
  snd_when at{};
  at.kind = SND_WHEN_CLOCK_SAMPLE;
  at.value = c + 100;
  x.r.sys->SetTempo(x.t, 240.0f, &at);
  const snd_musical_time to{2, 0, 0};
  x.r.sys->SeekTransport(x.t, &to, nullptr);
  x.r.RunTo(c + 60000);
  x.r.sys->Update();
  std::vector<Beat> after;
  for (const Beat& b : BeatsOf(x.r, x.t)) {
    if (b.clock >= c) after.push_back(b);
  }
  SND_REQUIRE(after.size() >= 3u);
  SND_CHECK_EQ(after[0].clock, c);
  SND_CHECK(after[0].bar == 2 && after[0].beat == 0);
  SND_CHECK_EQ(after[1].clock, c + 12050u);
  SND_CHECK(after[1].bar == 2 && after[1].beat == 1);
  SND_CHECK_EQ(after[2].clock, c + 12050u + 12000u);
}

SND_TEST(MusicIndep, QuantisedSeekRestartsAtTheBarLine) {
  // Seek to bar 0 on the next bar line: at t0 + 1 bar the position is bar 0 again. The
  // bar line is announced once, as bar 0 beat 0 (not also as bar 1).
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 30000);
  const snd_musical_time zero{0, 0, 0};
  const snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  x.r.sys->SeekTransport(x.t, &zero, &w);
  x.Play("H0", Kind(x.t, SND_WHEN_BARS_AFTER, 1));  // tick 3840 = the bar line after the seek
  x.r.RunTo(x.t0 + 3 * kBar);
  x.r.sys->Update();
  std::uint32_t atLine = 0;
  for (const Beat& b : BeatsOf(x.r, x.t)) {
    if (b.clock == x.t0 + kBar) {
      ++atLine;
      SND_CHECK(b.bar == 0 && b.beat == 0);
    }
    if (b.clock == x.t0 + kBar + kBeat) SND_CHECK(b.bar == 0 && b.beat == 1);
    if (b.clock == x.t0 + 2 * kBar) SND_CHECK(b.bar == 1 && b.beat == 0);
  }
  SND_CHECK_EQ(atLine, 1u);
  // The reservation held tick 3840 in music time: after the seek back it is one bar later.
  SND_CHECK_EQ(x.FirstStart("h0"), x.t0 + 2 * kBar);
}

SND_TEST(MusicIndep, StoppedTransportStartsAtBarZeroWhenPlayed) {
  Standalone x(120.0f, 0);
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 50000);
  x.r.sys->Update();
  SND_CHECK(BeatsOf(x.r, x.t).empty());
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_PLAYING, nullptr);
  x.r.Step();
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.r.RunTo(c + 2 * kBar);
  x.r.sys->Update();
  const std::vector<Beat> b = BeatsOf(x.r, x.t);
  SND_REQUIRE(b.size() >= 2u);
  SND_CHECK_EQ(b[0].clock, c);
  SND_CHECK(b[0].bar == 0 && b[0].beat == 0);
  SND_CHECK_EQ(b[1].clock, c + kBeat);
  SND_CHECK_EQ(x.FirstStart("h0"), c + kBar);
}

SND_TEST(MusicIndep, MusicalWhenOnADestroyedTransportIsImmediate) {
  // snd_when: a musical kind with a transport that is gone at apply time is IMMEDIATE
  // plus a warning with detail TRANSPORT_GONE.
  Standalone x;
  SND_REQUIRE(x.ok());
  SND_CHECK_EQ(x.r.sys->DestroyTransport(x.t), SND_OK);
  x.r.Steps(4);
  const std::uint64_t c = x.r.Clock();
  const snd_playback_handle p = x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));
  SND_REQUIRE(p.value != 0u);
  x.r.Steps(4);
  x.r.sys->Update();
  SND_CHECK_EQ(x.FirstStart("h0"), c);
  bool warned = false;
  for (const ErrorRec& e : x.r.ev.errors) warned = warned || (e.object == p.value && e.detail == SND_END_DETAIL_TRANSPORT_GONE);
  SND_CHECK(warned);
}

SND_TEST(MusicIndep, PlaybackOwnedTransportCannotBeDestroyed) {
  Standalone x;
  SND_REQUIRE(x.ok());
  const snd_playback_handle m = x.r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  const std::uint64_t s0 = x.r.Clock();
  x.r.Steps(10);
  SND_CHECK(x.r.sys->DestroyTransport(OwnedTransport(m)) != SND_OK);
  x.r.RunTo(s0 + 2 * kBar + 256);
  x.r.sys->Update();
  SND_CHECK_EQ(x.r.sys->GetPlaybackState(m), SND_PLAYBACK_STATE_PLAYING);
  bool bar2 = false;
  for (const Beat& b : BeatsOf(x.r, OwnedTransport(m))) bar2 = bar2 || (b.clock == s0 + 2 * kBar && b.bar == 2 && b.beat == 0);
  SND_CHECK(bar2);
}

SND_TEST(MusicIndep, SameClockReservationsFireInSequenceOrder) {
  // 2.5: entries due on the same sample fire in reservation (recording) order.
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  x.Play("H2", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.Play("H1", Kind(x.t, SND_WHEN_BARS_AFTER, 1));
  x.r.RunTo(x.t0 + kBar + 1024);
  const std::vector<StartLog> st = x.r.kind.Starts();
  std::vector<snd::Id> order;
  for (const StartLog& s : st) {
    if (s.clock == x.t0 + kBar) order.push_back(s.asset);
  }
  SND_REQUIRE_EQ(order.size(), std::size_t{3});
  SND_CHECK_EQ(order[0], HashId("h2"));
  SND_CHECK_EQ(order[1], HashId("h0"));
  SND_CHECK_EQ(order[2], HashId("h1"));
}

// --- R / music transport: signature and tempo changes (8.2, D-20) ------------------------

namespace {
// Segment T: bar 0 at 120 BPM 4/4 (96000 samples), bar 1 at 60 BPM 3/4 (3 x 48000).
std::vector<std::uint8_t> TempoBank() {
  MusicProject p("indep-tempo");
  p.Asset("t", Material(kBar + 144000, 0.3f));
  p.Segment("T", "t", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120}, {\"tick\": 3840, \"bpm\": 60, \"beatsPerBar\": 3}]");
  p.Playlist("PL", "{\"segment\": \"T\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  for (int i = 0; i < 4; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  return BuildBank(p);
}
}  // namespace

SND_TEST(MusicIndep, QuantisationAcrossSignatureAndTempoChange) {
  Rig r;
  SND_REQUIRE(r.Load(TempoBank()));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  const snd_transport_handle t = OwnedTransport(m);
  r.RunTo(s0 + 1000);
  // From bar 0: two bars later = 1 bar of 4/4 at 120 (96000) + 1 bar of 3/4 at 60 (144000).
  const snd_when w0 = Kind(t, SND_WHEN_BARS_AFTER, 2);
  r.sys->Play(HashId("H0"), snd_emitter_handle{0}, &w0);
  r.RunTo(s0 + 100000);  // inside bar 1 (3/4, quarter = 48000)
  const snd_when w1 = Kind(t, SND_WHEN_NEXT_BEAT);
  const snd_when w2 = Kind(t, SND_WHEN_GRID, 8);
  const snd_when w3 = Kind(t, SND_WHEN_NEXT_BAR);
  r.sys->Play(HashId("H1"), snd_emitter_handle{0}, &w1);
  r.sys->Play(HashId("H2"), snd_emitter_handle{0}, &w2);
  r.sys->Play(HashId("H3"), snd_emitter_handle{0}, &w3);
  r.RunTo(s0 + 300000);
  auto first = [&](const char* a) {
    const std::vector<StartLog> s = StartsOf(r, a);
    return s.empty() ? UINT64_MAX : s[0].clock;
  };
  SND_CHECK_EQ(first("h0"), s0 + 240000);
  SND_CHECK_EQ(first("h1"), s0 + 144000);
  SND_CHECK_EQ(first("h2"), s0 + 120000);  // an eighth at 60 BPM = 24000 after the bar line
  SND_CHECK_EQ(first("h3"), s0 + 240000);
}

// --- X: transitions (8.5, 8.6) -----------------------------------------------------------

namespace {
std::vector<std::uint8_t> StatesBank(const std::string& transitions) {
  MusicProject p("indep-states");
  p.Asset("e", Material(2 * kBar, 0.2f));
  p.Asset("bt", Material(2 * kBar, 0.6f));
  p.Asset("bo", Material(kBar, 0.4f));
  p.Asset("br", Material(kBar, 0.9f));
  p.Segment("E", "e", 2);
  p.Segment("Bt", "bt", 2);
  p.Segment("Bo", "bo", 1);
  p.Segment("Br", "br", 1);
  p.Playlist("PE", "{\"segment\": \"E\"}");
  p.Playlist("PBt", "{\"segment\": \"Bt\"}");
  p.Playlist("PBo", "{\"segment\": \"Bo\"}");
  std::string music =
      "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PE\"},"
      " {\"name\": \"Battle\", \"playlist\": \"PBt\"}, {\"name\": \"Boss\", \"playlist\": \"PBo\"}]";
  if (!transitions.empty()) music += ", \"transitions\": [" + transitions + "]";
  p.MusicEvent("Music", music);
  return BuildBank(p);
}

struct Music {
  Rig r;
  snd_playback_handle p{};
  std::uint64_t s0 = 0;
  explicit Music(const std::string& transitions) {
    if (r.sys == nullptr || !r.Load(StatesBank(transitions))) return;
    s0 = r.Clock();
    p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  }
  bool ok() const { return p.value != 0u; }
  void SetAt(std::uint64_t at, const char* state) {
    r.RunTo(at);
    r.sys->SetMusicState(p, HashId(state), nullptr);
  }
  snd::music::MusicPlaybackInfo Info() const {
    snd::music::MusicPlaybackInfo i;
    r.music.GetPlaybackInfo(SlotOf(p), &i);
    return i;
  }
};
}  // namespace

SND_TEST(MusicIndep, PendingTransitionFollowsATempoChange) {
  // 8.2: the pending transition is kept in music time (bar 1 = tick 3840); halving the
  // tempo before it moves the transition to the new bar line.
  Music x(Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0"));
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  x.r.RunTo(x.s0 + 30000);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTempo(OwnedTransport(x.p), 60.0f, nullptr);
  x.r.RunTo(x.s0 + 2 * kBar + 96000);
  const double tick = static_cast<double>(c - x.s0) * 0.04;
  const std::uint64_t X = c + Round((3840.0 - tick) / 0.02);
  const std::vector<StartLog> bt = StartsOf(x.r, "bt");
  SND_REQUIRE(!bt.empty());
  SND_CHECK_EQ(bt[0].clock, X);
  SND_CHECK_EQ(bt[0].frame, 0u);
}

SND_TEST(MusicIndep, RequestForTheCurrentStateIsIgnoredWhenIdle) {
  Music x(Rule("*", "*", "\"timing\": \"nextBeat\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0"));
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Explore");
  x.r.Steps(2);
  SND_CHECK_EQ(x.Info().phase, 0u);
  x.r.RunTo(x.s0 + 3 * kBar);
  SND_CHECK_EQ(x.Info().transitions, 0u);
  const std::vector<StartLog> e = StartsOf(x.r, "e");
  SND_REQUIRE_EQ(e.size(), std::size_t{2});
  SND_CHECK_EQ(e[1].clock, x.s0 + 2 * kBar);  // the playlist simply goes on
}

SND_TEST(MusicIndep, AfterBridgeLastQueuedRequestForTheNewStateDoesNothing) {
  // 8.6: during an "after bridge" transition one request is kept, the last one wins. The
  // last one names the state being entered: after the bridge nothing more happens.
  const std::string rules =
      Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"bridge\": \"Br\", \"interrupt\": \"afterBridge\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0") +
      ", " + Rule("*", "*", "\"timing\": \"nextBeat\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0");
  Music x(rules);
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.SetAt(X + 10000, "Boss");
  x.SetAt(X + 20000, "Explore");
  x.SetAt(X + 30000, "Battle");
  x.r.RunTo(X + 4 * kBar);
  SND_CHECK_EQ(x.Info().state, HashId("Battle"));
  SND_CHECK_EQ(x.Info().transitions, 1u);
  SND_CHECK(StartsOf(x.r, "bo").empty());
  SND_CHECK_EQ(StartsOf(x.r, "e").size(), std::size_t{1});
  const std::vector<StartLog> bt = StartsOf(x.r, "bt");
  SND_REQUIRE(!bt.empty());
  SND_CHECK_EQ(bt[0].clock, X + kBar);  // after the 1-bar bridge
}

SND_TEST(MusicIndep, WrongEventKindThroughPrepareAndAReservation) {
  // SND_END_DETAIL_WRONG_EVENT_KIND: music API on a normal playback, also when the
  // SetMusicState arrives through a reservation (fired on the next bar).
  Standalone x;
  SND_REQUIRE(x.ok());
  const snd_playback_handle p = x.r.sys->Play(HashId("Long"), snd_emitter_handle{0}, nullptr);
  x.r.Step();
  auto count = [&]() {
    x.r.sys->Update();
    std::uint32_t n = 0;
    for (const ErrorRec& e : x.r.ev.errors) n += (e.object == p.value && e.detail == SND_END_DETAIL_WRONG_EVENT_KIND) ? 1u : 0u;
    return n;
  };
  x.r.sys->PrepareMusicState(p, HashId("S"));
  x.r.Steps(2);
  SND_CHECK_EQ(count(), 1u);
  const snd_when w = Kind(x.t, SND_WHEN_NEXT_BAR);
  x.r.sys->SetMusicState(p, HashId("S"), &w);
  x.r.RunTo(x.t0 + kBar - 512);
  SND_CHECK_EQ(count(), 1u);
  x.r.RunTo(x.t0 + kBar + 1024);
  SND_CHECK_EQ(count(), 2u);
  SND_CHECK_EQ(x.r.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);  // the playback is not harmed
}

// --- S: stems (regression of the core SetGain ramp) ---------------------------------------

SND_TEST(MusicIndep, StemGainHoldsItsTargetLongAfterTheRamp) {
  MusicProject p("indep-stem");
  p.Parameter("Intensity", "\"min\": 0, \"max\": 1, \"default\": 0, \"scope\": [\"playback\"]");
  p.Asset("pad", Material(8 * kBar, 0.2f));
  p.Segment("S", "pad", 8,
            "\"stems\": [{\"channelFirst\": 0, \"channelCount\": 1, \"volumeCurve\": {\"parameter\": \"Intensity\", "
            "\"target\": \"volume\", \"points\": [[0, -6.0206], [1, 0]]}}]");
  p.Playlist("PL", "{\"segment\": \"S\"}");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildBank(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle h = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 20000);
  SND_CHECK_NEAR(r.At(s0 + 10000), 0.1f, 2e-4f);
  r.sys->SetParamPlayback(h, HashId("Intensity"), 1.0f);
  const std::uint64_t t1 = r.Clock();
  r.RunTo(t1 + 300000);
  for (std::uint64_t c = t1 + 2048; c < t1 + 300000; c += 9973) SND_CHECK_NEAR(r.At(c), 0.2f, 2e-4f);
  r.sys->SetParamPlayback(h, HashId("Intensity"), 0.0f);
  const std::uint64_t t2 = r.Clock();
  r.RunTo(t2 + 200000);
  for (std::uint64_t c = t2 + 2048; c < t2 + 200000; c += 9973) SND_CHECK_NEAR(r.At(c), 0.1f, 2e-4f);
}

// --- D: determinism, parallel rendering, RT safety (2.5, 3.3) --------------------------------

namespace {
std::vector<std::uint8_t> ScriptBank() {
  MusicProject p("indep-script");
  const char* names[] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    bt::PcmData m = Material(kBar + 6000, 0.1f + 0.05f * i);
    for (std::size_t k = 0; k < m.samples.size(); ++k) m.samples[k] *= static_cast<float>(1.0 + 0.4 * std::sin(0.0013 * k * (i + 1)));
    p.Asset(names[i], m);
    p.Segment(std::string("S") + names[i], names[i], 1,
              "\"entryFrame\": 3000, \"markers\": [{\"name\": \"Mk\", \"frame\": 27000}], \"tempo\": [{\"tick\": 0, \"bpm\": 120},"
              " {\"tick\": 1920, \"bpm\": 90}]");
  }
  p.Asset("st", Material(4800, 0.05f));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Playlist("P1", "{\"segment\": \"Sa\"}, {\"segment\": \"Sb\", \"weight\": 3}", "\"mode\": \"random\"");
  p.Playlist("P2", "{\"segment\": \"Sc\"}, {\"segment\": \"Sa\"}", "\"mode\": \"shuffle\"");
  p.MusicEvent("Music",
               "\"initialState\": \"One\", \"states\": [{\"name\": \"One\", \"playlist\": \"P1\"}, {\"name\": \"Two\", \"playlist\": \"P2\"}],"
               " \"transitions\": [{\"from\": \"*\", \"to\": \"*\", \"timing\": \"nextBeat\", \"start\": \"sameBar\", "
               "\"fadeOutBeats\": 1.5, \"fadeInBeats\": 0.5}],"
               " \"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBeat\", \"overlap\": \"stack\"}]");
  return BuildBank(p);
}

struct Trace {
  std::vector<float> audio;
  std::vector<StartLog> starts;
  std::vector<Beat> beats;
  std::vector<Marker> markers;
  std::vector<snd::music::MusicTransitionRecord> log;
  std::uint32_t lateChunks = 0;
};

void RunTrace(Rig& r, Trace* out, std::uint32_t blocks = 1600) {
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  const snd_transport_handle t = OwnedTransport(p);
  for (std::uint32_t i = 0; i < blocks; ++i) {
    if (i == 200) r.sys->SetMusicState(p, HashId("Two"), nullptr);
    if (i == 420) {
      const snd_musical_time to{1, 1, 120};
      r.sys->SeekTransport(t, &to, nullptr);
    }
    if (i == 600) {
      const snd_when w = Kind(t, SND_WHEN_NEXT_BAR);
      r.sys->SetTempo(t, 150.0f, &w);
      r.sys->Play(HashId("St"), snd_emitter_handle{0}, &w);
    }
    if (i == 800) {
      const snd_when w = Kind(t, SND_WHEN_GRID, 16);
      r.sys->SetMusicState(p, HashId("One"), &w);
    }
    if (i == 805) r.sys->SetMusicState(p, HashId("Two"), nullptr);
    if (i == 1100) r.sys->Pause(p, 0.0f);
    if (i == 1150) r.sys->ResumePlayback(p, 0.0f);
    if (i == 1300) r.sys->SetMusicState(p, HashId("One"), nullptr);
    r.Step();
  }
  r.sys->Update();
  out->audio = r.all;
  out->starts = r.kind.Starts();
  out->beats = r.ev.beats;
  out->markers = r.ev.markers;
  snd::music::MusicTransitionRecord log[32];
  const std::uint32_t n = r.music.TransitionLog(log, 32);
  out->log.assign(log, log + n);
  snd_stats st{};
  r.sys->GetStats(&st);
  out->lateChunks = st.late_chunks;
}

bool SameTrace(const Trace& a, const Trace& b, bool audio) {
  bool same = a.starts.size() == b.starts.size() && a.beats.size() == b.beats.size() &&
              a.markers.size() == b.markers.size() && a.log.size() == b.log.size();
  for (std::size_t i = 0; same && i < a.starts.size(); ++i) {
    same = a.starts[i].clock == b.starts[i].clock && a.starts[i].frame == b.starts[i].frame && a.starts[i].asset == b.starts[i].asset;
  }
  for (std::size_t i = 0; same && i < a.beats.size(); ++i) {
    same = a.beats[i].clock == b.beats[i].clock && a.beats[i].bar == b.beats[i].bar && a.beats[i].beat == b.beats[i].beat;
  }
  for (std::size_t i = 0; same && i < a.markers.size(); ++i) same = a.markers[i].clock == b.markers[i].clock;
  for (std::size_t i = 0; same && i < a.log.size(); ++i) same = std::memcmp(&a.log[i], &b.log[i], sizeof a.log[i]) == 0;
  if (same && audio) {
    same = a.audio.size() == b.audio.size() &&
           std::memcmp(a.audio.data(), b.audio.data(), a.audio.size() * sizeof(float)) == 0;
  }
  return same;
}

snd_config NoLod(std::uint32_t workers) {
  snd_config c = Config(workers);
  c.tuning.lod_down_blocks = 0;
  return c;
}
}  // namespace

SND_TEST(MusicIndep, ParallelRenderingGivesTheSameDecisionsAndBits) {
  // 2.5 / 3.4: the chunk sums are added in chunk order, so worker count does not change
  // the decisions, nor (load LOD off, no stalls, no late chunks) the bits.
  const std::vector<std::uint8_t> bank = ScriptBank();
  SND_REQUIRE(!bank.empty());
  Trace serial, parallel;
  {
    Rig r(NoLod(0));
    SND_REQUIRE(r.Load(bank));
    RunTrace(r, &serial);
  }
  {
    Rig r(NoLod(2));
    SND_REQUIRE(r.Load(bank));
    RunTrace(r, &parallel);
  }
  SND_CHECK_EQ(serial.log.size(), std::size_t{2});  // -> Two (block 200), One-then-Two cancels, -> One (1300)
  SND_CHECK(serial.starts.size() > 6u);
  SND_CHECK(!serial.markers.empty());
  SND_CHECK(SameTrace(serial, parallel, false));
  if (serial.lateChunks == 0u && parallel.lateChunks == 0u) {
    SND_CHECK(SameTrace(serial, parallel, true));
  } else {
    std::fprintf(stderr, "late chunks (%u / %u): bit identity not required (2.5)\n", serial.lateChunks, parallel.lateChunks);
  }
}

namespace {
class CountingAllocator final : public snd::IAllocator {
 public:
  void* Allocate(std::size_t bytes, std::size_t align, snd::MemTag tag) override {
    allocations.fetch_add(1, std::memory_order_relaxed);
    return snd::base::DefaultAllocator().Allocate(bytes, align, tag);
  }
  void Free(void* p, snd::MemTag tag) override { snd::base::DefaultAllocator().Free(p, tag); }
  std::atomic<std::uint64_t> allocations{0};
};
}  // namespace

SND_TEST(MusicIndep, NoAllocationWithSeeksTempoPausesAndTransitions) {
  const std::vector<std::uint8_t> bank = ScriptBank();
  SND_REQUIRE(!bank.empty());
  CountingAllocator alloc;
  Rig r(NoLod(0), &alloc);
  SND_REQUIRE(r.Load(bank));
  r.Steps(4);
  r.all.reserve(r.all.size() + 2 * 256 * 1700);
  r.ev.beats.reserve(4096);
  r.ev.markers.reserve(1024);
  r.ev.errors.reserve(1024);
  r.ev.ends.reserve(1024);
  const std::uint64_t before = alloc.allocations.load();
  Trace t;
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  const snd_transport_handle tr = OwnedTransport(p);
  for (std::uint32_t i = 0; i < 1600; ++i) {
    if (i == 200) r.sys->SetMusicState(p, HashId("Two"), nullptr);
    if (i == 420) {
      const snd_musical_time to{1, 1, 120};
      r.sys->SeekTransport(tr, &to, nullptr);
    }
    if (i == 600) {
      const snd_when w = Kind(tr, SND_WHEN_NEXT_BAR);
      r.sys->SetTempo(tr, 150.0f, &w);
      r.sys->Play(HashId("St"), snd_emitter_handle{0}, &w);
    }
    if (i == 800) r.sys->SetMusicState(p, HashId("One"), nullptr);
    if (i == 1100) r.sys->Pause(p, 0.0f);
    if (i == 1150) r.sys->ResumePlayback(p, 0.0f);
    r.Step();
  }
  SND_CHECK_EQ(alloc.allocations.load(), before);
  SND_CHECK_EQ(r.music.DroppedNotifications(), 0u);
  (void)t;
}
