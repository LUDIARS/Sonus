// core: voice management (4.4) and event sequences (4.2), 3D basics (4.5).
#include <cmath>
#include <set>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
float Mean(const Harness& h, std::size_t from, std::size_t to, std::uint32_t c) {
  double s = 0;
  for (std::size_t f = from; f < to; ++f) s += h.At(f, c);
  return static_cast<float>(s / double(to - from));
}
float Db(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

SND_TEST(CoreVoice, KeepsTheLoudestRealVoicesAndSumsThem) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 4;
  Harness h(cfg);
  TestBank b = BasicBank();
  // Audibility is a gain estimate (4.4): the clip volumes differ, the material is the same.
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  for (int i = 0; i < 8; ++i) {
    b.events.push_back(SimpleEvent(("E" + std::to_string(i)).c_str(), 0u, 0u, -2.0f * float(8 - i)));
  }
  h.Load("b", b.Build(HashId("B")));
  for (int i = 0; i < 8; ++i) h.sys->Play(HashId(("E" + std::to_string(i)).c_str()), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(8);
  // The 4 loudest (0.05 .. 0.08) are real: their sum.
  SND_CHECK_NEAR(Mean(h, 6 * 256, 8 * 256, 0), kCenter * 0.1f * (Db(-8.0f) + Db(-6.0f) + Db(-4.0f) + Db(-2.0f)), 1e-4f);
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK_EQ(st.real_voices, 4u);
  SND_CHECK_EQ(st.virtual_voices, 4u);
  SND_CHECK(h.sample.advances.load() > 0u);  // virtual voices keep moving
}

SND_TEST(CoreVoice, PriorityBeatsLoudness) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("loud"), 1, 48000, Constant(400000, 0.5f), 0, 0});
  b.assets.push_back(TAsset{HashId("soft"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent loud = SimpleEvent("Loud", 0);
  loud.priority = 10;
  TEvent soft = SimpleEvent("Soft", 1);
  soft.priority = 200;
  b.events.push_back(loud);
  b.events.push_back(soft);
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("Loud"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  h.sys->Play(HashId("Soft"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(6);
  // The soft high-priority voice steals the real slot; the loud one fades out (D-15).
  SND_CHECK_NEAR(Mean(h, 4 * 256, 6 * 256, 0), kCenter * 0.1f, 1e-4f);
}

SND_TEST(CoreVoice, StealFadeIsShortAndMonotone) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 1;
  cfg.tuning.short_fade_ms = 1024.0f * 1000.0f / 48000.0f;  // 4 blocks
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(400000, 1.0f), 0, 0});
  b.assets.push_back(TAsset{HashId("z"), 1, 48000, Constant(400000, 0.0f), 0, 0});
  TEvent a = SimpleEvent("A", 0);
  a.priority = 10;
  TEvent z = SimpleEvent("Z", 1);
  z.priority = 200;
  b.events.push_back(a);
  b.events.push_back(z);
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("A"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  h.sys->Play(HashId("Z"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(8);
  // A fades 1 -> 0 over 4 blocks, linearly, while Z (silent) already plays.
  for (std::size_t f = 1; f < 4 * 256; ++f) SND_CHECK(h.At(f, 0) <= h.At(f - 1, 0) + 1e-6f);
  SND_CHECK_NEAR(h.At(2 * 256, 0), kCenter * 0.5f, 2e-3f);
  SND_CHECK_NEAR(Mean(h, 5 * 256, 8 * 256, 0), 0.0f, 1e-6f);
}

SND_TEST(CoreVoice, FourTimesTheRealLimitStaysBounded) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 8;
  cfg.max_virtual_voices = 32;
  cfg.max_playbacks = 64;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4000, 0.01f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  std::vector<snd_playback_handle> ps;
  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 32; ++i) ps.push_back(h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr));
    h.Steps(2);
    h.sys->Update();
    snd_stats st{};
    h.sys->GetStats(&st);
    SND_CHECK(st.real_voices <= 8u);
    SND_CHECK(st.playbacks <= 64u);
  }
  h.Steps(40);
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  for (const snd_playback_handle p : ps) {
    if (p.value != 0u) SND_CHECK_EQ(h.ends.CountOf(p), 1u);
  }
  for (float v : h.out) SND_CHECK(std::isfinite(v));
}

SND_TEST(CoreVoice, LimitGroupNeverRejectsAndOldestSteals) {
  Harness h;
  TestBank b = BasicBank();
  b.groups.push_back(TGroup{HashId("Steps"), 2, 3});   // never
  b.groups.push_back(TGroup{HashId("Steps2"), 2, 0});  // oldest
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent n = SimpleEvent("N", 0);
  n.group = 0;
  TEvent o = SimpleEvent("O", 0);
  o.group = 1;
  b.events.push_back(n);
  b.events.push_back(o);
  h.Load("b", b.Build(HashId("B")));
  snd_playback_handle p[3], q[3];
  for (int i = 0; i < 3; ++i) {
    p[i] = h.sys->Play(HashId("N"), snd_emitter_handle{0}, nullptr);
    h.Step();
  }
  h.Steps(2);
  SND_CHECK(h.ends.Has(p[2], SND_END_REJECTED, SND_END_DETAIL_LIMIT_GROUP));
  SND_CHECK_EQ(h.ends.CountOf(p[0]), 0u);
  for (int i = 0; i < 3; ++i) {
    q[i] = h.sys->Play(HashId("O"), snd_emitter_handle{0}, nullptr);
    h.Step();
  }
  h.Steps(4);
  SND_CHECK(h.ends.Has(q[0], SND_END_STOLEN, SND_END_DETAIL_LIMIT_GROUP));
  SND_CHECK_EQ(h.ends.CountOf(q[1]), 0u);
  SND_CHECK_EQ(h.ends.CountOf(q[2]), 0u);
}

SND_TEST(CoreVoice, InstanceLimitAndRetrigger) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(400000, 0.1f), 0, 0});
  TEvent e = SimpleEvent("E", 0);
  e.maxInstances = 1;
  TEvent r = SimpleEvent("R", 0);
  r.retriggerMs = 100.0f;
  b.events.push_back(e);
  b.events.push_back(r);
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle a = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Step();
  const snd_playback_handle a2 = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle r1 = h.sys->Play(HashId("R"), snd_emitter_handle{0}, nullptr);
  h.Step();
  const snd_playback_handle r2 = h.sys->Play(HashId("R"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  SND_CHECK_EQ(h.ends.CountOf(a), 0u);
  SND_CHECK(h.ends.Has(a2, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT));
  SND_CHECK_EQ(h.ends.CountOf(r1), 0u);
  SND_CHECK(h.ends.Has(r2, SND_END_REJECTED, SND_END_DETAIL_RETRIGGER_INTERVAL));
  h.Steps(20);  // > 100 ms later: accepted
  const snd_playback_handle r3 = h.sys->Play(HashId("R"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  SND_CHECK_EQ(h.ends.CountOf(r3), 0u);
}

// Sequences: each clip plays a distinct DC level; the level tells which was chosen.
namespace {
TestBank SequenceBank(std::uint32_t kind, std::uint32_t clips) {
  TestBank b = BasicBank();
  TEvent e;
  e.id = HashId("Seq");
  e.sequence = kind;
  e.comboWindowMs = 50.0f;
  TTrack t;
  t.layerParam = HashId("Surface");
  for (std::uint32_t i = 0; i < clips; ++i) {
    b.assets.push_back(TAsset{HashId(("c" + std::to_string(i)).c_str()), 1, 48000, Constant(512, 0.1f * float(i + 1)), 0, 0});
    TClip c;
    c.target = i;
    c.switchValue = i == 0 ? 0u : HashId(("v" + std::to_string(i)).c_str());
    t.clips.push_back(c);
  }
  e.tracks.push_back(t);
  b.events.push_back(e);
  return b;
}
// Plays Seq once and returns the chosen clip index (from the level).
int PlayOnce(Harness& h) {
  h.Clear();
  h.sys->Play(HashId("Seq"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  const float v = h.At(256 + 10, 0) / kCenter;
  h.Steps(2);
  return static_cast<int>(std::lround(v / 0.1f)) - 1;
}
}  // namespace

SND_TEST(CoreSequence, SequentialCycles) {
  Harness h;
  h.Load("b", SequenceBank(static_cast<std::uint32_t>(bf::SequenceKind::Sequential), 3).Build(HashId("B")));
  int got[6];
  for (int& g : got) g = PlayOnce(h);
  const int expect[6] = {0, 1, 2, 0, 1, 2};
  for (int i = 0; i < 6; ++i) SND_CHECK_EQ(got[i], expect[i]);
}

SND_TEST(CoreSequence, ShuffleCoversEveryClipBeforeRepeating) {
  Harness h;
  h.Load("b", SequenceBank(static_cast<std::uint32_t>(bf::SequenceKind::Shuffle), 4).Build(HashId("B")));
  for (int round = 0; round < 3; ++round) {
    std::set<int> seen;
    for (int i = 0; i < 4; ++i) seen.insert(PlayOnce(h));
    SND_CHECK_EQ(seen.size(), std::size_t{4});
  }
}

SND_TEST(CoreSequence, RandomNoRepeatNeverRepeats) {
  Harness h;
  h.Load("b", SequenceBank(static_cast<std::uint32_t>(bf::SequenceKind::RandomNoRepeat), 3).Build(HashId("B")));
  int last = -1;
  std::set<int> seen;
  for (int i = 0; i < 20; ++i) {
    const int c = PlayOnce(h);
    SND_CHECK(c >= 0 && c < 3);
    SND_CHECK(c != last);
    seen.insert(c);
    last = c;
  }
  SND_CHECK_EQ(seen.size(), std::size_t{3});
}

SND_TEST(CoreSequence, SwitchFollowsTheSwitchValue) {
  Harness h;
  h.Load("b", SequenceBank(static_cast<std::uint32_t>(bf::SequenceKind::Switch), 3).Build(HashId("B")));
  SND_CHECK_EQ(PlayOnce(h), 0);  // no value: the default clip (switchValue 0)
  h.sys->SetSwitchGlobal(HashId("Surface"), HashId("v2"));
  SND_CHECK_EQ(PlayOnce(h), 2);
  h.sys->SetSwitchGlobal(HashId("Surface"), HashId("v1"));
  SND_CHECK_EQ(PlayOnce(h), 1);
}

SND_TEST(CoreSequence, ComboAdvancesOnlyInsideTheWindow) {
  Harness h;
  h.Load("b", SequenceBank(static_cast<std::uint32_t>(bf::SequenceKind::ComboSequential), 3).Build(HashId("B")));
  // PlayOnce spaces plays by 4 blocks (21 ms) < 50 ms window: 0, 1, 2, 0 ...
  SND_CHECK_EQ(PlayOnce(h), 0);
  SND_CHECK_EQ(PlayOnce(h), 1);
  SND_CHECK_EQ(PlayOnce(h), 2);
  h.Steps(20);  // > 50 ms: restart
  SND_CHECK_EQ(PlayOnce(h), 0);
}

SND_TEST(Core3D, DistanceAndDirection) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(400000, 1.0f), 0, 0});
  TEvent e = SimpleEvent("E", 0);
  e.flags = bf::kEvent3D;
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  snd_listener_desc l{};
  l.forward = snd_vec3{0, 0, 1};
  l.up = snd_vec3{0, 1, 0};
  l.enabled = 1;
  h.sys->SetListener(0, &l);
  const snd_emitter_handle em = h.sys->CreateEmitter();
  const snd_vec3 front{0, 0, 4};
  h.sys->SetTransform(em, &front, nullptr, nullptr);
  h.sys->Play(HashId("E"), em, nullptr);
  h.Clear();
  h.Steps(4);
  // 1 / distance; straight ahead: equal power left / right.
  SND_CHECK_NEAR(Mean(h, 3 * 256, 4 * 256, 0), 0.25f * kCenter, 1e-3f);
  SND_CHECK_NEAR(Mean(h, 3 * 256, 4 * 256, 1), 0.25f * kCenter, 1e-3f);
  const snd_vec3 right{2, 0, 0};
  h.sys->SetTransform(em, &right, nullptr, nullptr);
  h.Clear();
  h.Steps(4);
  const float L = Mean(h, 3 * 256, 4 * 256, 0), R = Mean(h, 3 * 256, 4 * 256, 1);
  SND_CHECK(R > 0.45f && L < 0.05f);
  SND_CHECK_NEAR(L * L + R * R, 0.25f, 2e-3f);  // energy preserving at 1/2
  (void)Db;
}
