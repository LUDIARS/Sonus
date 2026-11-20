// Independent tests, stage 3 group 4: voice management (4.4, D-140), the fade-in of
// snd_play_fade (D-146), end notifications (2.1, 4.1) and clip markers. See indep_g4_fixture.hpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "indep_g4_fixture.hpp"

using namespace g4;

namespace {

// Events "E<i>" (level[i]) in limit group G (max `max`, steal mode `mode`), priority[i].
// The level is the clip volume over a DC 0.5 source, so that the audibility (a gain
// estimate, 4.4) sees it; level 0 is a silent source at 0 dB.
B GroupBank(std::uint32_t max, std::uint32_t mode, const std::vector<float>& level,
            const std::vector<std::uint32_t>& priority = {}) {
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), max, mode});
  for (std::size_t i = 0; i < level.size(); ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "E%zu", i);
    const TClip c = level[i] > 0.0f ? B::SynthClip(bk.Synth(500.0f), 20.0f * std::log10(level[i] / 0.5f))
                                    : B::SynthClip(bk.Synth(0.0f));
    TEvent& e = bk.Event(name, {c});
    e.group = 0;
    e.priority = i < priority.size() ? priority[i] : 128u;
  }
  return bk;
}
Id E(int i) {
  char name[32];
  std::snprintf(name, sizeof name, "E%d", i);
  return HashId(name);
}
float Level(Rig& r) { return r.Mean(r.Now() - 256, r.Now()) / kC; }

}  // namespace

// ---------------------------------------------------------------------------
// Steal modes (VLGR.stealMode)
// ---------------------------------------------------------------------------

// 0 oldest: the oldest playback ends STOLEN / LIMIT_GROUP, the new one plays.
SND_TEST(IndepG4Steal, OldestStealsTheOldest) {
  Rig r;
  B bk = GroupBank(2, 0, {0.1f, 0.2f, 0.3f});
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle c = r.sys->Play(E(2), NoEm(), nullptr);
  r.Steps(4);
  SND_CHECK_NEAR(Level(r), 0.5f, 1e-3f);
  SND_CHECK_EQ(r.CountEnds(a.value), 1u);  // the reason: Defect_StolenPlaybackEndsAsCompleted
  SND_CHECK_EQ(r.CountEnds(b.value) + r.CountEnds(c.value), 0u);
}

// DEFECT (4.1, 2.1, types.h SND_END_STOLEN): a playback whose voice is stolen by its limit
// group ends with SND_END_COMPLETED / detail 0 in every steal mode. Expected
// SND_END_STOLEN / SND_END_DETAIL_LIMIT_GROUP.
SND_TEST(IndepG4Steal, Defect_StolenPlaybackEndsAsCompleted) {
  for (std::uint32_t mode : {0u, 1u, 2u}) {
    Rig r;
    B bk = GroupBank(1, mode, {0.2f, 0.4f}, {100, 150});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(4);
    const End* e = r.EndOf(a.value);
    SND_REQUIRE(e != nullptr);
    snd::test::Note("mode %u: victim ends with reason %u detail %u", mode, e->reason, e->detail);
    SND_CHECK_EQ(e->reason, SND_END_STOLEN);
    SND_CHECK_EQ(e->detail, SND_END_DETAIL_LIMIT_GROUP);
  }
}

// 1 quietest: the smallest audibility is stolen (not the oldest); equal audibility -> the
// oldest; a silent voice (audibility 0) is the first victim.
SND_TEST(IndepG4Steal, QuietestStealsTheQuietest) {
  {
    Rig r;
    B bk = GroupBank(2, 1, {0.3f, 0.1f, 0.2f});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(2), NoEm(), nullptr);
    r.Steps(4);
    SND_CHECK_NEAR(Level(r), 0.5f, 1e-3f);
    SND_CHECK_EQ(r.CountEnds(b.value), 1u);
    SND_CHECK_EQ(r.CountEnds(a.value), 0u);
  }
  {
    Rig r;
    B bk = GroupBank(2, 1, {0.2f, 0.2f, 0.2f});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(2), NoEm(), nullptr);
    r.Steps(4);
    SND_CHECK_EQ(r.CountEnds(a.value), 1u);
    SND_CHECK_EQ(r.CountEnds(b.value), 0u);
  }
  {
    // A silent source at 0 dB: its audibility is not 0 (the estimate does not see the
    // signal); records which one goes.
    Rig r;
    B bk = GroupBank(2, 1, {0.3f, 0.0f, 0.2f});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(2), NoEm(), nullptr);
    r.Steps(4);
    snd::test::Note("silent source vs -4.4 dB clip: %s stolen", r.CountEnds(b.value) != 0u ? "silent" : "other");
    SND_CHECK_EQ(r.CountEnds(a.value) + r.CountEnds(b.value), 1u);
  }
}

// 1 quietest: a voice far beyond the distance curve (audibility ~0) goes first.
SND_TEST(IndepG4Steal, QuietestTakesAVoiceOutOfRange) {
  Rig r;
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), 2, 1});
  TCurve dist;
  dist.input = bf::kInputDistance;
  dist.target = 0;  // volume
  dist.compose = 0;
  dist.points = {{0.0f, 0.0f}, {10.0f, -96.0f}};
  bk.b.curves.push_back(dist);
  TEvent& far = bk.Event("Far", {B::SynthClip(bk.Synth(500.0f))});
  far.group = 0;
  far.flags = bf::kEvent3D;
  far.curveVolume = 0;
  far.maxDistance = 10.0f;
  bk.Event("Near1", {B::SynthClip(bk.Synth(100.0f), -12.0f)}).group = 0;
  bk.Event("Near2", {B::SynthClip(bk.Synth(100.0f), -12.0f)}).group = 0;
  r.Load("b", bk.Build("B"));
  snd_listener_desc l{};
  l.forward = snd_vec3{0, 0, 1};
  l.up = snd_vec3{0, 1, 0};
  l.enabled = 1;
  r.sys->SetListener(0, &l);
  const snd_emitter_handle em = r.sys->CreateEmitter();
  const snd_vec3 pos{0.0f, 0.0f, 50.0f};
  r.sys->SetTransform(em, &pos, nullptr, nullptr);
  const snd_playback_handle a = r.sys->Play(HashId("Near1"), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle f = r.sys->Play(HashId("Far"), em, nullptr);
  r.Steps(3);
  snd::test::Note("Near1 + Far level %g (Near1 alone 0.0251)", double(Level(r)));
  SND_CHECK_NEAR(Level(r), 0.1f * 0.2512f, 2e-3f);  // Far is (nearly) inaudible
  r.sys->Play(HashId("Near2"), NoEm(), nullptr);
  r.Steps(4);
  SND_CHECK_EQ(r.CountEnds(f.value), 1u);
  SND_CHECK_EQ(r.CountEnds(a.value), 0u);
}

// 1 quietest uses the audibility (volume, bus, distance), so a voice made quiet by its
// clip volume is the victim even though its source level is high.
SND_TEST(IndepG4Steal, QuietestCountsTheClipVolume) {
  Rig r;
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), 2, 1});
  bk.Event("Loud", {B::SynthClip(bk.Synth(300.0f))}).group = 0;              // 0.3
  bk.Event("Soft", {B::SynthClip(bk.Synth(900.0f), -20.0f)}).group = 0;      // 0.09
  bk.Event("New", {B::SynthClip(bk.Synth(200.0f))}).group = 0;               // 0.2
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(HashId("Loud"), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle b = r.sys->Play(HashId("Soft"), NoEm(), nullptr);
  r.Steps(2);
  r.sys->Play(HashId("New"), NoEm(), nullptr);
  r.Steps(4);
  SND_CHECK_EQ(r.CountEnds(b.value), 1u);
  SND_CHECK_EQ(r.CountEnds(a.value), 0u);
}

// 2 lowest priority: the lowest priority is stolen (not the oldest); equal -> the oldest.
SND_TEST(IndepG4Steal, LowestPriorityStealsTheLowest) {
  {
    Rig r;
    B bk = GroupBank(2, 2, {0.1f, 0.2f, 0.3f}, {100, 50, 150});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(2), NoEm(), nullptr);
    r.Steps(4);
    SND_CHECK_EQ(r.CountEnds(b.value), 1u);
    SND_CHECK_EQ(r.CountEnds(a.value), 0u);
    SND_CHECK_NEAR(Level(r), 0.4f, 1e-3f);
  }
  {
    Rig r;
    B bk = GroupBank(2, 2, {0.1f, 0.2f, 0.3f}, {100, 100, 100});
    r.Load("b", bk.Build("B"));
    const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
    r.Steps(2);
    const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Play(E(2), NoEm(), nullptr);
    r.Steps(4);
    SND_CHECK_EQ(r.CountEnds(a.value), 1u);
    SND_CHECK_EQ(r.CountEnds(b.value), 0u);
  }
}

// 2 lowest priority when the new request is itself the lowest: records the choice.
SND_TEST(IndepG4Steal, SpecHole_LowestPriorityNewcomerIsTheLowest) {
  Rig r;
  B bk = GroupBank(2, 2, {0.1f, 0.2f, 0.3f}, {100, 50, 10});
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle c = r.sys->Play(E(2), NoEm(), nullptr);
  r.Steps(4);
  const End* ec = r.EndOf(c.value);
  snd::test::Note("newcomer priority 10 vs 100 / 50: newcomer %s (reason %u); priority 50 %s",
                  ec != nullptr ? "ended" : "plays", ec != nullptr ? ec->reason : 0u,
                  r.CountEnds(b.value) != 0u ? "stolen" : "plays");
  SND_CHECK_EQ(r.CountEnds(a.value), 0u);
  SND_CHECK_EQ(r.CountEnds(b.value) + r.CountEnds(c.value), 1u);
}

// 3 never: the new request ends REJECTED / LIMIT_GROUP, the others play on.
SND_TEST(IndepG4Steal, NeverRejectsTheNewRequest) {
  Rig r;
  B bk = GroupBank(2, 3, {0.1f, 0.2f, 0.3f});
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle b = r.sys->Play(E(1), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle c = r.sys->Play(E(2), NoEm(), nullptr);
  r.Steps(4);
  SND_CHECK(r.Ended(c.value, SND_END_REJECTED, SND_END_DETAIL_LIMIT_GROUP));
  SND_CHECK_EQ(r.CountEnds(c.value), 1u);
  SND_CHECK_EQ(r.CountEnds(a.value) + r.CountEnds(b.value), 0u);
  SND_CHECK_NEAR(Level(r), 0.3f, 1e-3f);
}

// Many times the group limit in one block, every mode: at most `max` sound, every
// playback ends exactly once (after everything is stopped), no click from the steals.
SND_TEST(IndepG4Steal, ManyRequestsInOneBlockEveryMode) {
  for (std::uint32_t mode = 0; mode < 4; ++mode) {
    Rig r;
    std::vector<float> lv;
    std::vector<std::uint32_t> pr;
    for (int i = 0; i < 12; ++i) {
      lv.push_back(0.01f * float(i + 1));
      pr.push_back(100u + std::uint32_t(i % 3));
    }
    B bk = GroupBank(3, mode, lv, pr);
    r.Load("b", bk.Build("B"));
    std::vector<snd_playback_handle> hs;
    for (int round = 0; round < 3; ++round) {
      for (int i = 0; i < 12; ++i) hs.push_back(r.sys->Play(E(i), NoEm(), nullptr));
      r.Steps(3);
      std::uint32_t alive = 0;
      for (const snd_playback_handle& h : hs) alive += r.CountEnds(h.value) == 0u ? 1u : 0u;
      SND_CHECK(alive <= 3u);
    }
    for (const snd_playback_handle& h : hs) r.sys->Stop(h, 0.0f, nullptr);
    r.Steps(4);
    std::uint32_t notOnce = 0;
    for (const snd_playback_handle& h : hs) notOnce += r.CountEnds(h.value) != 1u ? 1u : 0u;
    snd::test::Note("mode %u: %zu playbacks, %u without exactly one end", mode, hs.size(), notOnce);
    SND_CHECK_EQ(notOnce, 0u);
    SND_CHECK_EQ(r.ends.size(), hs.size());
    SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
  }
}

// A stolen voice fades over the short fade (D-15) even when the fade reserve is exhausted
// by several steals in one block: no step in the output.
SND_TEST(IndepG4Steal, StealsBeyondTheFadeReserveDoNotClick) {
  snd_config cfg = Config(4);
  cfg.fade_reserve_voices = 1;
  Rig r(cfg);
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), 4, 0});
  bk.Event("Loud", {B::SynthClip(bk.Synth(200.0f))}).group = 0;   // 0.2 each
  bk.Event("Mute", {B::SynthClip(bk.Synth(0.0f))}).group = 0;     // silent newcomers
  r.Load("b", bk.Build("B"));
  for (int i = 0; i < 4; ++i) r.sys->Play(HashId("Loud"), NoEm(), nullptr);
  r.Steps(3);
  const std::size_t from = r.Now() - 1;
  for (int i = 0; i < 4; ++i) r.sys->Play(HashId("Mute"), NoEm(), nullptr);  // 4 steals, 1 reserve
  r.Steps(3);
  float d1 = 0;
  for (std::size_t f = from; f + 1 < r.Now(); ++f) d1 = std::max(d1, std::fabs(r.At(f + 1) - r.At(f)));
  snd::test::Note("max |step| %g (0.8 x kC = %g would be a cut; fade slope 0.8 kC / 240 = %g)", double(d1),
                  double(0.8f * kC), double(0.8f * kC / 240.0f));
  SND_CHECK(d1 < 0.8f * kC / 240.0f * 3.0f);
  SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
}

// A victim that is already fading out (long stop fade) is stolen: it reaches 0 within the
// short fade and ends exactly once.
SND_TEST(IndepG4Steal, StealingAFadingVoice) {
  Rig r;
  B bk = GroupBank(1, 0, {0.4f, 0.0f});
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(E(0), NoEm(), nullptr);
  r.Steps(2);
  r.sys->Stop(a, 1.0f, nullptr);
  r.Steps(2);
  const std::size_t t = r.Now();
  r.sys->Play(E(1), NoEm(), nullptr);
  r.Steps(4);
  SND_CHECK_NEAR(r.MaxAbs(t + 241, r.Now()), 0.0f, 1e-6f);
  SND_CHECK_EQ(r.CountEnds(a.value), 1u);
  const End* e = r.EndOf(a.value);
  snd::test::Note("fading victim ends with reason %u", e != nullptr ? e->reason : 0u);
}

// 4x the real voice limit (no group): bounded, the loudest are real, the rest virtual;
// nothing is refused while the virtual pool lasts.
SND_TEST(IndepG4Steal, FourTimesTheRealLimitIsBoundedAndNotRefused) {
  Rig r(Config(8));
  B bk;
  for (int i = 0; i < 32; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "V%d", i);
    bk.Event(name, {B::SynthClip(bk.Synth(100.0f), -60.0f + float(i))});  // 0.1 x 10^((i - 60) / 20)
  }
  r.Load("b", bk.Build("B"));
  std::vector<snd_playback_handle> hs;
  for (int i = 0; i < 32; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "V%d", i);
    hs.push_back(r.sys->Play(HashId(name), NoEm(), nullptr));
  }
  r.Steps(6);
  // The 8 loudest (clip volume, the audibility estimate): i = 24..31.
  double want = 0;
  for (int i = 24; i < 32; ++i) want += 0.1 * std::pow(10.0, (i - 60) / 20.0);
  SND_CHECK_NEAR(Level(r), float(want), float(want) * 0.01f);
  SND_CHECK_EQ(r.ends.size(), 0u);
  for (const snd_playback_handle& h : hs) r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(3);
  for (const snd_playback_handle& h : hs) SND_CHECK_EQ(r.CountEnds(h.value), 1u);
}

// ---------------------------------------------------------------------------
// Virtual voices: the way back (D-140)
// ---------------------------------------------------------------------------
namespace {
// "L" (priority 100): a 2 s ramp asset (value 0.25 + 0.5 * frame / 96000), virtualReturn `mode`,
// ASST.flags `flags`. "H" (priority 200, DC 0.1) takes the only real voice from block 4 to 14.
struct VR {
  float valueAfter = 0;    // L's value (relative) one block after H stopped (0 = silent)
  double posAfter = 0;     // material frame that value is
  std::size_t clockAfter;  // frames since L started at that point
  std::uint32_t ends = 0;
  snd_end_reason reason = 0;
};
VR RunVirtualReturn(std::uint32_t mode, std::uint32_t flags, bool pauseWhileVirtual = false) {
  Rig r(Config(1));
  B bk;
  std::vector<float> ramp(96000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = 0.25f + 0.5f * float(i) / 96000.0f;
  TEvent& l = bk.Event("L", {B::AssetClip(bk.Asset("ramp", ramp, 0, 0, flags))});
  l.priority = 100;
  l.virtualReturn = mode;
  bk.Event("H", {B::SynthClip(bk.Synth(100.0f))}).priority = 200;
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("L"), NoEm(), nullptr);
  r.Steps(4);
  if (pauseWhileVirtual) r.sys->Pause(h, 0.0f);
  const snd_playback_handle hi = r.sys->Play(HashId("H"), NoEm(), nullptr);
  r.Steps(10);
  r.sys->Stop(hi, 0.0f, nullptr);
  if (pauseWhileVirtual) r.sys->ResumePlayback(h, 0.0f);
  r.Steps(3);
  VR v;
  v.clockAfter = r.Now() - 1 - t0;
  v.valueAfter = r.At(r.Now() - 1) / kC;
  v.posAfter = (double(v.valueAfter) - 0.25) / 0.5 * 96000.0;
  r.Steps(2);
  v.ends = r.CountEnds(h.value);
  const End* e = r.EndOf(h.value);
  v.reason = e != nullptr ? e->reason : 0;
  return v;
}
}  // namespace

SND_TEST(IndepG4Virtual, ContinueRestartAndNeverFromTheEvent) {
  const VR c = RunVirtualReturn(0, 0);
  snd::test::Note("continue: material frame %.0f at clock %zu", c.posAfter, c.clockAfter);
  SND_CHECK(std::fabs(c.posAfter - double(c.clockAfter)) < 64.0);
  SND_CHECK_EQ(c.ends, 0u);
  const VR s = RunVirtualReturn(1, 0);
  snd::test::Note("restart: material frame %.0f at clock %zu (returned at 3584)", s.posAfter, s.clockAfter);
  SND_CHECK(s.posAfter < 768.0 + 64.0 && s.valueAfter > 0.2f);
  SND_CHECK_EQ(s.ends, 0u);
  const VR n = RunVirtualReturn(2, 0);
  snd::test::Note("never: value %g, ends %u reason %u", double(n.valueAfter), n.ends, n.reason);
  SND_CHECK_NEAR(n.valueAfter, 0.0f, 1e-6f);
  SND_CHECK_EQ(n.ends, 1u);
}

// The asset's mark wins over EVNT.virtualReturn (D-140).
SND_TEST(IndepG4Virtual, AssetFlagsWinOverTheEvent) {
  const VR s = RunVirtualReturn(0, bf::kAssetVirtualRestart);
  snd::test::Note("event continue + asset restart: material frame %.0f at clock %zu", s.posAfter, s.clockAfter);
  SND_CHECK(s.posAfter < 768.0 + 64.0 && s.valueAfter > 0.2f);
  const VR n = RunVirtualReturn(1, bf::kAssetVirtualKill);
  snd::test::Note("event restart + asset kill: value %g ends %u", double(n.valueAfter), n.ends);
  SND_CHECK_NEAR(n.valueAfter, 0.0f, 1e-6f);
  SND_CHECK_EQ(n.ends, 1u);
}

// "Never" for a playback paused while it is virtual: it never comes back, one end.
SND_TEST(IndepG4Virtual, NeverWhilePaused) {
  const VR n = RunVirtualReturn(2, 0, true);
  snd::test::Note("never + paused: value %g, ends %u reason %u", double(n.valueAfter), n.ends, n.reason);
  SND_CHECK_NEAR(n.valueAfter, 0.0f, 1e-6f);
  SND_CHECK(n.ends <= 1u);
}

// ---------------------------------------------------------------------------
// snd_play_fade (D-146)
// ---------------------------------------------------------------------------
namespace {
B FadeBank() {
  B bk;
  bk.Event("D", {B::SynthClip(bk.Synth(500.0f))});  // DC 0.5 endless (output rate, no resampler)
  return bk;
}
}  // namespace

// Fade 0 is the same as Play, bit for bit.
SND_TEST(IndepG4Fade, ZeroFadeIsPlayBitForBit) {
  auto run = [](bool fade) {
    Rig r;
    r.Load("b", FadeBank().Build("B"));
    r.all.clear();
    for (int i = 0; i < 4; ++i) {
      if (fade) {
        r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.0f);
      } else {
        r.sys->Play(HashId("D"), NoEm(), nullptr);
      }
      r.Steps(3);
    }
    return r.all;
  };
  const std::vector<float> a = run(false), b = run(true);
  SND_REQUIRE_EQ(a.size(), b.size());
  SND_CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Coarse: from 0, rising, at the level at most one block after the fade's end.
SND_TEST(IndepG4Fade, RisesFromZeroToTheLevel) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.05f);  // 2400
  r.Steps(14);
  SND_CHECK_NEAR(r.At(t0), 0.0f, 1e-6f);
  int falls = 0;
  for (std::size_t f = t0; f + 1 < r.Now(); ++f) falls += r.At(f + 1) < r.At(f) - 1e-7f ? 1 : 0;
  SND_CHECK_EQ(falls, 0);
  SND_CHECK_NEAR(r.At(t0 + 2400 + 256) / kC, 0.5f, 1e-5f);
  SND_CHECK_NEAR(r.At(t0 + 1200) / kC, 0.25f, 0.03f);
}

// DEFECT (D-146 "開始のフェードイン（線形）", fade_in_seconds): the gain is interpolated per
// block from the block's first frame: a 1 ms fade (48 frames) ramps over the whole block
// (256 frames), a 10 ms fade (480) reaches the level at frame 512, and a scheduled start at
// frame 1000 (inside a block) starts at 48 % of the level (a step) because the ramp is
// counted from the block start (768). Expected: gain = frames since the voice's start /
// fade frames, sample by sample, 0 at the start frame.
SND_TEST(IndepG4Fade, Defect_FadeInIsNotLinearSampleBySample) {
  {
    Rig r;
    r.Load("b", FadeBank().Build("B"));
    const std::size_t t0 = r.Now();
    const snd_when w = AtClock(t0 + 1000);
    r.sys->PlayFade(HashId("D"), NoEm(), &w, 0.01f);
    r.Steps(8);
    snd::test::Note("scheduled at 1000, 10 ms: frame 1000 %g, 1240 %g (expected 0, 0.25)", double(r.At(t0 + 1000) / kC),
                    double(r.At(t0 + 1240) / kC));
    SND_CHECK_NEAR(r.At(t0 + 1000) / kC, 0.0f, 0.5f / 480.0f + 1e-5f);
    SND_CHECK_NEAR(r.At(t0 + 1240) / kC, 0.25f, 0.5f / 480.0f + 1e-5f);
  }
  for (float sec : {0.010f, 0.001f, 0.0256f}) {
    Rig r;
    r.Load("b", FadeBank().Build("B"));
    const std::size_t t0 = r.Now();
    const snd_playback_handle h = r.sys->PlayFade(HashId("D"), NoEm(), nullptr, sec);
    SND_CHECK(h.value != 0u);
    r.Steps(8);
    const double n = std::round(double(sec) * 1000.0) * 48.0;
    double maxErr = 0;
    for (std::size_t i = 0; i < std::size_t(n) + 100; ++i) {
      const double want = 0.5 * std::min(1.0, double(i) / n);
      maxErr = std::max(maxErr, std::fabs(double(r.At(t0 + i)) / kC - want));
    }
    snd::test::Note("fade %g s (%g frames): max |error| vs the line %g (one frame step = %g)", double(sec), n, maxErr,
                    0.5 / n);
    SND_CHECK(maxErr <= 0.5 / n * 1.01 + 1e-5);
  }
}

// Rounded to milliseconds: 0.4 ms is 0 (no fade).
SND_TEST(IndepG4Fade, BelowHalfAMillisecondIsNoFade) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.0004f);
  r.Steps(2);
  SND_CHECK_NEAR(r.At(t0) / kC, 0.5f, 1e-6f);
}

// More than 65.535 s is limited to 65.535 s ("at most"), not refused.
SND_TEST(IndepG4Fade, LongerThanTheMaximumIsLimited) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 100.0f);
  SND_REQUIRE(h.value != 0u);
  r.Steps(20);
  const double want = 0.5 * 4800.0 / (65535.0 * 48.0);
  snd::test::Note("at 4800 frames %g (65.535 s line %g)", double(r.At(t0 + 4800) / kC), want);
  SND_CHECK_NEAR(r.At(t0 + 4800) / kC, want, want * 0.01 + 1e-6);
  SND_CHECK_EQ(r.CountEnds(h.value), 0u);
}

// NaN, infinite or negative: invalid handle, nothing plays, no end.
SND_TEST(IndepG4Fade, InvalidFadeGivesAnInvalidHandle) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(), -0.5f, -1e-6f};
  for (float f : bad) SND_CHECK_EQ(r.sys->PlayFade(HashId("D"), NoEm(), nullptr, f).value, 0u);
  r.Steps(4);
  SND_CHECK_NEAR(r.MaxAbs(t0, r.Now()), 0.0f, 1e-7f);
  SND_CHECK_EQ(r.ends.size(), 0u);
}

// Stopping during the fade-in fades out from the level reached (never up).
SND_TEST(IndepG4Fade, StopDuringTheFadeFadesFromTheLevelReached) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.05f);  // 2400 frames
  r.Steps(2);                                                                         // level at 512: 0.107
  const float reached = r.At(t0 + 511) / kC;
  r.sys->Stop(h, 0.02f, nullptr);
  r.Steps(10);
  float maxAfter = 0;
  int rises = 0;
  for (std::size_t f = t0 + 512; f < r.Now(); ++f) {
    maxAfter = std::max(maxAfter, r.At(f) / kC);
    rises += r.At(f + 1 < r.Now() ? f + 1 : f) > r.At(f) + 1e-6f ? 1 : 0;
  }
  snd::test::Note("reached %g, max after the stop %g", double(reached), double(maxAfter));
  SND_CHECK(maxAfter <= reached + 0.5f / 2400.0f + 1e-6f);
  SND_CHECK_EQ(rises, 0);
  SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_STOPPED));
}

// A scheduled start: nothing before the scheduled frame, the level a block after the fade.
SND_TEST(IndepG4Fade, ScheduledStartFadesIn) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  const snd_when w = AtClock(t0 + 1000);
  r.sys->PlayFade(HashId("D"), NoEm(), &w, 0.01f);
  r.Steps(8);
  SND_CHECK_NEAR(r.MaxAbs(t0, t0 + 1000), 0.0f, 1e-7f);
  SND_CHECK(r.At(t0 + 1001) / kC < 0.5f);
  SND_CHECK_NEAR(r.At(t0 + 1480 + 256) / kC, 0.5f, 1e-5f);
}

// A pause during the fade-in: records whether the fade counts playback time or clock time.
SND_TEST(IndepG4Fade, SpecHole_PauseDuringTheFadeIn) {
  Rig r;
  r.Load("b", FadeBank().Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.05f);  // 2400 frames
  r.Steps(2);
  r.sys->Pause(h, 0.0f);
  r.Steps(10);
  r.sys->ResumePlayback(h, 0.0f);
  r.Steps(2);
  const float v = r.At(r.Now() - 1) / kC;
  (void)t0;
  snd::test::Note("one block after the resume: %g (playback time ~ %g, clock time 0.5)", double(v),
                  0.5 * (512.0 + 512.0) / 2400.0);
  SND_CHECK(v > 0.0f && v <= 0.5f + 1e-5f);
}

// Virtual during the fade: the fade keeps going while virtual (clock of the playback).
SND_TEST(IndepG4Fade, SpecHole_VirtualDuringTheFadeIn) {
  Rig r(Config(1));
  B bk = FadeBank();
  bk.b.events[0].priority = 100;
  bk.Event("H", {B::SynthClip(bk.Synth(100.0f))}).priority = 200;
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->PlayFade(HashId("D"), NoEm(), nullptr, 0.1f);  // 4800
  r.Steps(2);
  const snd_playback_handle hi = r.sys->Play(HashId("H"), NoEm(), nullptr);
  r.Steps(8);
  r.sys->Stop(hi, 0.0f, nullptr);
  r.Steps(2);
  snd::test::Note("after the virtual phase (clock 3072 of 4800): %g (clock-time fade 0.32)",
                  double(r.At(r.Now() - 1) / kC));
  SND_CHECK_EQ(r.CountEnds(h.value), 0u);
}

// ---------------------------------------------------------------------------
// End notifications: exactly once on every path (2.1)
// ---------------------------------------------------------------------------

// Command overflow: every play still yields a handle that ends exactly once.
SND_TEST(IndepG4Ends, CommandOverflowEndsExactlyOnce) {
  snd_config cfg = Config();
  cfg.max_playbacks = 4096;
  Rig r(cfg);
  B bk;
  bk.Event("D", {B::SynthClip(bk.Synth(1.0f))});
  r.Load("b", bk.Build("B"));
  std::vector<snd_playback_handle> hs;
  for (int i = 0; i < 2000; ++i) {
    const snd_playback_handle h = r.sys->Play(HashId("D"), NoEm(), nullptr);
    if (h.value != 0u) hs.push_back(h);
  }
  r.Steps(3);
  for (const snd_playback_handle& h : hs) r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(3);
  std::uint32_t notOnce = 0, overflow = 0;
  for (const snd_playback_handle& h : hs) {
    notOnce += r.CountEnds(h.value) != 1u ? 1u : 0u;
    overflow += r.Ended(h.value, SND_END_REJECTED, SND_END_DETAIL_COMMAND_OVERFLOW) ? 1u : 0u;
  }
  snd::test::Note("%zu handles, %u overflowed, %u without exactly one end", hs.size(), overflow, notOnce);
  SND_CHECK(overflow > 0u);
  SND_CHECK_EQ(notOnce, 0u);
}

// Bank unload: playing, stolen-in-progress, nested and action-started playbacks: one end each.
SND_TEST(IndepG4Ends, BankUnloadEndsEveryPlaybackOnce) {
  Rig r;
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), 1, 0});
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(0.0f))});
  TAction a;
  a.timeMs = 0;
  a.action = 0;
  a.target = HashId("X");
  p.tracks[0].actions = {a};
  bk.Event("X", {B::SynthClip(bk.Synth(100.0f))});
  bk.Event("N", {TClip{1, 3}});  // row 2 nests row 3
  bk.Event("Leaf", {B::SynthClip(bk.Synth(100.0f))});
  bk.Event("Grp", {B::SynthClip(bk.Synth(100.0f))}).group = 0;
  const snd_bank_handle bank = r.Load("b", bk.Build("B"));
  std::vector<snd_playback_handle> hs;
  hs.push_back(r.sys->Play(HashId("P"), NoEm(), nullptr));
  hs.push_back(r.sys->Play(HashId("N"), NoEm(), nullptr));
  for (int i = 0; i < 3; ++i) hs.push_back(r.sys->Play(HashId("Grp"), NoEm(), nullptr));
  r.Steps(3);
  hs.push_back(r.sys->Play(HashId("Grp"), NoEm(), nullptr));  // steals in the unload block
  r.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD);
  r.Steps(10);
  std::uint32_t notOnce = 0;
  for (const snd_playback_handle& h : hs) notOnce += r.CountEnds(h.value) != 1u ? 1u : 0u;
  SND_CHECK_EQ(notOnce, 0u);
  SND_CHECK(r.Ended(hs[0].value, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK(r.Ended(hs[1].value, SND_END_DISCARDED, SND_END_DETAIL_BANK_UNLOADED));
  SND_CHECK_EQ(r.ends.size(), hs.size());  // nothing for handle-less action children
  SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
}

// ---------------------------------------------------------------------------
// Clip markers (core's entry point CoreDesc::clipMarkers)
// ---------------------------------------------------------------------------

// Each marker inside the material is reported once (within the resampler look-ahead of its
// frame); a marker past the end never. The exact clock: Defect_MarkerClockRunsAheadOfTheFrame.
SND_TEST(IndepG4Marker, MarkersOnceEach) {
  MarkerTable mt;
  mt.asset = HashId("m");
  mt.frames = {0, 100, 256, 4799, 5000};
  mt.ids = {HashId("m0"), HashId("m1"), HashId("m2"), HashId("m3"), HashId("m4")};
  Rig r(Config(), {}, nullptr, &mt);
  B bk;
  bk.Event("M", {B::AssetClip(bk.Asset("m", Constant(4800, 0.5f)))});
  r.Load("b", bk.Build("B"));
  r.Steps(1);
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("M"), NoEm(), nullptr);
  r.Steps(24);
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t n = 0;
    for (const Marker& m : r.markers) {
      if (m.id == mt.ids[i]) {
        ++n;
        SND_CHECK_EQ(m.handle, h.value);
        SND_CHECK(m.clock + 16 >= t0 + mt.frames[i] && m.clock <= t0 + mt.frames[i]);
      }
    }
    SND_CHECK_EQ(n, 1u);
  }
  for (const Marker& m : r.markers) SND_CHECK(m.id != mt.ids[4]);
}

// DEFECT (snd_timing.clock_sample "when the moment happens on the common clock", sonus.h
// on_marker): the reported clock runs ahead of the marker's frame by up to the resampler's
// look-ahead: frame 256 -> clock + 255, frame 4799 -> clock + 4783, in a second loop -16,
// at pitch 2 -8. Expected: start clock + frame / pitch (pitch 1: + frame).
SND_TEST(IndepG4Marker, Defect_MarkerClockRunsAheadOfTheFrame) {
  MarkerTable mt;
  mt.asset = HashId("m");
  mt.frames = {0, 100, 256, 4799, 5000};
  mt.ids = {HashId("m0"), HashId("m1"), HashId("m2"), HashId("m3"), HashId("m4")};
  Rig r(Config(), {}, nullptr, &mt);
  B bk;
  bk.Event("M", {B::AssetClip(bk.Asset("m", Constant(4800, 0.5f)))});
  r.Load("b", bk.Build("B"));
  r.Steps(1);
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("M"), NoEm(), nullptr);
  r.Steps(24);
  for (const Marker& m : r.markers) {
    snd::test::Note("marker %08x at clock %llu (rel %lld)", m.id, static_cast<unsigned long long>(m.clock),
                    static_cast<long long>(m.clock) - static_cast<long long>(t0));
  }
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t n = 0;
    for (const Marker& m : r.markers) {
      if (m.id == mt.ids[i]) {
        ++n;
        SND_CHECK_EQ(m.handle, h.value);
        SND_CHECK_EQ(m.clock, t0 + mt.frames[i]);
      }
    }
    SND_CHECK_EQ(n, 1u);
  }
  for (const Marker& m : r.markers) SND_CHECK(m.id != mt.ids[4]);
}

// With 2 loops the markers come once per loop (D-142 loops counted by core); with pitch 2
// at half the frames. Exact clock (part of Defect_MarkerClockRunsAheadOfTheFrame).
SND_TEST(IndepG4Marker, Defect_MarkersWithLoopsAndPitchAtTheirClock) {
  MarkerTable mt;
  mt.asset = HashId("m");
  mt.frames = {1000};
  mt.ids = {HashId("mk")};
  {
    Rig r(Config(), {}, nullptr, &mt);
    B bk;
    TClip c = B::AssetClip(bk.Asset("m", Constant(4800, 0.5f)));
    c.loopCount = 2;
    bk.Event("M", {c});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("M"), NoEm(), nullptr);
    r.Steps(44);
    std::vector<std::uint64_t> at;
    for (const Marker& m : r.markers) at.push_back(m.clock - t0);
    snd::test::Note("2 loops: %zu markers (%llu, %llu)", at.size(), at.empty() ? 0ull : (unsigned long long)at[0],
                    at.size() < 2 ? 0ull : (unsigned long long)at[1]);
    SND_CHECK_EQ(at.size(), 2u);
    if (at.size() == 2u) {
      SND_CHECK_EQ(at[0], 1000u);
      SND_CHECK_EQ(at[1], 5800u);
    }
  }
  {
    Rig r(Config(), {}, nullptr, &mt);
    B bk;
    TClip c = B::AssetClip(bk.Asset("m", Constant(4800, 0.5f)));
    c.pitchCents = 1200.0f;
    bk.Event("M", {c});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("M"), NoEm(), nullptr);
    r.Steps(12);
    SND_REQUIRE_EQ(r.markers.size(), 1u);
    snd::test::Note("pitch 2: marker at %llu (expected 500)", (unsigned long long)(r.markers[0].clock - t0));
    SND_CHECK(r.markers[0].clock - t0 >= 499 && r.markers[0].clock - t0 <= 501);
  }
}

// ---------------------------------------------------------------------------
// Added after reading the implementation (step 3)
// ---------------------------------------------------------------------------

// "Never" for a paused voice: records when it ends (while paused, or only after the resume).
// The implementation's comment says paused voices are not ended.
SND_TEST(IndepG4Virtual, SpecHole_NeverForAVoicePausedWhileVirtual) {
  Rig r(Config(1));
  B bk;
  TEvent& l = bk.Event("L", {B::SynthClip(bk.Synth(400.0f))});
  l.priority = 100;
  l.virtualReturn = 2;
  bk.Event("H", {B::SynthClip(bk.Synth(100.0f))}).priority = 200;
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("L"), NoEm(), nullptr);
  r.Steps(3);
  r.sys->Pause(h, 0.0f);
  r.Steps(2);
  const snd_playback_handle hi = r.sys->Play(HashId("H"), NoEm(), nullptr);
  r.Steps(6);
  const std::uint32_t endedWhilePaused = r.CountEnds(h.value);
  r.sys->Stop(hi, 0.0f, nullptr);
  r.Steps(3);
  const std::uint32_t endedBeforeResume = r.CountEnds(h.value);
  r.sys->ResumePlayback(h, 0.0f);
  r.Steps(4);
  snd::test::Note("ended while paused and H plays: %u; before the resume: %u; after: %u; level after %g",
                  endedWhilePaused, endedBeforeResume, r.CountEnds(h.value), double(Level(r)));
  SND_CHECK(r.CountEnds(h.value) <= 1u);
}

// Meters over the limit: records which bus gets the only meter when the later row is active
// first (D-147 says "数に上限" only).
SND_TEST(IndepG4Virtual, SpecHole_MeterSlotGoesToTheFirstActiveBus) {
  snd::core::CoreLimits lim;
  lim.maxMeteredBuses = 1;
  Rig r(Config(), lim);
  B bk;
  bk.b.buses.clear();
  bk.b.buses.push_back(TBus{HashId("M1"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("M2"), 2u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  bk.Event("On2", {B::SynthClip(bk.Synth(300.0f))}, 0, 1);
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("On2"), NoEm(), nullptr);
  r.Steps(20);
  snd_meter m1{}, m2{};
  r.sys->Update();
  r.sys->GetMeter(HashId("M1"), &m1);
  r.sys->GetMeter(HashId("M2"), &m2);
  snd::test::Note("limit 1: M1 (row 0, silent) loudness %g, M2 (row 1, active) loudness %g", double(m1.loudness_short),
                  double(m2.loudness_short));
  SND_CHECK(std::isnan(m1.loudness_short) || std::isnan(m2.loudness_short));
}

// Markers of a voice that is virtual when it crosses them: records whether they are reported
// (the playback is Playing while virtual, 4.1).
SND_TEST(IndepG4Marker, SpecHole_MarkersWhileVirtual) {
  MarkerTable mt;
  mt.asset = HashId("m");
  mt.frames = {2000};
  mt.ids = {HashId("mk")};
  Rig r(Config(1), {}, nullptr, &mt);
  B bk;
  bk.Event("M", {B::AssetClip(bk.Asset("m", Constant(9600, 0.5f)))}).priority = 100;
  bk.Event("H", {B::SynthClip(bk.Synth(100.0f))}).priority = 200;
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("M"), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle hi = r.sys->Play(HashId("H"), NoEm(), nullptr);
  r.Steps(16);  // the marker at 2000 passes while M is virtual
  r.sys->Stop(hi, 0.0f, nullptr);
  r.Steps(30);
  snd::test::Note("marker crossed while virtual: %zu reported", r.markers.size());
  SND_CHECK(r.markers.size() <= 1u);
}
