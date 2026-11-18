// Fixed defect (independent music tests; was Defect_music_samebar_other_signature, fixed in 7cfb9fe): "same bar position" between states with
// different time signatures keeps the beat inside the bar.
//
// Expected (8.5): "拍子が違うState間の「同じ小節位置」は、小節番号を遷移先Segmentの長さで
// 割った余りの小節の頭拍にする". Leaving E (4/4) in its bar 1 at beat 2 into W
// (3/4, 2 bars): W starts at its bar 1 mod 2 = 1, head beat = W tick 2880 =
// frame 72000, at the transition clock X.
// Seen: W starts at bar 1 + 2 beats (frame 120000): the remainder inside the bar
// is kept for every signature, not only for equal ones.
// Reproduce: rule nextBeat + sameBar, request at s0 + 1 bar + 30000.
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepFixed, SameBarIntoAnotherSignatureStartsOnTheHeadBeat) {
  MusicProject p("defect-samebar");
  p.Asset("e", Material(2 * kBar, 0.2f));
  p.Asset("w", Material(2 * 72000, 0.6f));
  p.Segment("E", "e", 2);
  p.Segment("W", "w", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120, \"beatsPerBar\": 3}]");
  p.Playlist("PE", "{\"segment\": \"E\"}");
  p.Playlist("PW", "{\"segment\": \"W\"}");
  p.MusicEvent("Music",
               "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PE\"},"
               " {\"name\": \"Waltz\", \"playlist\": \"PW\"}], \"transitions\": [" +
                   Rule("*", "*", "\"timing\": \"nextBeat\", \"start\": \"sameBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0") + "]");
  Rig r;
  SND_REQUIRE(r.Load(BuildBank(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + kBar + 30000);
  r.sys->SetMusicState(m, HashId("Waltz"), nullptr);
  const std::uint64_t X = s0 + kBar + 2 * kBeat;
  r.RunTo(X + kBar);
  const std::vector<StartLog> w = StartsOf(r, "w");
  SND_REQUIRE(!w.empty());
  SND_CHECK_EQ(w[0].clock, X);
  std::fprintf(stderr, "W entered at frame %llu\n", static_cast<unsigned long long>(w[0].frame));
  SND_CHECK_EQ(w[0].frame, 72000u);
}
