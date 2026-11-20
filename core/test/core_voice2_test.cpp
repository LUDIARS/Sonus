// core, stage 3 group 4: voice management (4.4) - the four steal modes of a limit group,
// the limit hierarchy, and the virtual voice return modes (D-140).
#include <cmath>
#include <set>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
constexpr double kRampScale = 131072.0;
float Mean(const Harness& h, std::size_t from, std::size_t to, std::uint32_t c) {
  double s = 0;
  for (std::size_t f = from; f < to; ++f) s += h.At(f, c);
  return static_cast<float>(s / double(to - from));
}
// Mono data whose frame i reads (i + 1) / 131072: the output tells the source position.
std::vector<float> Ramp(std::size_t frames) {
  std::vector<float> d(frames);
  for (std::size_t i = 0; i < frames; ++i) d[i] = static_cast<float>(double(i + 1) / kRampScale);
  return d;
}
long FrameAt(const Harness& h, std::size_t f) { return std::lround(double(h.At(f, 0)) / kCenter * kRampScale) - 1; }
snd_playback_handle Play(Harness& h, const char* e) { return h.sys->Play(HashId(e), snd_emitter_handle{0}, nullptr); }

// A limit group of 2 with the given steal mode, and events A, B, C, D in it.
struct GroupBank {
  TestBank b = BasicBank();
  GroupBank(std::uint32_t stealMode, const float (&db)[4], const std::uint32_t (&prio)[4]) {
    b.groups.push_back(TGroup{HashId("G"), 2, stealMode});
    b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.1f), 0, 0});
    const char* names[4] = {"A", "B", "C", "D"};
    for (int i = 0; i < 4; ++i) {
      TEvent e = SimpleEvent(names[i], 0, 0, db[i]);
      e.group = 0;
      e.priority = prio[i];
      b.events.push_back(e);
    }
  }
};
}  // namespace

// Quietest (VLGR.stealMode 1): the voice with the lowest audibility of the last block goes,
// whatever its age (the oldest would be A).
SND_TEST(CoreVoice2, StealQuietestTakesTheLowestAudibility) {
  Harness h;
  GroupBank g(1u, {0.0f, -20.0f, 0.0f, 0.0f}, {128, 128, 128, 128});
  h.Load("b", g.b.Build(HashId("B")));
  const snd_playback_handle a = Play(h, "A");
  h.Steps(2);
  const snd_playback_handle bq = Play(h, "B");
  h.Steps(2);
  const snd_playback_handle c = Play(h, "C");
  h.Steps(4);
  SND_CHECK(h.ends.Has(bq, SND_END_COMPLETED));  // its only voice was stolen
  SND_CHECK_EQ(h.ends.CountOf(a), 0u);
  SND_CHECK_EQ(h.ends.CountOf(c), 0u);
  h.Clear();
  h.Steps(2);
  SND_CHECK_NEAR(Mean(h, 256, 512, 0), kCenter * 0.2f, 1e-4f);  // A + C
}

// Quietest with equal audibility: the oldest goes.
SND_TEST(CoreVoice2, StealQuietestTieTakesTheOldest) {
  Harness h;
  GroupBank g(1u, {-6.0f, -6.0f, -6.0f, 0.0f}, {128, 128, 128, 128});
  h.Load("b", g.b.Build(HashId("B")));
  const snd_playback_handle a = Play(h, "A");
  h.Steps(2);
  const snd_playback_handle bq = Play(h, "B");
  h.Steps(2);
  Play(h, "C");
  h.Steps(4);
  SND_CHECK(h.ends.Has(a, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.CountOf(bq), 0u);
}

// Quietest: a voice created in the same block has no audibility yet and is not taken.
SND_TEST(CoreVoice2, StealQuietestSparesAVoiceOfTheSameBlock) {
  Harness h;
  GroupBank g(1u, {0.0f, -30.0f, -10.0f, 0.0f}, {128, 128, 128, 128});
  h.Load("b", g.b.Build(HashId("B")));
  const snd_playback_handle a = Play(h, "A");
  h.Steps(2);
  const snd_playback_handle bq = Play(h, "B");  // B and C in one block: C steals, B is new
  const snd_playback_handle c = Play(h, "C");
  h.Steps(4);
  SND_CHECK(h.ends.Has(a, SND_END_COMPLETED));  // the only voice with an audibility
  SND_CHECK_EQ(h.ends.CountOf(bq), 0u);
  SND_CHECK_EQ(h.ends.CountOf(c), 0u);
}

// Lowest priority (stealMode 2): the lowest priority goes (not the oldest); a new voice
// with a lower priority than every member is refused; equal priority may steal.
SND_TEST(CoreVoice2, StealLowestPriority) {
  Harness h;
  GroupBank g(2u, {0.0f, 0.0f, 0.0f, 0.0f}, {100, 50, 80, 40});
  h.Load("b", g.b.Build(HashId("B")));
  const snd_playback_handle a = Play(h, "A");
  h.Steps(2);
  const snd_playback_handle bq = Play(h, "B");
  h.Steps(2);
  const snd_playback_handle c = Play(h, "C");  // 80 > 50: B goes
  h.Steps(4);
  SND_CHECK(h.ends.Has(bq, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.CountOf(a), 0u);
  SND_CHECK_EQ(h.ends.CountOf(c), 0u);
  const snd_playback_handle d = Play(h, "D");  // 40 < 80, 100: refused
  h.Steps(2);
  SND_CHECK(h.ends.Has(d, SND_END_REJECTED, SND_END_DETAIL_LIMIT_GROUP));
  const snd_playback_handle c2 = Play(h, "C");  // 80 == 80: takes C (the lowest)
  h.Steps(4);
  SND_CHECK(h.ends.Has(c, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.CountOf(a), 0u);
  SND_CHECK_EQ(h.ends.CountOf(c2), 0u);
}

// The limit hierarchy: the event's instance limit refuses before the group steals; the
// retrigger interval refuses inside a group; the group steals among voices that the
// global real-voice limit keeps partly virtual (stats stay bounded).
SND_TEST(CoreVoice2, LimitHierarchyEventThenGroupThenGlobal) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 2;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.groups.push_back(TGroup{HashId("G"), 3, 0});  // oldest
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent one = SimpleEvent("One", 0);
  one.group = 0;
  one.maxInstances = 1;
  TEvent re = SimpleEvent("Re", 0);
  re.group = 0;
  re.retriggerMs = 50.0f;
  TEvent any = SimpleEvent("Any", 0);
  any.group = 0;
  b.events = {one, re, any};
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle o1 = Play(h, "One");
  h.Step();
  const snd_playback_handle o2 = Play(h, "One");  // the event limit, the group has room
  h.Steps(2);
  SND_CHECK(h.ends.Has(o2, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT));
  const snd_playback_handle r1 = Play(h, "Re");
  h.Step();
  const snd_playback_handle r2 = Play(h, "Re");  // inside 50 ms
  h.Steps(2);
  SND_CHECK(h.ends.Has(r2, SND_END_REJECTED, SND_END_DETAIL_RETRIGGER_INTERVAL));
  // One, Re: 2 voices (both real). Any x3: the group (3) steals the oldest each time over 3.
  snd_playback_handle any3[3];
  for (auto& p : any3) {
    p = Play(h, "Any");
    h.Steps(2);
  }
  h.Steps(4);
  SND_CHECK(h.ends.Has(o1, SND_END_COMPLETED));  // oldest of the group
  SND_CHECK(h.ends.Has(r1, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.CountOf(any3[0]), 0u);
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK_EQ(st.real_voices, 2u);     // the global limit
  SND_CHECK_EQ(st.virtual_voices, 1u);  // the third group member is virtual
}

// --- D-140: virtual voice return modes -----------------------------------------------
namespace {
// V (priority 100, the ramp) is pushed virtual by H (priority 200, silent, 12 blocks).
struct ReturnRun {
  Harness h;
  snd_playback_handle v{}, hi{};
  std::size_t vStart = 0;
  explicit ReturnRun(std::uint32_t virtualReturn, std::uint32_t assetFlags = 0) : h(Config()) {
    TestBank b = BasicBank();
    TAsset ramp{HashId("ramp"), 1, 48000, Ramp(100000), 0, 0};
    ramp.flags = assetFlags;
    b.assets.push_back(ramp);
    b.assets.push_back(TAsset{HashId("zero"), 1, 48000, Constant(12 * 256, 0.0f), 0, 0});
    TEvent ev = SimpleEvent("V", 0);
    ev.priority = 100;
    ev.virtualReturn = virtualReturn;
    TEvent hv = SimpleEvent("H", 1);
    hv.priority = 200;
    b.events = {ev, hv};
    h.Load("b", b.Build(HashId("B")));
    h.Clear();
    v = h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
    vStart = h.TotalFrames();
    h.Steps(4);
    hi = h.sys->Play(HashId("H"), snd_emitter_handle{0}, nullptr);
    h.Steps(24);
  }
  static snd_config Config() {
    snd_config c = SmallConfig();
    c.max_real_voices = 1;
    return c;
  }
};
}  // namespace

SND_TEST(CoreVoice2, VirtualReturnContinueKeepsTheTimeline) {
  ReturnRun r(0u);
  SND_CHECK(r.h.ends.Has(r.hi, SND_END_COMPLETED));
  SND_CHECK_EQ(r.h.ends.CountOf(r.v), 0u);
  // On the timeline it would have had without the interruption, to the frame.
  for (std::size_t f = r.h.TotalFrames() - 8; f < r.h.TotalFrames(); ++f) {
    const long d = FrameAt(r.h, f) - static_cast<long>(f - r.vStart);
    SND_CHECK_MSG(d == 0, "frame offset %ld", d);  // (the resampler's look-ahead is taken back)
  }
}

SND_TEST(CoreVoice2, VirtualReturnRestartStartsAgainFromTheBeginning) {
  ReturnRun r(1u);
  SND_CHECK_EQ(r.h.ends.CountOf(r.v), 0u);
  // Find where V sounds again (H is silent): it reads its frames from 0 there.
  std::size_t back = 0;
  for (std::size_t f = r.vStart + 6 * 256; f < r.h.TotalFrames(); ++f) {
    if (r.h.At(f, 0) != 0.0f) {
      back = f;
      break;
    }
  }
  SND_REQUIRE(back != 0u);
  // (The resampler's filter sees silence before the restart: skip its first 32 frames.)
  for (std::size_t f = back + 32; f < back + 96; ++f) SND_CHECK(std::labs(FrameAt(r.h, f) - static_cast<long>(f - back)) <= 1);
  const std::size_t last = r.h.TotalFrames() - 1;
  SND_CHECK_EQ(FrameAt(r.h, last), static_cast<long>(last - back));
  SND_CHECK(static_cast<long>(last - back) < static_cast<long>(last - r.vStart) - 2000);
}

SND_TEST(CoreVoice2, VirtualReturnNeverEndsTheVoiceAsStolen) {
  ReturnRun r(2u);
  SND_CHECK(r.h.ends.Has(r.v, SND_END_STOLEN, SND_END_DETAIL_VOICE_LIMIT));
  SND_CHECK_EQ(r.h.ends.CountOf(r.v), 1u);
  for (std::size_t f = r.h.TotalFrames() - 8 * 256; f < r.h.TotalFrames(); ++f) SND_CHECK_EQ(r.h.At(f, 0), 0.0f);
}

// The asset's own return mode (ASST.flags, applied by the source) wins over the Event's:
// with kAssetVirtualRestart core does not end a "never" voice (and leaves the return to
// the source; the test source ignores it, so V continues).
SND_TEST(CoreVoice2, VirtualReturnTheAssetFlagWins) {
  ReturnRun r(2u, bf::kAssetVirtualRestart);
  SND_CHECK_EQ(r.h.ends.CountOf(r.v), 0u);
  const std::size_t last = r.h.TotalFrames() - 1;
  const long d = FrameAt(r.h, last) - static_cast<long>(last - r.vStart);
  SND_CHECK_MSG(d == 0, "frame offset %ld", d);  // continued
}

// "Never": a voice that never gets a real slot ends at once (it went virtual at its start).
SND_TEST(CoreVoice2, VirtualReturnNeverEndsAVoiceThatStartsVirtual) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(100000, 0.1f), 0, 0});
  TEvent v = SimpleEvent("V", 0);
  v.priority = 50;
  v.virtualReturn = 2;
  TEvent hv = SimpleEvent("H", 0);
  hv.priority = 200;
  b.events = {v, hv};
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle hp = Play(h, "H");
  h.Step();
  const snd_playback_handle vp = Play(h, "V");
  h.Steps(3);
  SND_CHECK(h.ends.Has(vp, SND_END_STOLEN, SND_END_DETAIL_VOICE_LIMIT));
  SND_CHECK_EQ(h.ends.CountOf(hp), 0u);
  // A paused "never" voice loses its real slot without ending.
  const snd_playback_handle v2 = Play(h, "V");
  h.Steps(3);
  SND_CHECK(h.ends.Has(v2, SND_END_STOLEN));
  h.sys->Stop(hp, 0.0f, nullptr);
  h.Steps(3);
  const snd_playback_handle v3 = Play(h, "V");
  h.Steps(2);
  SND_CHECK_EQ(h.ends.CountOf(v3), 0u);  // real now
  h.sys->Pause(v3, 0.0f);
  h.Steps(4);
  h.sys->ResumePlayback(v3, 0.0f);
  h.Steps(4);
  SND_CHECK_EQ(h.ends.CountOf(v3), 0u);
  h.Clear();
  h.Step();
  SND_CHECK_NEAR(Mean(h, 128, 256, 0), kCenter * 0.1f, 1e-4f);
}
