// Music segments and playlists through core (8.3): sample-exact concatenation,
// pickup and tail, tempo maps and beat notices, playlist order / random /
// shuffle with fixed seeds, repeats and loop counts, markers.
#include <cmath>
#include <map>
#include <vector>

#include "music_fixture.hpp"

using namespace mt;

namespace {

// Two 1-bar segments A (level 0.25) and B (level 0.5).
std::vector<std::uint8_t> TwoSegments(const std::string& playlistExtra, const std::string& entries) {
  MusicProject p("seg-two");
  p.Asset("a", Material(kBar, 0.25f));
  p.Asset("b", Material(kBar, 0.5f));
  p.Segment("SegA", "a", 1);
  p.Segment("SegB", "b", 1);
  p.Playlist("PL", entries, playlistExtra);
  p.MusicEvent("Music", "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]");
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

}  // namespace

SND_TEST(MusicSegment, ConcatenatesSampleExactlyAndEndsWithThePlaylist) {
  Rig r;
  SND_REQUIRE(r.sys != nullptr);
  SND_REQUIRE(r.Load(TwoSegments("\"loopCount\": 1", "{\"segment\": \"SegA\"}, {\"segment\": \"SegB\"}")));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  SND_REQUIRE(p.value != 0u);
  r.RunTo(s0 + 2 * kBar + 4 * 256);
  // The voices start exactly on the bar lines (start log of the source kind).
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE_EQ(st.size(), std::size_t{2});
  SND_CHECK_EQ(st[0].clock, s0);
  SND_CHECK_EQ(st[0].frame, 0u);
  SND_CHECK_EQ(st[0].asset, HashId("a"));
  SND_CHECK_EQ(st[1].clock, s0 + kBar);
  SND_CHECK_EQ(st[1].frame, 0u);
  SND_CHECK_EQ(st[1].asset, HashId("b"));
  // Audio: A's level in bar 0, B's in bar 1, the steps where they belong, then silence.
  SND_CHECK_NEAR(r.At(s0 + kBar / 2), 0.25f, 1e-4f);
  SND_CHECK_NEAR(r.At(s0 + kBar + kBar / 2), 0.5f, 1e-4f);
  SND_CHECK(Diff(Crossing(r, s0 - 100, s0 + 100, 0.125f), s0) <= 1u);
  SND_CHECK(Diff(Crossing(r, s0 + kBar - 100, s0 + kBar + 100, 0.375f), s0 + kBar) <= 1u);
  SND_CHECK(Diff(Crossing(r, s0 + 2 * kBar - 100, s0 + 2 * kBar + 100, 0.25f), s0 + 2 * kBar) <= 1u);
  SND_CHECK_NEAR(r.At(s0 + 2 * kBar + 200), 0.0f, 1e-4f);
  // Beat notices on the same clocks (bar / beat numbers from 0).
  r.sys->Update();
  std::uint32_t seen = 0;
  for (const Beat& b : r.ev.beats) {
    if (b.clock < s0 || b.clock >= s0 + 2 * kBar) continue;
    SND_CHECK_EQ((b.clock - s0) % kBeat, 0u);
    const std::uint64_t k = (b.clock - s0) / kBeat;
    SND_CHECK_EQ(b.bar, static_cast<std::int32_t>(k / 4));
    SND_CHECK_EQ(b.beat, static_cast<std::int32_t>(k % 4));
    SND_CHECK_EQ(b.transport, OwnedTransport(p).value);
    ++seen;
  }
  SND_CHECK_EQ(seen, 8u);
  snd::music::MusicPlaybackInfo info;
  SND_CHECK(r.music.GetPlaybackInfo(SlotOf(p), &info));
  SND_CHECK_EQ(info.state, HashId("S"));
}

namespace {

// Mono material made of constant-level pieces {frames, level}.
bt::PcmData Pieces(std::initializer_list<std::pair<std::uint64_t, float>> parts) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = kRate;
  for (const auto& [n, level] : parts) p.samples.insert(p.samples.end(), static_cast<std::size_t>(n), level);
  p.frames = p.samples.size();
  return p;
}

std::vector<std::uint8_t> BuildOne(MusicProject& p) {
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

const char* kOneState = "\"initialState\": \"S\", \"states\": [{\"name\": \"S\", \"playlist\": \"PL\"}]";

std::vector<snd::Id> AssetSequence(const Rig& r) {
  std::vector<snd::Id> v;
  for (const StartLog& s : r.kind.Starts()) v.push_back(s.asset);
  return v;
}

// 480 BPM, 1 bar = 24000 samples: short segments for playlist statistics.
const char* kFast = "\"tempo\": [{\"tick\": 0, \"bpm\": 480}]";
constexpr std::uint32_t kFastBar = 24000;

}  // namespace

SND_TEST(MusicSegment, PickupLeadsAndTailOverlapsTheNextSegment) {
  // P: pickup 12000 (0.1) | 1 bar body (0.2) | tail 12000 (0.05); bar 0 at frame 12000.
  MusicProject p("seg-pickup");
  p.Asset("p", Pieces({{12000, 0.1f}, {kBar, 0.2f}, {12000, 0.05f}}));
  p.Segment("P", "p", 1, "\"entryFrame\": 12000");
  p.Playlist("PL", "{\"segment\": \"P\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 3 * kBar);
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE_EQ(st.size(), std::size_t{2});
  SND_CHECK_EQ(st[0].clock, s0);                 // the pickup starts at the play clock
  SND_CHECK_EQ(st[1].clock, s0 + kBar);          // next bar 0 at s0 + 12000 + 1 bar, pickup before it
  SND_CHECK_EQ(st[1].frame, 0u);
  SND_CHECK_NEAR(r.At(s0 + 6000), 0.1f, 1e-4f);              // pickup
  SND_CHECK_NEAR(r.At(s0 + 50000), 0.2f, 1e-4f);             // body
  SND_CHECK_NEAR(r.At(s0 + kBar + 6000), 0.3f, 1e-4f);       // body + next pickup
  SND_CHECK_NEAR(r.At(s0 + kBar + 18000), 0.25f, 1e-4f);     // tail + next body
  SND_CHECK_NEAR(r.At(s0 + 2 * kBar + 18000), 0.05f, 1e-4f); // last tail
  SND_CHECK_NEAR(r.At(s0 + 2 * kBar + 30000), 0.0f, 1e-4f);
  // Bar 0 beat 0 is announced at the entry, not at the pickup.
  r.sys->Update();
  bool found = false;
  for (const Beat& b : r.ev.beats) {
    if (b.bar == 0 && b.beat == 0 && b.clock >= s0) {
      SND_CHECK_EQ(b.clock, s0 + 12000);
      found = true;
      break;
    }
  }
  SND_CHECK(found);
}

SND_TEST(MusicSegment, TempoMapAndSignatureChangesDriveTheBeats) {
  // T: bar 0 at 120 BPM 4/4 (96000), bar 1 at 60 BPM 3/4 (3 x 48000); played twice.
  MusicProject p("seg-tempo");
  p.Asset("t", Material(kBar + 144000, 0.3f));
  p.Segment("T", "t", 2,
            "\"tempo\": [{\"tick\": 0, \"bpm\": 120}, {\"tick\": 3840, \"bpm\": 60, \"beatsPerBar\": 3}]");
  p.Playlist("PL", "{\"segment\": \"T\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 2 * (kBar + 144000) + 1024);
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE_EQ(st.size(), std::size_t{2});
  SND_CHECK_EQ(st[1].clock, s0 + kBar + 144000);
  r.sys->Update();
  struct E {
    std::int32_t bar, beat;
    std::uint64_t at;
  };
  const E expect[] = {{0, 0, 0},      {0, 1, 24000},  {0, 2, 48000},  {0, 3, 72000},  {1, 0, 96000},
                      {1, 1, 144000}, {1, 2, 192000}, {2, 0, 240000}, {2, 1, 264000}, {2, 2, 288000},
                      {2, 3, 312000}, {3, 0, 336000}, {3, 1, 384000}};
  std::size_t i = 0;
  for (const Beat& b : r.ev.beats) {
    if (b.clock < s0 || i >= sizeof(expect) / sizeof(expect[0])) continue;
    SND_CHECK_EQ(b.bar, expect[i].bar);
    SND_CHECK_EQ(b.beat, expect[i].beat);
    SND_CHECK_EQ(b.clock, s0 + expect[i].at);
    ++i;
  }
  SND_CHECK_EQ(i, sizeof(expect) / sizeof(expect[0]));
}

SND_TEST(MusicSegment, SequentialPlaylistRepeatsAndLoops) {
  MusicProject p("seg-seq");
  p.Asset("a", Material(kFastBar, 0.1f));
  p.Asset("b", Material(kFastBar, 0.2f));
  p.Segment("A", "a", 1, kFast);
  p.Segment("B", "b", 1, kFast);
  p.Playlist("PL", "{\"segment\": \"A\", \"repeat\": 2}, {\"segment\": \"B\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 8 * kFastBar);
  const std::vector<StartLog> st = r.kind.Starts();
  const snd::Id a = HashId("a"), b = HashId("b");
  const std::vector<snd::Id> want{a, a, b, a, a, b};
  SND_REQUIRE_EQ(st.size(), want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    SND_CHECK_EQ(st[i].asset, want[i]);
    SND_CHECK_EQ(st[i].clock, s0 + i * kFastBar);
  }
  SND_CHECK_NEAR(r.At(s0 + 6 * kFastBar + 1000), 0.0f, 1e-4f);
}

namespace {
std::vector<std::uint8_t> RandomBank(const char* mode, const char* loop) {
  MusicProject p(std::string("seg-") + mode);
  p.Asset("x", Material(kFastBar, 0.1f));
  p.Asset("y", Material(kFastBar, 0.2f));
  p.Asset("z", Material(kFastBar, 0.3f));
  p.Asset("w", Material(kFastBar, 0.4f));
  p.Segment("X", "x", 1, kFast);
  p.Segment("Y", "y", 1, kFast);
  p.Segment("Z", "z", 1, kFast);
  p.Segment("W", "w", 1, kFast);
  if (std::string(mode) == "random") {
    p.Playlist("PL", "{\"segment\": \"X\"}, {\"segment\": \"Y\"}, {\"segment\": \"Z\", \"weight\": 2}",
               std::string("\"mode\": \"random\", \"loopCount\": ") + loop);
  } else {
    p.Playlist("PL", "{\"segment\": \"X\"}, {\"segment\": \"Y\"}, {\"segment\": \"Z\"}, {\"segment\": \"W\"}",
               std::string("\"mode\": \"shuffle\", \"loopCount\": ") + loop);
  }
  p.MusicEvent("Music", kOneState);
  return BuildOne(p);
}

std::vector<snd::Id> PlayFor(const std::vector<std::uint8_t>& bank, std::uint32_t seed, std::uint32_t bars) {
  snd_config c = Config();
  c.seed = seed;
  Rig r(c);
  if (!r.Load(bank)) return {};
  const std::uint64_t s0 = r.Clock();
  r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + bars * kFastBar + 512);
  std::vector<snd::Id> v = AssetSequence(r);
  // Every segment starts exactly one bar after the previous one.
  const std::vector<StartLog> st = r.kind.Starts();
  for (std::size_t i = 0; i < st.size(); ++i) {
    if (st[i].clock != s0 + i * kFastBar) return {};
  }
  return v;
}
}  // namespace

SND_TEST(MusicSegment, RandomPlaylistIsWeightedAndReproducibleFromTheSeed) {
  const std::vector<std::uint8_t> bank = RandomBank("random", "0");
  SND_REQUIRE(!bank.empty());
  const std::vector<snd::Id> a = PlayFor(bank, 1234u, 48);
  const std::vector<snd::Id> b = PlayFor(bank, 1234u, 48);
  const std::vector<snd::Id> c = PlayFor(bank, 99u, 48);
  SND_REQUIRE(a.size() >= 48u);
  SND_CHECK(a == b);  // 2.5: same seed, same decisions
  SND_CHECK(a != c);
  std::map<snd::Id, std::uint32_t> n;
  for (std::size_t i = 0; i < 48; ++i) ++n[a[i]];
  SND_CHECK(n[HashId("z")] >= 14u && n[HashId("z")] <= 34u);  // weight 2 of 4: ~24 of 48
  SND_CHECK(n[HashId("x")] >= 4u && n[HashId("y")] >= 4u);
  SND_CHECK_EQ(n[HashId("w")], 0u);
}

SND_TEST(MusicSegment, ShufflePlaysEveryEntryOncePerPass) {
  const std::vector<std::uint8_t> bank = RandomBank("shuffle", "3");
  SND_REQUIRE(!bank.empty());
  const std::vector<snd::Id> a = PlayFor(bank, 42u, 14);
  SND_REQUIRE_EQ(a.size(), std::size_t{12});  // 3 passes of 4, then the playlist ends
  for (std::size_t pass = 0; pass < 3; ++pass) {
    std::map<snd::Id, std::uint32_t> n;
    for (std::size_t i = 0; i < 4; ++i) ++n[a[pass * 4 + i]];
    SND_CHECK_EQ(n.size(), std::size_t{4});
    if (pass > 0) SND_CHECK(a[pass * 4] != a[pass * 4 - 1]);  // no repeat across the pass boundary
  }
  SND_CHECK(a == PlayFor(bank, 42u, 14));
}

SND_TEST(MusicSegment, MarkersAreNotifiedOnTheirSample) {
  MusicProject p("seg-markers");
  p.Asset("m", Material(kBar, 0.2f));
  p.Segment("M", "m", 1,
            "\"markers\": [{\"name\": \"M1\", \"frame\": 30000}, {\"name\": \"M2\", \"frame\": 70000, \"kind\": \"user\"}]");
  p.Playlist("PL", "{\"segment\": \"M\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle h = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 2 * kBar + 512);
  r.sys->Update();
  std::vector<std::pair<snd::Id, std::uint64_t>> got;
  for (const Marker& m : r.ev.markers) {
    SND_CHECK_EQ(m.playback, h.value);
    got.emplace_back(m.id, m.clock - s0);
  }
  const std::vector<std::pair<snd::Id, std::uint64_t>> want{
      {HashId("M1"), 30000}, {HashId("M2"), 70000}, {HashId("M1"), kBar + 30000}, {HashId("M2"), kBar + 70000}};
  SND_CHECK(got == want);
}

SND_TEST(MusicSegment, StemVolumesFollowTheirParameters) {
  // Main "pad" (0.2) with a full-cover stem curve on "Calm" (0 dB .. -6 dB), and a separate
  // file stem "drums" (0.1) on "Intensity" (-6 dB .. 0 dB). Same start, same sync group.
  MusicProject p("seg-stems");
  p.Parameter("Intensity", "\"min\": 0, \"max\": 1, \"default\": 0, \"scope\": [\"playback\"]");
  p.Parameter("Calm", "\"min\": 0, \"max\": 1, \"default\": 0, \"scope\": [\"playback\"]");
  p.Asset("pad", Material(4 * kBar, 0.2f));
  p.Asset("drums", Material(4 * kBar, 0.1f));
  p.Segment("S", "pad", 4,
            "\"stems\": [{\"channelFirst\": 0, \"channelCount\": 1, \"volumeCurve\": {\"parameter\": \"Calm\", "
            "\"target\": \"volume\", \"points\": [[0, 0], [1, -6.0206]]}},"
            " {\"asset\": \"drums\", \"channelFirst\": 0, \"channelCount\": 1, \"volumeCurve\": {\"parameter\": "
            "\"Intensity\", \"target\": \"volume\", \"points\": [[0, -6.0206], [1, 0]]}}]");
  p.Playlist("PL", "{\"segment\": \"S\"}");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildOne(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle h = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 20000);
  const std::vector<StartLog> st = r.kind.Starts();
  SND_REQUIRE_EQ(st.size(), std::size_t{2});
  SND_CHECK_EQ(st[0].clock, s0);
  SND_CHECK_EQ(st[1].clock, s0);
  SND_CHECK_EQ(r.kind.syncGroups[st[0].voice % 64u], r.kind.syncGroups[st[1].voice % 64u]);
  SND_CHECK(r.kind.syncGroups[st[0].voice % 64u] != 0u);
  SND_CHECK_NEAR(r.At(s0 + 10000), 0.2f + 0.05f, 2e-4f);
  r.sys->SetParamPlayback(h, HashId("Intensity"), 1.0f);
  const std::uint64_t t1 = r.Clock();
  r.RunTo(t1 + 4096);
  SND_CHECK_NEAR(r.At(t1 + 3000), 0.2f + 0.1f, 2e-4f);
  r.sys->SetParamPlayback(h, HashId("Calm"), 1.0f);
  const std::uint64_t t2 = r.Clock();
  r.RunTo(t2 + 4096);
  SND_CHECK_NEAR(r.At(t2 + 3000), 0.1f + 0.1f, 2e-4f);
  // The change ramps over one block (no step).
  float maxStep = 0.0f;
  for (std::uint64_t c = t2; c < t2 + 2048; ++c) maxStep = std::max(maxStep, std::fabs(r.At(c + 1) - r.At(c)));
  SND_CHECK(maxStep < 0.1f / 128.0f);
}
