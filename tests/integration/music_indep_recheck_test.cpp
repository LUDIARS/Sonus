// Independent re-check of the stage-4 music decisions D-68..D-82 (CONTRACTS.md,
// design 8). Properties are derived from the decisions; expected values are
// computed from them (120 BPM: 0.04 ticks per sample, quarter = 24000, bar = 96000).
#include <algorithm>
#include <map>
#include <set>

#include "music_indep_util.hpp"

using namespace mi;

namespace {

std::vector<std::uint8_t> PlainBank() {
  MusicProject p("recheck-plain");
  for (int i = 0; i < 4; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  return BuildBank(p);
}

struct Standalone {
  Rig r;
  snd_transport_handle t{};
  std::uint64_t t0 = 0;
  explicit Standalone(float bpm = 120.0f) {
    if (r.sys == nullptr || !r.Load(PlainBank())) return;
    snd_transport_desc d{bpm, 4, 4, 1, 0};
    t = r.sys->CreateTransport(&d);
    t0 = r.Clock();
    r.Step();
  }
  bool ok() const { return t.value != 0u; }
  void Play(const char* ev, const snd_when& w) { r.sys->Play(HashId(ev), snd_emitter_handle{0}, &w); }
  std::uint64_t First(const char* asset) const {
    const std::vector<StartLog> s = StartsOf(r, asset);
    return s.empty() ? UINT64_MAX : s[0].clock;
  }
};

snd_when With(snd_transport_handle t, std::uint32_t kind, std::uint32_t count, std::int32_t offset) {
  snd_when w = Kind(t, kind, count);
  w.offset_ticks = offset;
  return w;
}

// Segment T: bar 0 at 120 BPM 4/4, bar 1 at 60 BPM 3/4; the playlist plays it twice.
std::vector<std::uint8_t> TempoBank() {
  MusicProject p("recheck-tempo");
  p.Asset("t", Material(kBar + 144000, 0.3f));
  p.Segment("T", "t", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120}, {\"tick\": 3840, \"bpm\": 60, \"beatsPerBar\": 3}]");
  p.Playlist("PL", "{\"segment\": \"T\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  for (int i = 0; i < 2; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  return BuildBank(p);
}

// Explore (E, 2 bars, 0.2; bar 1 at 0.4 when `stepped`), Battle (Bt, 2 bars, 0.6), Boss (Bo, 1 bar, 0.4).
std::vector<std::uint8_t> StatesBank(const std::string& transitions, const std::string& segE = "", bool stepped = false) {
  MusicProject p("recheck-states");
  bt::PcmData e = Material(2 * kBar, 0.2f);
  if (stepped) {
    for (std::size_t i = kBar; i < 2 * kBar; ++i) e.samples[i] = 0.4f;
  }
  p.Asset("e", e);
  p.Asset("bt", Material(2 * kBar, 0.6f));
  p.Asset("bo", Material(kBar, 0.4f));
  p.Segment("E", "e", 2, segE);
  p.Segment("Bt", "bt", 2);
  p.Segment("Bo", "bo", 1);
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
  explicit Music(const std::vector<std::uint8_t>& bank) {
    if (r.sys == nullptr || bank.empty() || !r.Load(bank)) return;
    s0 = r.Clock();
    p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  }
  bool ok() const { return p.value != 0u; }
  snd::music::MusicPlaybackInfo Info() const {
    snd::music::MusicPlaybackInfo i;
    r.music.GetPlaybackInfo(SlotOf(p), &i);
    return i;
  }
};

const std::string kCut = "\"fadeOutBeats\": 0, \"fadeInBeats\": 0";

}  // namespace

// --- D-69 / D-70: lead time and ties ----------------------------------------------------

SND_TEST(MusicRecheck, D69_FiresInsideTheAcceptingBlock) {
  // The beat at t0 + 24000 lies inside the block the request is applied in (t0 + 23808).
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 23808);
  SND_REQUIRE_EQ(x.r.Clock(), x.t0 + 23808);
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BEAT));
  x.r.RunTo(x.t0 + kBar);
  SND_CHECK_EQ(x.First("h0"), x.t0 + 24000);
}

SND_TEST(MusicRecheck, D70_ARequestOnTheGridPointFiresThere) {
  // 112.5 BPM: beat = 25600 = 100 blocks, bar = 102400. Applied exactly on bar line 1.
  Standalone x(112.5f);
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 102400);
  SND_REQUIRE_EQ(x.r.Clock(), x.t0 + 102400);
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BAR));
  x.Play("H1", Kind(x.t, SND_WHEN_NEXT_BEAT));
  x.Play("H2", Kind(x.t, SND_WHEN_GRID, 16));
  x.Play("H3", Kind(x.t, SND_WHEN_BARS_AFTER, 1));  // "N bars after the current bar's start"
  x.r.RunTo(x.t0 + 3 * 102400);
  SND_CHECK_EQ(x.First("h0"), x.t0 + 102400);
  SND_CHECK_EQ(x.First("h1"), x.t0 + 102400);
  SND_CHECK_EQ(x.First("h2"), x.t0 + 102400);
  SND_CHECK_EQ(x.First("h3"), x.t0 + 2 * 102400);
}

// --- D-71: grid reservations kept as musical positions ------------------------------------

SND_TEST(MusicRecheck, D71_BarsAfterIntoALaterLoopWithOffset) {
  // Accepted in bar 1 (3/4 at 60) of the first pass, before the second pass is planned:
  // "2 bars after the current bar" = bar 3 = second pass bar 1 (bar 2 is 4/4 at 120).
  // bar 2 at s0 + 240000, bar 3 at s0 + 336000; offset -480 ticks at bar 2's 120 BPM = -12000.
  Music x(TempoBank());
  SND_REQUIRE(x.ok());
  const snd_transport_handle t = OwnedTransport(x.p);
  x.r.RunTo(x.s0 + 100000);
  const snd_when a = Kind(t, SND_WHEN_BARS_AFTER, 2);
  const snd_when b = With(t, SND_WHEN_BARS_AFTER, 2, -480);
  x.r.sys->Play(HashId("H0"), snd_emitter_handle{0}, &a);
  x.r.sys->Play(HashId("H1"), snd_emitter_handle{0}, &b);
  x.r.RunTo(x.s0 + 400000);
  const std::vector<StartLog> h0 = StartsOf(x.r, "h0"), h1 = StartsOf(x.r, "h1");
  SND_REQUIRE(!h0.empty() && !h1.empty());
  SND_CHECK_EQ(h0[0].clock, x.s0 + 336000);
  SND_CHECK_EQ(h1[0].clock, x.s0 + 324000);
}

SND_TEST(MusicRecheck, D71_D79_ReservationsAcrossStopAndRestart) {
  // H3 was made while playing (bar 2); the transport is stopped (rewound to bar 0) and
  // started again at c: bar 2 of the new time line. H0..H2 are made while Stopped and
  // resolve from bar 0 (D-79): next beat = c, 1 bar after = c + bar, next bar + 480 = c + 12000.
  Standalone x;
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.t0 + 1000);
  x.Play("H3", Kind(x.t, SND_WHEN_BARS_AFTER, 2));
  x.r.RunTo(x.t0 + 30000);
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_STOPPED, nullptr);
  x.r.Steps(2);
  x.Play("H0", Kind(x.t, SND_WHEN_NEXT_BEAT));
  x.Play("H1", Kind(x.t, SND_WHEN_BARS_AFTER, 1));
  x.Play("H2", With(x.t, SND_WHEN_NEXT_BAR, 0, 480));
  x.r.Steps(100);
  SND_CHECK_EQ(x.First("h0"), UINT64_MAX);
  SND_CHECK_EQ(x.First("h3"), UINT64_MAX);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTransportState(x.t, SND_TRANSPORT_PLAYING, nullptr);
  x.r.RunTo(c + 3 * kBar);
  SND_CHECK_EQ(x.First("h0"), c);
  SND_CHECK_EQ(x.First("h1"), c + kBar);
  SND_CHECK_EQ(x.First("h2"), c + 12000);
  SND_CHECK_EQ(x.First("h3"), c + 2 * kBar);
}

// --- D-72: SetTempo overrides the later segment tempo until the next SetTempo ---------------

SND_TEST(MusicRecheck, D72_SetTempoOverridesTheSegmentTempoKeepsItsSignature) {
  Music x(TempoBank());
  SND_REQUIRE(x.ok());
  const snd_transport_handle t = OwnedTransport(x.p);
  x.r.RunTo(x.s0 + 30000);
  const std::uint64_t c = x.r.Clock();
  x.r.sys->SetTempo(t, 90.0f, nullptr);  // 0.03 ticks per sample, quarter = 32000
  x.r.RunTo(x.s0 + 400000);
  x.r.sys->Update();
  const double tick = static_cast<double>(c - x.s0) * 0.04;
  const std::uint64_t bar1 = c + Round((3840.0 - tick) / 0.03);
  std::map<std::pair<int, int>, std::uint64_t> at;
  for (const Beat& b : BeatsOf(x.r, t)) {
    if (b.clock >= c) at[{b.bar, b.beat}] = b.clock;
  }
  // Bar 1 keeps the segment's 3/4 (three beats), at 90 BPM instead of the segment's 60.
  SND_CHECK_EQ(at[std::make_pair(1, 0)], bar1);
  SND_CHECK_EQ(at[std::make_pair(1, 1)], bar1 + 32000);
  SND_CHECK_EQ(at[std::make_pair(1, 2)], bar1 + 64000);
  SND_CHECK_EQ(at[std::make_pair(2, 0)], bar1 + 96000);
  SND_CHECK(at.find(std::make_pair(1, 3)) == at.end());
  // The second pass (its own 120 BPM point at bar 2) is overridden as well.
  SND_CHECK_EQ(at[std::make_pair(2, 1)], bar1 + 96000 + 32000);
}

// --- D-73: next-marker timing by id, 0 = any user marker ------------------------------------

SND_TEST(MusicRecheck, D73_NextMarkerTimingById) {
  const std::string segE = "\"markers\": [{\"name\": \"A\", \"frame\": 30000}, {\"name\": \"B\", \"frame\": 60000}]";
  for (int any = 0; any < 2; ++any) {
    const std::string count = any != 0 ? "0" : std::to_string(HashId("B"));
    Music x(StatesBank(Rule("*", "*", "\"timing\": \"nextMarker\", \"timingCount\": " + count + ", " + kCut), segE));
    SND_REQUIRE(x.ok());
    x.r.RunTo(x.s0 + 1000);
    x.r.sys->SetMusicState(x.p, HashId("Battle"), nullptr);
    x.r.RunTo(x.s0 + kBar);
    const std::vector<StartLog> bt = StartsOf(x.r, "bt");
    SND_REQUIRE(!bt.empty());
    SND_CHECK_EQ(bt[0].clock, x.s0 + (any != 0 ? 30000u : 60000u));
  }
}

// --- D-75: seek re-places the current segment, position modulo its length -------------------

SND_TEST(MusicRecheck, D75_SeekWrapsByTheSegmentLengthBothWays) {
  // E: 2 bars (7680 ticks). Seek to bar 5 beat 1 (tick 20160): 20160 mod 7680 = 4800
  // -> frame 120000; the playlist goes on 2880 ticks (72000 samples) later. Then seek back
  // to bar 0 beat 2 (tick 1920) from the next pass: frame 48000.
  Music x(StatesBank("", "", true));
  SND_REQUIRE(x.ok());
  const snd_transport_handle t = OwnedTransport(x.p);
  x.r.RunTo(x.s0 + 10000);
  const std::uint64_t c1 = x.r.Clock();
  const snd_musical_time far{5, 1, 0};
  x.r.sys->SeekTransport(t, &far, nullptr);
  x.r.RunTo(c1 + 100000);
  const std::uint64_t c2 = x.r.Clock();
  const snd_musical_time back{0, 2, 0};
  x.r.sys->SeekTransport(t, &back, nullptr);
  x.r.RunTo(c2 + 10000);
  const std::vector<StartLog> e = StartsOf(x.r, "e");
  bool a = false, b = false, n = false;
  for (const StartLog& s : e) {
    if (s.clock == c1) a = s.frame == 120000u;
    if (s.clock == c1 + 72000) n = s.frame == 0u;
    if (s.clock == c2) b = s.frame == 48000u;
  }
  SND_CHECK(a);
  SND_CHECK(n);
  SND_CHECK(b);
  SND_CHECK_NEAR(x.r.At(c1 + 2000), 0.4f, 1e-4f);  // bar 1 of E
  SND_CHECK_NEAR(x.r.At(c2 + 2000), 0.2f, 1e-4f);  // bar 0 of E
}

// --- D-77: interrupted fade shortened linearly from the current gain --------------------------

SND_TEST(MusicRecheck, D77_InterruptShortensTheFadeLinearly) {
  // Explore -> Battle at X (bar 1): E fades out over 4 beats (gain 1 - u / 96000), Battle at
  // full level. At Y = X + 1 beat, Boss interrupts (fade out 1 beat): E goes from 0.75 to 0
  // and Battle from 1 to 0, both linearly until Y + 24000; Boss (0.4) at full level from Y.
  const std::string rules = Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"fadeOutBeats\": 4, \"fadeInBeats\": 0") + ", " +
                            Rule("*", "Boss", "\"timing\": \"nextBeat\", \"fadeOutBeats\": 1, \"fadeInBeats\": 0");
  Music x(StatesBank(rules));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 1000);
  x.r.sys->SetMusicState(x.p, HashId("Battle"), nullptr);
  const std::uint64_t X = x.s0 + kBar;
  x.r.RunTo(X + 10000);
  x.r.sys->SetMusicState(x.p, HashId("Boss"), nullptr);
  const std::uint64_t Y = X + kBeat;
  x.r.RunTo(Y + 2 * kBeat);
  SND_CHECK_EQ(StartsOf(x.r, "bo").empty() ? 0u : StartsOf(x.r, "bo")[0].clock, Y);
  for (std::uint64_t u = 2000; u < 24000; u += 2000) {
    SND_CHECK_NEAR(x.r.At(X + u), 0.6f + 0.2f * (1.0f - static_cast<float>(u) / 96000.0f), 1e-3f);
  }
  float worst = 0.0f;
  for (std::uint64_t u = 64; u < 24000; u += 500) {
    const float want = 0.4f + (0.2f * 0.75f + 0.6f) * (1.0f - static_cast<float>(u) / 24000.0f);
    worst = std::max(worst, std::fabs(x.r.At(Y + u) - want));
  }
  SND_CHECK_MSG(worst < 2e-3f, "max deviation from the straight line %f", static_cast<double>(worst));
  SND_CHECK_NEAR(x.r.At(Y + 24000 + 500), 0.4f, 1e-3f);
}

// --- D-78: long sound-priority stall -------------------------------------------------------------

SND_TEST(MusicRecheck, D78_LongStallAnnouncesEveryBeatOnceInOrder) {
  // Four starved blocks [s0 + 92 * 256, s0 + 96 * 256) around beat 1 (s0 + 24000): every
  // beat is announced once, in order, and the beats after the stall are 1024 later.
  MusicProject p("recheck-stall");
  p.Asset("s", Material(kBar, 0.3f), "\"stallPolicy\": \"sound\"");
  p.Segment("S1", "s", 1, "\"markers\": [{\"name\": \"Mk\", \"frame\": 30000}]");
  p.Playlist("PL", "{\"segment\": \"S1\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Music x(BuildBank(p));
  SND_REQUIRE(x.ok());
  x.r.kind.stallAsset.store(HashId("s"));
  x.r.kind.stallFrom.store(x.s0 + 92 * 256);
  x.r.kind.stallTo.store(x.s0 + 96 * 256);
  x.r.RunTo(x.s0 + 2 * kBar + 4096);
  x.r.sys->Update();
  std::vector<Beat> b = BeatsOf(x.r, OwnedTransport(x.p));
  SND_REQUIRE(b.size() >= 8u);
  std::set<std::pair<int, int>> seen;
  bool once = true, ordered = true;
  for (std::size_t i = 0; i < b.size(); ++i) {
    once = once && seen.insert({b[i].bar, b[i].beat}).second;
    if (i != 0u) ordered = ordered && b[i].clock > b[i - 1].clock;
  }
  SND_CHECK(once);
  SND_CHECK(ordered);
  for (const Beat& e : b) {
    if (e.bar == 0 && e.beat >= 2) SND_CHECK_EQ(e.clock, x.s0 + e.beat * kBeat + 1024u);
    if (e.bar == 1 && e.beat == 0) SND_CHECK_EQ(e.clock, x.s0 + kBar + 1024u);
  }
  // The marker after the stall: once per pass, 1024 late in the first pass.
  std::vector<std::uint64_t> mk;
  for (const Marker& m : x.r.ev.markers) {
    if (m.playback == x.p.value && m.id == HashId("Mk")) mk.push_back(m.clock);
  }
  SND_REQUIRE_EQ(mk.size(), std::size_t{2});
  SND_CHECK_EQ(mk[0], x.s0 + 30000u + 1024u);
  SND_CHECK_EQ(mk[1], x.s0 + kBar + 30000u + 1024u);
}

// --- D-80: requests while paused --------------------------------------------------------------------

SND_TEST(MusicRecheck, D80_PausedRequestsLastWinsThenRuleTiming) {
  // Paused at cp (tick p), Battle then Boss requested, resumed at cr: Boss on the next bar
  // after p, counted from cr (pause and resume act at block starts: +-1024).
  Music x(StatesBank(Rule("*", "*", "\"timing\": \"nextBar\", " + kCut)));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 30000);
  const std::uint64_t cp = x.r.Clock();
  x.r.sys->Pause(x.p, 0.0f);
  x.r.RunTo(x.s0 + 50000);
  x.r.sys->SetMusicState(x.p, HashId("Battle"), nullptr);
  x.r.Step();
  x.r.sys->SetMusicState(x.p, HashId("Boss"), nullptr);
  x.r.RunTo(x.s0 + 100000);
  const std::uint64_t cr = x.r.Clock();
  x.r.sys->ResumePlayback(x.p, 0.0f);
  x.r.RunTo(cr + 2 * kBar);
  SND_CHECK(StartsOf(x.r, "bt").empty());
  const std::vector<StartLog> bo = StartsOf(x.r, "bo");
  SND_REQUIRE(!bo.empty());
  const std::uint64_t want = cr + (kBar - (cp - x.s0));
  SND_CHECK_MSG(Diff(bo[0].clock, want) <= 1024u, "Boss at +%llu, want about +%llu",
                static_cast<unsigned long long>(bo[0].clock - x.s0), static_cast<unsigned long long>(want - x.s0));
  SND_CHECK_EQ(x.Info().state, HashId("Boss"));
}

SND_TEST(MusicRecheck, D80_PausedRequestForTheCurrentStateCancels) {
  Music x(StatesBank(Rule("*", "*", "\"timing\": \"nextBar\", " + kCut)));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 30000);
  x.r.sys->Pause(x.p, 0.0f);
  x.r.RunTo(x.s0 + 50000);
  x.r.sys->SetMusicState(x.p, HashId("Battle"), nullptr);
  x.r.Step();
  x.r.sys->SetMusicState(x.p, HashId("Explore"), nullptr);
  x.r.RunTo(x.s0 + 100000);
  x.r.sys->ResumePlayback(x.p, 0.0f);
  x.r.RunTo(x.s0 + 100000 + 3 * kBar);
  SND_CHECK(StartsOf(x.r, "bt").empty());
  SND_CHECK_EQ(x.Info().transitions, 0u);
  SND_CHECK_EQ(x.Info().state, HashId("Explore"));
}

// --- D-81: shuffle of any length ---------------------------------------------------------------------

namespace {
std::vector<snd::Id> ShuffleRun(int n, int passes) {
  MusicProject p("recheck-shuffle");
  std::string entries;
  for (int i = 0; i < n; ++i) {
    const std::string a = "a" + std::to_string(i);
    p.Asset(a, Material(6000, 0.1f));
    p.Segment("S" + a, a, 1, "\"tempo\": [{\"tick\": 0, \"bpm\": 1920}]");
    entries += (i == 0 ? "" : ", ") + std::string("{\"segment\": \"S") + a + "\"}";
  }
  p.Playlist("PL", entries, "\"mode\": \"shuffle\", \"loopCount\": " + std::to_string(passes));
  p.MusicEvent("Music", kOneState);
  Music x(BuildBank(p));
  std::vector<snd::Id> order;
  if (!x.ok()) return order;
  x.r.RunTo(x.s0 + static_cast<std::uint64_t>(n * passes + 3) * 6000u);
  std::uint64_t k = 0;
  for (const StartLog& s : x.r.kind.Starts()) {
    if (s.clock != x.s0 + k * 6000u) return {};  // one segment per bar, back to back
    order.push_back(s.asset);
    ++k;
  }
  return order;
}
}  // namespace

SND_TEST(MusicRecheck, D81_ShuffleOfFortyEntriesPlaysEachOncePerPass) {
  const int n = 40, passes = 3;
  const std::vector<snd::Id> a = ShuffleRun(n, passes);
  SND_REQUIRE_EQ(a.size(), static_cast<std::size_t>(n * passes));
  for (int pass = 0; pass < passes; ++pass) {
    std::set<snd::Id> ids(a.begin() + pass * n, a.begin() + (pass + 1) * n);
    SND_CHECK_EQ(ids.size(), static_cast<std::size_t>(n));
    if (pass != 0) SND_CHECK(a[static_cast<std::size_t>(pass * n)] != a[static_cast<std::size_t>(pass * n - 1)]);
  }
  SND_CHECK(!std::equal(a.begin(), a.begin() + n, a.begin() + n));  // a new permutation per pass
  SND_CHECK(a == ShuffleRun(n, passes));                            // reproducible (2.5)
}
