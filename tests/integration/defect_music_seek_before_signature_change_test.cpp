// Defect (independent music tests): seeking the music transport to a position
// before a time-signature change that has already been passed lands on the
// wrong tick.
//
// Expected (D-20, 8.1): the transport position bar 0 / beat 2 / tick 0 is tick
// 1920 (bar 0 is 4/4), whatever the current signature is. Segment T: bar 0 4/4
// at 120 BPM, bar 1 3/4 at 60 BPM. Seeking there from inside bar 1 must
// re-place T at 1920 ticks = 48000 frames (and announce bar 0 beat 2 there).
// Seen: the time line drops tempo points once they are passed, so the bar
// position is extrapolated backwards with the 3/4 bar length: the seek goes to
// tick 2880 and T is re-placed at frame 72000 (bar 0 beat 3 of the material),
// while the beat at the seek clock is still announced as bar 0 beat 2 (the same
// extrapolation run backwards): what is heard and what is announced disagree.
// Reproduce: play T, seek to {0, 2, 0} at s0 + 120000.
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepDefect, SeekBackOverASignatureChange) {
  MusicProject p("defect-seek-sig");
  p.Asset("t", Material(kBar + 144000, 0.3f));
  p.Segment("T", "t", 2, "\"tempo\": [{\"tick\": 0, \"bpm\": 120}, {\"tick\": 3840, \"bpm\": 60, \"beatsPerBar\": 3}]");
  p.Playlist("PL", "{\"segment\": \"T\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildBank(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 120000);  // bar 1 (3/4)
  const std::uint64_t c = r.Clock();
  const snd_musical_time to{0, 2, 0};
  r.sys->SeekTransport(OwnedTransport(m), &to, nullptr);
  r.RunTo(c + 4096);
  r.sys->Update();
  bool placed = false;
  for (const StartLog& s : StartsOf(r, "t")) {
    if (s.clock == c) {
      placed = true;
      std::fprintf(stderr, "re-placed at frame %llu\n", static_cast<unsigned long long>(s.frame));
      SND_CHECK_EQ(s.frame, 48000u);
    }
  }
  SND_CHECK(placed);
  bool announced = false;
  for (const Beat& b : BeatsOf(r, OwnedTransport(m))) {
    if (b.clock == c) {
      std::fprintf(stderr, "beat at the seek: %d.%d\n", b.bar, b.beat);
      announced = b.bar == 0 && b.beat == 2;
    }
  }
  SND_CHECK(announced);
}
