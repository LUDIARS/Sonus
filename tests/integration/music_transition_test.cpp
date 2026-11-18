// Transitions (design 8.5, 8.6) through core: rule timings, fade shapes and
// lengths, bridge segments, start positions (head / same bar / marker / last
// position), the state machine (pending overwrite, cancel, interrupt allowed /
// after bridge), the late rule (prime time), When overriding the rule timing,
// and the default rule when no row matches (8.7).
//
// Material (mono DC levels, 120 BPM 4/4, bar = 96000 samples at 48 kHz):
//   Explore: segment E  (2 bars, level 0.2)   Battle: segment Bt (2 bars, 0.6)
//   Boss:    segment Bo (1 bar, 0.4)          bridge: segment Br (1 bar, 0.9)
#include <cmath>
#include <string>
#include <vector>

#include "music_fixture.hpp"

using namespace mt;

namespace {

struct Setup {
  std::string transitions;
  std::string segE = "";        // extra members of segment E
  std::string segBt = "";       // extra members of segment Bt
};

std::vector<std::uint8_t> BuildStates(const Setup& s) {
  MusicProject p("trans");
  p.Asset("e", Material(2 * kBar, 0.2f));
  p.Asset("bt", Material(2 * kBar, 0.6f));
  p.Asset("bo", Material(kBar, 0.4f));
  p.Asset("br", Material(kBar, 0.9f));
  p.Segment("E", "e", 2, s.segE);
  p.Segment("Bt", "bt", 2, s.segBt);
  p.Segment("Bo", "bo", 1);
  p.Segment("Br", "br", 1);
  p.Playlist("PE", "{\"segment\": \"E\"}");
  p.Playlist("PBt", "{\"segment\": \"Bt\"}");
  p.Playlist("PBo", "{\"segment\": \"Bo\"}");
  std::string music =
      "\"initialState\": \"Explore\", \"states\": [{\"name\": \"Explore\", \"playlist\": \"PE\"},"
      " {\"name\": \"Battle\", \"playlist\": \"PBt\"}, {\"name\": \"Boss\", \"playlist\": \"PBo\"}]";
  if (!s.transitions.empty()) music += ", \"transitions\": [" + s.transitions + "]";
  p.MusicEvent("Music", music);
  std::string diag;
  std::vector<std::uint8_t> bank = p.Build(&diag);
  if (bank.empty()) std::fprintf(stderr, "%s\n", diag.c_str());
  return bank;
}

// First start of `asset` at or after `from`.
bool StartOf(const Rig& r, const char* asset, std::uint64_t from, StartLog* out) {
  for (const StartLog& s : r.kind.Starts()) {
    if (s.asset == HashId(asset) && s.clock >= from) {
      *out = s;
      return true;
    }
  }
  return false;
}
std::uint32_t CountStarts(const Rig& r, const char* asset) {
  std::uint32_t n = 0;
  for (const StartLog& s : r.kind.Starts()) n += s.asset == HashId(asset) ? 1u : 0u;
  return n;
}

// A rig with the bank loaded and the music started at block start s0.
struct Session {
  Rig r;
  snd_playback_handle p{};
  std::uint64_t s0 = 0;
  explicit Session(const Setup& setup) {
    if (r.sys == nullptr || !r.Load(BuildStates(setup))) return;
    s0 = r.Clock();
    p = r.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  }
  bool ok() const { return p.value != 0u; }
  // Sets the state when the clock reaches `at` (block granularity: the request is
  // applied at the first block starting at or after `at`).
  void SetAt(std::uint64_t at, const char* state, const snd_when* when = nullptr) {
    r.RunTo(at);
    r.sys->SetMusicState(p, HashId(state), when);
  }
  snd::music::MusicPlaybackInfo Info() const {
    snd::music::MusicPlaybackInfo i;
    r.music.GetPlaybackInfo(SlotOf(p), &i);
    return i;
  }
};

std::string Rule(const std::string& from, const std::string& to, const std::string& more) {
  return "{\"from\": \"" + from + "\", \"to\": \"" + to + "\"" + (more.empty() ? "" : ", " + more) + "}";
}

}  // namespace

SND_TEST(MusicTransition, DefaultRuleWhenNothingMatches) {
  // 8.7: no rule rows at all -> next bar, 1 beat out / 1 beat in.
  Session x(Setup{});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 30000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.r.RunTo(X + kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, X);
  SND_CHECK_EQ(st.frame, 0u);
  // Explore fades out over one beat from X (exact linear release), Battle fades in over one beat.
  SND_CHECK_NEAR(x.r.At(X - 1000), 0.2f, 1e-4f);
  SND_CHECK_NEAR(x.r.At(X + kBeat + 2000), 0.6f, 1e-4f);
  const float mid = x.r.At(X + kBeat / 2);
  SND_CHECK_NEAR(mid, 0.2f * 0.5f + 0.6f * 0.5f, 0.6f * 256.0f / kBeat + 1e-3f);
  const snd::music::MusicPlaybackInfo i = x.Info();
  SND_CHECK_EQ(i.state, HashId("Battle"));
  SND_CHECK_EQ(i.transitions, 1u);
}

SND_TEST(MusicTransition, FadeOutIsLinearAndExactlyAsLongAsTheRuleSays) {
  // Fade out 2 beats at 120 BPM = 48000 samples from the bar line; no fade in.
  Session x(Setup{Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"fadeOutBeats\": 2, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.r.RunTo(X + kBar);
  // Output = Battle (0.6 from X) + Explore x (1 - t / 48000).
  for (std::uint64_t t = 1000; t < 48000; t += 4700) {
    const float expect = 0.6f + 0.2f * (1.0f - static_cast<float>(t) / 48000.0f);
    SND_CHECK_NEAR(x.r.At(X + t), expect, 2e-4f);
  }
  SND_CHECK_NEAR(x.r.At(X + 48000 + 500), 0.6f, 1e-4f);
  SND_CHECK_NEAR(x.r.At(X - 500), 0.2f, 1e-4f);
}

SND_TEST(MusicTransition, FadeInFollowsTheDestinationTempo) {
  // Fade in 2 beats; cut out (short fade). Battle at 60 BPM: 2 beats = 96000 samples.
  Setup s{Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 2")};
  s.segBt = "\"tempo\": [{\"tick\": 0, \"bpm\": 60}]";
  Session x(s);
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.r.RunTo(X + 2 * kBar);
  const float tol = 0.6f * 256.0f / 96000.0f + 1e-3f;  // per-block envelope steps
  SND_CHECK_NEAR(x.r.At(X + 24000), 0.6f * 0.25f, tol);
  SND_CHECK_NEAR(x.r.At(X + 48000), 0.6f * 0.5f, tol);
  SND_CHECK_NEAR(x.r.At(X + 72000), 0.6f * 0.75f, tol);
  SND_CHECK_NEAR(x.r.At(X + 96000 + 300), 0.6f, 1e-4f);
  // Explore is gone within the short fade (5 ms).
  SND_CHECK_NEAR(x.r.At(X + 300) - 0.6f * 300.0f / 96000.0f, 0.0f, tol);
}

SND_TEST(MusicTransition, RuleTimingsLandOnTheirGrid) {
  struct Case {
    const char* timing;
    std::uint64_t request;   // offset from s0
    std::uint64_t expected;  // offset from s0
  };
  const Case cases[] = {
      {"\"timing\": \"nextBeat\"", 30000, 48000},
      {"\"timing\": \"nextBar\"", 30000, kBar},
      {"\"timing\": \"barsAfter\", \"timingCount\": 2", 30000, 2 * kBar},
      {"\"timing\": \"grid\", \"timingCount\": 8", 30000, 36000},
      {"\"timing\": \"grid\", \"timingCount\": 32", 30000, 33000},
      {"\"timing\": \"segmentEnd\"", 30000, 2 * kBar},
      {"\"timing\": \"immediate\"", 30000, 30208},  // the block start where the request is applied
  };
  for (const Case& c : cases) {
    Session x(Setup{Rule("*", "*", std::string(c.timing) + ", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
    SND_REQUIRE(x.ok());
    x.SetAt(x.s0 + c.request, "Battle");
    x.r.RunTo(x.s0 + 3 * kBar);
    StartLog st{};
    SND_CHECK_MSG(StartOf(x.r, "bt", x.s0, &st) && st.clock == x.s0 + c.expected, "%s: start %llu, expected %llu",
                  c.timing, static_cast<unsigned long long>(st.clock - x.s0),
                  static_cast<unsigned long long>(c.expected));
  }
}

SND_TEST(MusicTransition, NextMarkerTiming) {
  Setup s{Rule("*", "*", "\"timing\": \"nextMarker\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")};
  s.segE = "\"markers\": [{\"name\": \"Hit\", \"frame\": 60000}, {\"name\": \"Hit2\", \"frame\": 150000}]";
  Session x(s);
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 70000, "Battle");  // after the first marker: the second one
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + 150000);
  // Marker notices of E on their clocks.
  x.r.sys->Update();
  std::uint32_t hit = 0;
  for (const Marker& m : x.r.ev.markers) {
    if (m.id == HashId("Hit")) {
      SND_CHECK_EQ(m.clock, x.s0 + 60000);
      SND_CHECK_EQ(m.playback, x.p.value);
      ++hit;
    }
  }
  SND_CHECK_EQ(hit, 1u);
}

SND_TEST(MusicTransition, BridgeSegmentIsInsertedBetweenTheStates) {
  Session x(Setup{Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"bridge\": \"Br\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 5000, "Battle");
  x.r.RunTo(x.s0 + 4 * kBar);
  StartLog br{}, bt{};
  SND_REQUIRE(StartOf(x.r, "br", x.s0, &br));
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &bt));
  SND_CHECK_EQ(br.clock, x.s0 + kBar);
  SND_CHECK_EQ(bt.clock, x.s0 + 2 * kBar);
  SND_CHECK_NEAR(x.r.At(x.s0 + kBar + kBar / 2), 0.9f, 1e-4f);
  SND_CHECK_NEAR(x.r.At(x.s0 + 2 * kBar + kBar / 2), 0.6f, 1e-4f);
  SND_CHECK_EQ(x.Info().phase, 0u);  // transition complete
}

SND_TEST(MusicTransition, SameBarStartsAtTheCorrespondingBar) {
  // Leaving E in its bar 1, beat 2 (s0 + 1 bar + 2 beats): Bt enters at its bar 1, beat 2.
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBeat\", \"start\": \"sameBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + kBar + 30000, "Battle");
  const std::uint64_t X = x.s0 + kBar + 48000;
  x.r.RunTo(x.s0 + 4 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, X);
  SND_CHECK_EQ(st.frame, static_cast<std::uint64_t>(kBar + 48000));
  SND_CHECK_NEAR(x.r.At(X + 5000), 0.6f, 1e-4f);
  // Bt's bar 0 lies 1.5 bars before X on the transport: its next loop starts at X + 0.5 bar.
  StartLog next{};
  SND_REQUIRE(StartOf(x.r, "bt", X + 1, &next));
  SND_CHECK_EQ(next.clock, X + kBar / 2);
  SND_CHECK_EQ(next.frame, 0u);
}

SND_TEST(MusicTransition, SameBarWrapsByTheDestinationLength) {
  // 8.5: bar n of a 2-bar segment into a 1-bar segment -> bar n mod 1 = 0.
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBeat\", \"start\": \"sameBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + kBar + 30000, "Boss");
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bo", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + kBar + 48000);
  SND_CHECK_EQ(st.frame, 48000u);  // bar 0, same beat
}

SND_TEST(MusicTransition, MarkerStartPosition) {
  Setup s{Rule("*", "*", "\"timing\": \"nextBar\", \"start\": \"marker\", \"startMarker\": \"Drop\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")};
  s.segBt = "\"markers\": [{\"name\": \"Drop\", \"frame\": 48000}]";
  Session x(s);
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + kBar);
  SND_CHECK_EQ(st.frame, 48000u);
  // Bt began half a bar early on the transport: its end (and the next Bt) is at s0 + 2.5 bars.
  StartLog next{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0 + kBar + 1, &next));
  SND_CHECK_EQ(next.clock, x.s0 + kBar + 2 * kBar - 48000);
}

SND_TEST(MusicTransition, LastPositionResumesWhereTheStateWasLeft) {
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBeat\", \"start\": \"lastPosition\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 100000, "Battle");  // leave E at s0 + 120000 (next beat): offset 120000
  x.r.RunTo(x.s0 + 150000);
  x.r.sys->SetMusicState(x.p, HashId("Explore"), nullptr);  // back at s0 + 168000
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog bt{}, e2{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &bt));
  SND_CHECK_EQ(bt.clock, x.s0 + 120000);
  SND_CHECK_EQ(bt.frame, 0u);  // Battle never played: from its head
  SND_REQUIRE(StartOf(x.r, "e", x.s0 + 1, &e2));
  SND_CHECK_EQ(e2.clock, x.s0 + 168000);
  SND_CHECK_EQ(e2.frame, 120000u);
}

SND_TEST(MusicTransition, PendingRequestIsOverwrittenOrCancelled) {
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 10000, "Battle");
  x.SetAt(x.s0 + 20000, "Boss");  // last request wins
  x.r.RunTo(x.s0 + kBar + 1000);
  SND_CHECK_EQ(CountStarts(x.r, "bt"), 0u);
  StartLog bo{};
  SND_REQUIRE(StartOf(x.r, "bo", x.s0, &bo));
  SND_CHECK_EQ(bo.clock, x.s0 + kBar);
  // Pending Explore then back to the current state (Boss): nothing happens.
  x.SetAt(x.s0 + kBar + 10000, "Explore");
  x.r.Step();
  SND_CHECK_EQ(x.Info().phase, 1u);
  x.SetAt(x.s0 + kBar + 20000, "Boss");
  x.r.RunTo(x.s0 + 3 * kBar);
  SND_CHECK_EQ(x.Info().state, HashId("Boss"));
  SND_CHECK_EQ(x.Info().transitions, 1u);
}

SND_TEST(MusicTransition, InterruptAllowedRestartsFromWhatIsAudible) {
  const std::string rules = Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"fadeOutBeats\": 4, \"fadeInBeats\": 4") + ", " +
                            Rule("*", "Boss", "\"timing\": \"nextBeat\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0");
  Session x(Setup{rules});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.SetAt(X + 10000, "Boss");  // during the 4-beat fades
  x.r.Step();
  SND_CHECK_EQ(x.Info().phase, 1u);
  x.r.RunTo(X + 2 * kBar);
  StartLog bo{};
  SND_REQUIRE(StartOf(x.r, "bo", x.s0, &bo));
  SND_CHECK_EQ(bo.clock, X + kBeat);
  // Everything else is cut at X + 1 beat (short fade): only Boss sounds afterwards.
  SND_CHECK_NEAR(x.r.At(X + kBeat + 2000), 0.4f, 1e-4f);
  SND_CHECK_EQ(x.Info().transitions, 2u);
}

SND_TEST(MusicTransition, AfterBridgeQueuesOneRequest) {
  const std::string rules =
      Rule("Explore", "Battle", "\"timing\": \"nextBar\", \"bridge\": \"Br\", \"interrupt\": \"afterBridge\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0") +
      ", " + Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0");
  Session x(Setup{rules});
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  const std::uint64_t X = x.s0 + kBar;
  x.SetAt(X + 10000, "Explore");  // during the bridge: queued
  x.SetAt(X + 20000, "Boss");     // last one wins
  x.r.Step();
  SND_CHECK_EQ(x.Info().phase, 2u);
  SND_CHECK_EQ(x.Info().queuedState, HashId("Boss"));
  x.r.RunTo(X + 3 * kBar);
  StartLog bo{};
  SND_REQUIRE(StartOf(x.r, "bo", x.s0, &bo));
  // The bridge runs to its end (X + 1 bar); then the queued request takes the next bar line.
  SND_CHECK_EQ(bo.clock, X + kBar);
  SND_CHECK_NEAR(x.r.At(X + kBar / 2), 0.9f, 1e-4f);
  SND_CHECK_EQ(x.Info().state, HashId("Boss"));
}

SND_TEST(MusicTransition, LateRuleNextGridMovesToTheFollowingBar) {
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBar\", \"late\": \"nextGrid\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.r.kind.primeFrames.store(30000);
  x.r.kind.primeAsset.store(HashId("bt"));
  x.SetAt(x.s0 + 80000, "Battle");  // 16000 left before the bar, 30000 needed
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + 2 * kBar);
  SND_CHECK_EQ(x.Info().deferrals, 1u);
  snd::music::MusicTransitionRecord log[4];
  const std::uint32_t n = x.r.music.TransitionLog(log, 4);
  SND_REQUIRE(n >= 1u);
  SND_CHECK_EQ(log[n - 1].clock, x.s0 + 2 * kBar);
  SND_CHECK_EQ(log[n - 1].lateRule, static_cast<std::uint32_t>(snd::ReservationLateRule::NextGrid));
  SND_CHECK_EQ(log[n - 1].deferrals, 1u);
}

SND_TEST(MusicTransition, LateRuleStartLateAndCancel) {
  {
    Session x(Setup{Rule("*", "*", "\"timing\": \"nextBar\", \"late\": \"startLate\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
    SND_REQUIRE(x.ok());
    x.r.kind.primeFrames.store(30000);
    x.r.kind.primeAsset.store(HashId("bt"));
    x.SetAt(x.s0 + 80000, "Battle");
    x.r.RunTo(x.s0 + 3 * kBar);
    StartLog st{};
    SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
    SND_CHECK_EQ(st.clock, x.s0 + kBar);  // kept on its bar ("遅れて開始": the audio may come late)
    SND_CHECK_EQ(x.Info().deferrals, 0u);
  }
  {
    Session x(Setup{Rule("*", "*", "\"timing\": \"nextBar\", \"late\": \"cancel\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
    SND_REQUIRE(x.ok());
    x.r.kind.primeFrames.store(30000);
    x.r.kind.primeAsset.store(HashId("bt"));
    x.SetAt(x.s0 + 80000, "Battle");
    x.r.RunTo(x.s0 + 3 * kBar);
    SND_CHECK_EQ(CountStarts(x.r, "bt"), 0u);
    SND_CHECK_EQ(x.Info().state, HashId("Explore"));
    SND_CHECK_EQ(x.Info().cancels, 1u);
  }
}

SND_TEST(MusicTransition, WhenOverridesTheRuleTiming) {
  // Rule: next bar. The call's When: next beat of the music transport.
  Session x(Setup{Rule("*", "*", "\"timing\": \"nextBar\", \"fadeOutBeats\": 0, \"fadeInBeats\": 0")});
  SND_REQUIRE(x.ok());
  x.r.RunTo(x.s0 + 30000);
  const snd_when w = Kind(OwnedTransport(x.p), SND_WHEN_NEXT_BEAT);
  x.r.sys->SetMusicState(x.p, HashId("Battle"), &w);
  x.r.RunTo(x.s0 + 2 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + 48000);
}

SND_TEST(MusicTransition, SegmentEndLetsTheTailRing) {
  // E has a tail of half a bar after its 2 bars; segmentEnd: Bt starts on E's end, E keeps ringing.
  Setup s{Rule("*", "*", "\"timing\": \"segmentEnd\", \"fadeOutBeats\": 2, \"fadeInBeats\": 0")};
  s.segE = "\"exitFrame\": 192000";
  Session x(s);
  SND_REQUIRE(x.ok());
  x.SetAt(x.s0 + 1000, "Battle");
  x.r.RunTo(x.s0 + 3 * kBar);
  StartLog st{};
  SND_REQUIRE(StartOf(x.r, "bt", x.s0, &st));
  SND_CHECK_EQ(st.clock, x.s0 + 2 * kBar);
  // E was not stopped: its material ends by itself at 2 bars (the asset is exactly 2 bars long).
  SND_CHECK_NEAR(x.r.At(x.s0 + 2 * kBar - 500), 0.2f, 1e-4f);
  SND_CHECK_NEAR(x.r.At(x.s0 + 2 * kBar + 500), 0.6f, 1e-4f);
}
