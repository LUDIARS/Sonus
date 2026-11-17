// Fixed defect (independent music tests; was Defect_music_stall_beat_twice, fixed in 7cfb9fe): with a "sound priority" sync group (8.4) a
// one-block stall makes the next beat be notified twice.
//
// Expected (8.4, 8.1): "sound priority" holds the whole transport until the data
// is there, so every beat is announced once, when the transport reaches it
// (here 256 samples late). Beat notifications are one per beat (on_beat).
// Seen: bar 0 beat 2 is announced at its undelayed clock (s0 + 48000, emitted
// before the stall is detected one block later) and again at s0 + 48256 after
// the transport was delayed: the beat cursor is not shifted with the time line.
// Reproduce: segment on a sound-priority asset, starve it during block 186
// (the block before the one that holds beat 2).
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepFixed, SoundPriorityStallAnnouncesABeatOnce) {
  MusicProject p("defect-stall");
  p.Asset("s", Material(kBar, 0.3f), "\"stallPolicy\": \"sound\"");
  p.Segment("S1", "s", 1);
  p.Playlist("PL", "{\"segment\": \"S1\"}", "\"loopCount\": 2");
  p.MusicEvent("Music", kOneState);
  Rig r;
  SND_REQUIRE(r.Load(BuildBank(p)));
  SND_REQUIRE_EQ(r.sys->BlockFrames(), 256u);
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.kind.stallAsset.store(HashId("s"));
  r.kind.stallFrom.store(s0 + 186 * 256);
  r.kind.stallTo.store(s0 + 187 * 256);  // one block starved, just before beat 2 (s0 + 48000)
  r.RunTo(s0 + kBar);
  r.sys->Update();
  std::uint32_t n = 0;
  std::uint64_t last = 0;
  for (const Beat& b : BeatsOf(r, OwnedTransport(m))) {
    if (b.bar == 0 && b.beat == 2) {
      ++n;
      last = b.clock;
      std::fprintf(stderr, "bar 0 beat 2 at s0 + %llu\n", static_cast<unsigned long long>(b.clock - s0));
    }
  }
  SND_CHECK_EQ(n, 1u);
  SND_CHECK_EQ(last, s0 + 48000 + 256);
}
