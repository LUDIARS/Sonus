// Independent re-check (2nd) of the music decisions after 4c70a64: D-78 (held beats
// and markers during long sound-priority stalls), D-71 (segment tempo points topped up
// 4 s ahead, any number per segment; seeks rebuild the segment's map from its data),
// D-75 (seek modulo the segment length). Expected values come from the decisions.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "music_indep_util.hpp"

using namespace mi;

namespace {

struct Music {
  Rig r;
  snd_playback_handle p{};
  std::uint64_t s0 = 0;
  explicit Music(const std::vector<std::uint8_t>& bank, const char* event = "Music") {
    if (r.sys == nullptr || bank.empty() || !r.Load(bank)) return;
    s0 = r.Clock();
    p = r.sys->Play(HashId(event), snd_emitter_handle{0}, nullptr);
  }
  bool ok() const { return p.value != 0u; }
  snd_transport_handle T() const { return OwnedTransport(p); }
};

std::uint64_t FirstStartAtOrAfter(const Rig& r, const char* asset, std::uint64_t from, std::uint64_t* frame = nullptr) {
  for (const StartLog& s : r.kind.Starts()) {
    if (s.asset == HashId(asset) && s.clock >= from) {
      if (frame != nullptr) *frame = s.frame;
      return s.clock;
    }
  }
  return UINT64_MAX;
}

bool OnceAndOrdered(const std::vector<Beat>& b) {
  std::set<std::pair<int, int>> seen;
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (!seen.insert({b[i].bar, b[i].beat}).second) return false;
    if (i != 0u && b[i].clock <= b[i - 1].clock) return false;
  }
  return true;
}

std::map<std::pair<int, int>, std::uint64_t> BeatMap(const std::vector<Beat>& b) {
  std::map<std::pair<int, int>, std::uint64_t> m;
  for (const Beat& e : b) m[{e.bar, e.beat}] = e.clock;
  return m;
}

}  // namespace

// --- D-78: around a long stall ------------------------------------------------------------------

namespace {
// Music: Explore (E, sound priority, 2 bars, marker Mk at 30000) -> Battle (Bt) on the next bar.
// Other: an unrelated music event (time priority, 1 bar) playing at the same time.
std::vector<std::uint8_t> StallStatesBank() {
  MusicProject p("recheck2-stall");
  p.Asset("e", Material(2 * kBar, 0.2f), "\"stallPolicy\": \"sound\"");
  p.Asset("bt", Material(2 * kBar, 0.6f), "\"stallPolicy\": \"sound\"");
  p.Asset("o", Material(kBar, 0.05f));
  p.Asset("st", Material(2400, 0.05f));
  p.Event("St", "\"stinger\": true, \"tracks\": [{\"clips\": [{\"asset\": \"st\"}]}]");
  p.Segment("E", "e", 2, "\"markers\": [{\"name\": \"Mk\", \"frame\": 30000}]");
  p.Segment("Bt", "bt", 2);
  p.Segment("O", "o", 1);
  p.Playlist("PE", "{\"segment\": \"E\"}");
  p.Playlist("PBt", "{\"segment\": \"Bt\"}");
  p.Playlist("PO", "{\"segment\": \"O\"}");
  p.MusicEvent("Music",
               "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PE\"},"
               " {\"name\": \"Battle\", \"playlist\": \"PBt\"}], \"transitions\": [" +
                   Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0") +
                   "], \"stingers\": [{\"name\": \"Hit\", \"event\": \"St\", \"timing\": \"nextBeat\", \"overlap\": \"stack\"}]");
  p.MusicEvent("Other", "\"initialState\": \"O\", \"states\": [{\"name\": \"O\", \"playlist\": \"PO\"}]");
  return BuildBank(p);
}
}  // namespace

SND_TEST(MusicRecheck2, LongStallDelaysTransitionStingerMarkerAndBeatsOfThatPlaybackOnly) {
  // Four starved blocks [s0 + 92 * 256, s0 + 96 * 256): the transport of Music is 1024 late
  // afterwards. Battle (next bar), the stinger (next beat = beat 2), the marker Mk and every
  // later beat follow the delayed time line; each beat and the marker are announced once;
  // beat 1 at most one block early (D-78). Other is not stalled: its beats stay exact.
  Music x(StallStatesBank());
  SND_REQUIRE(x.ok());
  const snd_playback_handle other = x.r.sys->Play(HashId("Other"), snd_emitter_handle{0}, nullptr);
  x.r.kind.stallAsset.store(HashId("e"));
  x.r.kind.stallFrom.store(x.s0 + 92 * 256);
  x.r.kind.stallTo.store(x.s0 + 96 * 256);
  x.r.RunTo(x.s0 + 1000);
  x.r.sys->SetMusicState(x.p, HashId("Battle"), nullptr);
  x.r.RunTo(x.s0 + 30000);  // after the stall: the next beat is beat 2
  const snd_when w = Kind(x.T(), SND_WHEN_NEXT_BEAT);
  x.r.sys->Play(HashId("St"), snd_emitter_handle{0}, &w);
  x.r.RunTo(x.s0 + 3 * kBar);
  x.r.sys->Update();
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "bt", x.s0), x.s0 + kBar + 1024);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "st", x.s0), x.s0 + 48000 + 1024);
  std::vector<std::uint64_t> mk;
  for (const Marker& m : x.r.ev.markers) {
    if (m.playback == x.p.value && m.id == HashId("Mk")) mk.push_back(m.clock);
  }
  SND_REQUIRE_EQ(mk.size(), std::size_t{1});
  SND_CHECK_EQ(mk[0], x.s0 + 30000 + 1024);
  const std::vector<Beat> b = BeatsOf(x.r, x.T());
  SND_CHECK(OnceAndOrdered(b));
  auto m = BeatMap(b);
  const auto b01 = std::make_pair(0, 1);
  SND_CHECK_EQ(m.count(b01), std::size_t{1});
  SND_CHECK((m[b01] + 256u >= x.s0 + 24000 + 1024) && (m[b01] <= x.s0 + 24000 + 1024));
  SND_CHECK_EQ(m[std::make_pair(0, 2)], x.s0 + 48000 + 1024);
  SND_CHECK_EQ(m[std::make_pair(1, 0)], x.s0 + kBar + 1024);
  SND_CHECK_EQ(m[std::make_pair(2, 0)], x.s0 + 2 * kBar + 1024);
  std::uint32_t k = 0;
  for (const Beat& e : BeatsOf(x.r, OwnedTransport(other))) {
    SND_CHECK_EQ(e.clock, x.s0 + std::uint64_t{k} * kBeat);
    ++k;
  }
  SND_CHECK(k >= 12u);
  SND_CHECK_EQ(x.r.music.DroppedNotifications(), 0u);
}

SND_TEST(MusicRecheck2, PlaybackEndingDuringAStallDeliversItsHeldNoticesOnce) {
  // Starved from block 92 for good; the music is stopped in the middle of the stall. The held
  // beats / markers go out once (D-78 "Playback が終わったら保留分を配送"), nothing comes twice,
  // nothing comes after the end, and a new music playback afterwards is not affected.
  Music x(StallStatesBank());
  SND_REQUIRE(x.ok());
  x.r.kind.stallAsset.store(HashId("e"));
  x.r.kind.stallFrom.store(x.s0 + 92 * 256);
  x.r.kind.stallTo.store(x.s0 + 400 * 256);
  x.r.RunTo(x.s0 + 120 * 256);
  x.r.sys->Stop(x.p, 0.0f, nullptr);
  x.r.RunTo(x.s0 + 200 * 256);
  x.r.sys->Update();
  bool ended = false;
  for (const EndRec& e : x.r.ev.ends) ended = ended || e.handle == x.p.value;
  SND_CHECK(ended);
  const std::vector<Beat> b = BeatsOf(x.r, x.T());
  SND_CHECK(OnceAndOrdered(b));
  auto m = BeatMap(b);
  SND_CHECK_EQ(m.count(std::make_pair(0, 0)), std::size_t{1});
  SND_CHECK_EQ(m.count(std::make_pair(0, 1)), std::size_t{1});  // queued before the stop: delivered, not lost
  const std::size_t before = x.r.ev.beats.size() + x.r.ev.markers.size();
  x.r.Steps(50);
  x.r.sys->Update();
  std::size_t late = 0;
  for (std::size_t i = 0; i < x.r.ev.beats.size(); ++i) late += x.r.ev.beats[i].transport == x.T().value ? 1u : 0u;
  SND_CHECK_EQ(late, b.size());  // nothing more for the ended playback
  (void)before;
  // A new music playback (possibly in the same slot) starts clean: exact beats, no stale ones.
  x.r.kind.stallTo.store(0);
  const std::uint64_t c = x.r.Clock();
  const snd_playback_handle q = x.r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  x.r.RunTo(c + kBar + 256);
  x.r.sys->Update();
  std::uint32_t k = 0;
  for (const Beat& e : BeatsOf(x.r, OwnedTransport(q))) {
    SND_CHECK_EQ(e.clock, c + std::uint64_t{k} * kBeat);
    ++k;
  }
  SND_CHECK_EQ(k, 5u);
  SND_CHECK_EQ(x.r.music.DroppedNotifications(), 0u);
}

// --- D-71: many tempo points --------------------------------------------------------------------------

namespace {
// Segment F: 4 bars, a tempo point every 60 ticks alternating 120 / 180 BPM (256 points; about
// 150 in any 4 s). A 60-tick step lasts 1500 samples at 120 and 1000 at 180, so each beat (16
// steps) is exactly 20000 samples and each bar 80000.
std::vector<std::uint8_t> ManyPointsBank(std::uint32_t loops) {
  MusicProject p("recheck2-points");
  std::string tempo;
  for (int k = 0; k < 256; ++k) {
    tempo += (k == 0 ? "" : ", ") + std::string("{\"tick\": ") + std::to_string(k * 60) +
             ", \"bpm\": " + ((k % 2) == 0 ? "120" : "180") + "}";
  }
  p.Asset("f", Material(4 * 80000, 0.3f));
  p.Segment("F", "f", 4, "\"tempo\": [" + tempo + "]");
  p.Playlist("PL", "{\"segment\": \"F\"}", "\"loopCount\": " + std::to_string(loops));
  p.MusicEvent("Music", kOneState);
  for (int i = 0; i < 3; ++i) {
    const std::string n = std::to_string(i);
    p.Asset("h" + n, Material(2400, 0.5f));
    p.Event("H" + n, "\"tracks\": [{\"clips\": [{\"asset\": \"h" + n + "\"}]}]");
  }
  return BuildBank(p);
}
}  // namespace

SND_TEST(MusicRecheck2, MoreThan64TempoChangesWithinFourSeconds) {
  Music x(ManyPointsBank(2));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 1000);
  const snd_when w = Kind(x.T(), SND_WHEN_BARS_AFTER, 2);
  x.r.sys->Play(HashId("H0"), snd_emitter_handle{0}, &w);
  x.r.RunTo(x.s0 + 8 * 80000 + 1024);
  x.r.sys->Update();
  const std::vector<Beat> b = BeatsOf(x.r, x.T());
  SND_REQUIRE(b.size() >= 32u);
  std::uint32_t bad = 0;
  for (std::uint32_t k = 0; k < 32; ++k) {
    if (b[k].clock != x.s0 + std::uint64_t{k} * 20000u || b[k].bar != static_cast<int>(k / 4) ||
        b[k].beat != static_cast<int>(k % 4)) {
      if (bad++ < 4) std::fprintf(stderr, "beat %u: %d.%d at +%llu\n", k, b[k].bar, b[k].beat,
                                  static_cast<unsigned long long>(b[k].clock - x.s0));
    }
  }
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "h0", x.s0), x.s0 + 160000);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "f", x.s0 + 1), x.s0 + 320000);  // second pass
}

SND_TEST(MusicRecheck2, SeeksAfterThePassedPointsWereDroppedAndReservationsAfterThem) {
  // After 3 bars (about 190 points passed, at most 64 kept) seek back to bar 0 beat 1 and
  // later forward to bar 6 beat 2 (6 mod 4 = bar 2). Material frame, announced beat and a
  // reservation made right after each seek follow the segment's own map.
  Music x(ManyPointsBank(0));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 250000);
  const std::uint64_t c1 = x.r.Clock();
  const snd_musical_time back{0, 1, 0};
  x.r.sys->SeekTransport(x.T(), &back, nullptr);
  x.r.Step();
  const snd_when nb = Kind(x.T(), SND_WHEN_NEXT_BAR);
  x.r.sys->Play(HashId("H0"), snd_emitter_handle{0}, &nb);
  x.r.RunTo(c1 + 100000);
  const std::uint64_t c2 = x.r.Clock();
  const snd_musical_time fwd{6, 2, 0};
  x.r.sys->SeekTransport(x.T(), &fwd, nullptr);
  x.r.Step();
  const snd_when ba = Kind(x.T(), SND_WHEN_BARS_AFTER, 1);
  x.r.sys->Play(HashId("H1"), snd_emitter_handle{0}, &ba);
  x.r.RunTo(c2 + 100000);
  x.r.sys->Update();
  std::uint64_t f1 = 0, f2 = 0;
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "f", c1, &f1), c1);
  SND_CHECK_EQ(f1, 20000u);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "f", c2, &f2), c2);
  SND_CHECK_EQ(f2, 200000u);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "h0", c1), c1 + 60000);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "h1", c2), c2 + 40000);
  auto m = BeatMap(BeatsOf(x.r, x.T()));
  std::map<std::uint64_t, std::pair<int, int>> byClock;
  for (const Beat& e : BeatsOf(x.r, x.T())) byClock[e.clock] = {e.bar, e.beat};
  SND_CHECK(byClock.count(c1) == 1u && byClock[c1] == std::make_pair(0, 1));
  SND_CHECK(byClock.count(c1 + 20000) == 1u && byClock[c1 + 20000] == std::make_pair(0, 2));
  SND_CHECK(byClock.count(c1 + 60000) == 1u && byClock[c1 + 60000] == std::make_pair(1, 0));
  SND_CHECK(byClock.count(c2) == 1u && byClock[c2] == std::make_pair(6, 2));
  SND_CHECK(byClock.count(c2 + 40000) == 1u && byClock[c2 + 40000] == std::make_pair(7, 0));
  (void)m;
}

// --- D-71 / D-75: seeks across a signature change, and inside a block --------------------------------

namespace {
std::vector<std::uint8_t> TempoBank() {
  MusicProject p("recheck2-tempo");
  p.Asset("t", Material(kBar + 144000, 0.3f));
  p.Segment("T", "t", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120}, {\"tick\": 3840, \"bpm\": 60, \"beatsPerBar\": 3}]");
  p.Playlist("PL", "{\"segment\": \"T\"}");
  p.MusicEvent("Music", kOneState);
  p.Asset("h0", Material(2400, 0.5f));
  p.Event("H0", "\"tracks\": [{\"clips\": [{\"asset\": \"h0\"}]}]");
  return BuildBank(p);
}
}  // namespace

SND_TEST(MusicRecheck2, SeekForwardIntoAnotherSignatureAndAcrossTheSegmentEnd) {
  // T: bar 0 4/4 at 120, bar 1 3/4 at 60 (quarter = 48000). Seek from bar 0 to bar 1 beat 2
  // (frame 96000 + 96000): the next bar line (bar 2, next pass of T) is 48000 later; a
  // NEXT_BAR made right after lands there. Then seek to bar 5 beat 1: 5 mod 2 = bar 1 of T,
  // beat 1 -> frame 144000, announced as bar 5 beat 1.
  Music x(TempoBank());
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 30000);
  const std::uint64_t c1 = x.r.Clock();
  const snd_musical_time a{1, 2, 0};
  x.r.sys->SeekTransport(x.T(), &a, nullptr);
  x.r.Step();
  const snd_when nb = Kind(x.T(), SND_WHEN_NEXT_BAR);
  x.r.sys->Play(HashId("H0"), snd_emitter_handle{0}, &nb);
  x.r.RunTo(c1 + 100000);
  const std::uint64_t c2 = x.r.Clock();
  const snd_musical_time b{5, 1, 0};
  x.r.sys->SeekTransport(x.T(), &b, nullptr);
  x.r.RunTo(c2 + 10000);
  x.r.sys->Update();
  std::uint64_t f1 = 0, f2 = 0;
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "t", c1, &f1), c1);
  SND_CHECK_EQ(f1, 192000u);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "t", c1 + 1), c1 + 48000);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "h0", c1), c1 + 48000);
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "t", c2, &f2), c2);
  SND_CHECK_EQ(f2, 144000u);
  std::map<std::uint64_t, std::pair<int, int>> byClock;
  for (const Beat& e : BeatsOf(x.r, x.T())) byClock[e.clock] = {e.bar, e.beat};
  SND_CHECK(byClock.count(c1) == 1u && byClock[c1] == std::make_pair(1, 2));
  SND_CHECK(byClock.count(c1 + 48000) == 1u && byClock[c1 + 48000] == std::make_pair(2, 0));
  SND_CHECK(byClock.count(c1 + 72000) == 1u && byClock[c1 + 72000] == std::make_pair(2, 1));  // 4/4 at 120 again
  SND_CHECK(byClock.count(c2) == 1u && byClock[c2] == std::make_pair(5, 1));
}

SND_TEST(MusicRecheck2, SeekInsideABlockKeepsTheEarlierBeatOfThatBlock) {
  // Block [s0 + 23808, s0 + 24064) holds beat 0.1 (s0 + 24000); a seek at s0 + 24032
  // (CLOCK_SAMPLE) to bar 1 beat 0. Announced: 0.1 at 24000 (old map), 1.0 at 24032, 1.1 at
  // 48032; beat 0.2 never. The material is re-placed at bar 1 (frame 96000) at 24032.
  MusicProject p("recheck2-blockseek");
  bt::PcmData e = Material(2 * kBar, 0.2f);
  for (std::size_t i = kBar; i < 2 * kBar; ++i) e.samples[i] = 0.4f;
  p.Asset("e", e);
  p.Segment("E", "e", 2);
  p.Playlist("PL", "{\"segment\": \"E\"}");
  p.MusicEvent("Music", kOneState);
  Music x(BuildBank(p));
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 10000);
  snd_when at{};
  at.kind = SND_WHEN_CLOCK_SAMPLE;
  at.value = x.s0 + 24032;
  const snd_musical_time to{1, 0, 0};
  x.r.sys->SeekTransport(x.T(), &to, &at);
  x.r.RunTo(x.s0 + 60000);
  x.r.sys->Update();
  std::map<std::uint64_t, std::pair<int, int>> byClock;
  bool b02 = false;
  for (const Beat& b : BeatsOf(x.r, x.T())) {
    byClock[b.clock] = {b.bar, b.beat};
    b02 = b02 || (b.bar == 0 && b.beat == 2);
  }
  SND_CHECK(byClock.count(x.s0 + 24000) == 1u && byClock[x.s0 + 24000] == std::make_pair(0, 1));
  SND_CHECK(byClock.count(x.s0 + 24032) == 1u && byClock[x.s0 + 24032] == std::make_pair(1, 0));
  SND_CHECK(byClock.count(x.s0 + 48032) == 1u && byClock[x.s0 + 48032] == std::make_pair(1, 1));
  SND_CHECK(!b02);
  std::uint64_t f = 0;
  SND_CHECK_EQ(FirstStartAtOrAfter(x.r, "e", x.s0 + 1, &f), x.s0 + 24032);
  SND_CHECK_EQ(f, static_cast<std::uint64_t>(kBar));
  SND_CHECK_NEAR(x.r.At(x.s0 + 24032 + 500), 0.4f, 1e-4f);  // after the 5 ms short fade of the old voice
}
