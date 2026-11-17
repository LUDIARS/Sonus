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
