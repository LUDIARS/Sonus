// Defect (independent music tests): SetMusicState while the music playback is
// paused is dropped.
//
// Expected (8.6, D-09): SetMusicState is a must-deliver command; in Idle a new
// state "looks up the matching rule and goes to Pending". Nothing in 8.6 / 4.1
// exempts a paused playback, so after resume the transition happens at the
// rule's timing (here the next bar).
// Seen: HandleSetState returns early when the transport is not Playing (a
// paused playback pauses its transport): the request is lost, the music stays
// in Explore forever.
// Reproduce: snd_pause the music, snd_set_music_state, snd_resume_playback.
#include "music_indep_util.hpp"

using namespace mi;

SND_TEST(MusicIndepDefect, StateRequestedWhilePausedHappensAfterResume) {
  MusicProject p("defect-paused");
  p.Asset("e", Material(2 * kBar, 0.2f));
  p.Asset("bt", Material(2 * kBar, 0.6f));
  p.Segment("E", "e", 2);
  p.Segment("Bt", "bt", 2);
  p.Playlist("PE", "{\"segment\": \"E\"}");
  p.Playlist("PBt", "{\"segment\": \"Bt\"}");
  p.MusicEvent("Music",
               "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PE\"},"
               " {\"name\": \"Battle\", \"playlist\": \"PBt\"}], \"transitions\": [" +
                   Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0") + "]");
  Rig r;
  SND_REQUIRE(r.Load(BuildBank(p)));
  const std::uint64_t s0 = r.Clock();
  const snd_playback_handle m = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  r.RunTo(s0 + 30000);
  r.sys->Pause(m, 0.0f);
  r.RunTo(s0 + 60000);
  r.sys->SetMusicState(m, HashId("Battle"), nullptr);
  r.RunTo(s0 + 100000);
  r.sys->ResumePlayback(m, 0.0f);
  r.RunTo(s0 + 100000 + 2 * kBar);
  snd::music::MusicPlaybackInfo info;
  SND_REQUIRE(r.music.GetPlaybackInfo(SlotOf(m), &info));
  SND_CHECK_EQ(info.state, HashId("Battle"));
  SND_CHECK(!StartsOf(r, "bt").empty());
}
