// Defect (independent music re-check of D-78): during a sound-priority stall that
// lasts several blocks, a beat is announced much earlier than the transport
// reaches it.
//
// Expected (D-78, design 2.5): the stall is seen one block late, so a beat
// announced before the detection may keep its undelayed clock, "at most one
// block" off. Four blocks starved from s0 + 92 * 256: the transport stops before
// beat 1 (s0 + 24000) and reaches it 1024 samples later (s0 + 25024, as every
// later beat: beat 2 at s0 + 49024). The announcement may be at most 256 early.
// Seen: beat 1 is announced at s0 + 24256 (768 early): it was queued in block 93,
// shifted once by the deficit of block 92, and published; the deficits of blocks
// 93..95 found later do not move it. The bound of D-78 holds only for a one-block
// stall; for an N-block stall the error is N - 1 blocks.
// Reproduce: sound-priority segment, starve blocks 92..95.
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepDefect, LongStallAnnouncesABeatAtMostOneBlockEarly) {
  MusicProject p("defect-long-stall");
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
  r.kind.stallFrom.store(s0 + 92 * 256);
  r.kind.stallTo.store(s0 + 96 * 256);
  r.RunTo(s0 + kBar);
  r.sys->Update();
  std::uint64_t beat1 = 0, beat2 = 0;
  for (const Beat& b : BeatsOf(r, OwnedTransport(m))) {
    if (b.bar == 0 && b.beat == 1) beat1 = b.clock;
    if (b.bar == 0 && b.beat == 2) beat2 = b.clock;
  }
  SND_REQUIRE_EQ(beat2, s0 + 48000 + 1024);  // the transport is 1024 late after the stall
  std::fprintf(stderr, "beat 1 announced at s0 + %llu, reached at s0 + 25024\n",
               static_cast<unsigned long long>(beat1 - s0));
  SND_CHECK(beat1 + 256u >= s0 + 25024u);
}
