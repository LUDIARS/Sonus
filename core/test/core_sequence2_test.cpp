// core, stage 3 group 4: sequence kinds and clips (4.2) - weighted random, switch scopes,
// combo window, sequential state across playbacks, shuffle without a limit (D-141), random
// ranges, loop counts (D-142), CLIP.fadeOutMs, layer crossfade (D-143), nested events
// (D-144), action tracks (D-145), the fade-in of a play (D-146).
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;
using snd::Id;

namespace {
constexpr float kCenter = 0.70710678f;
constexpr std::uint32_t kSeq(bf::SequenceKind k) { return static_cast<std::uint32_t>(k); }
float Mean(const Harness& h, std::size_t from, std::size_t to, std::uint32_t c) {
  double s = 0;
  for (std::size_t f = from; f < to; ++f) s += h.At(f, c);
  return static_cast<float>(s / double(to - from));
}
std::string Name(const char* p, int i) { return std::string(p) + std::to_string(i); }

// Event "Seq" with `n` clips; clip i plays DC (i + 1) * step: the level tells the clip.
TestBank LevelBank(std::uint32_t kind, int n, float step, std::vector<float> weights = {}) {
  TestBank b = BasicBank();
  TEvent e;
  e.id = HashId("Seq");
  e.sequence = kind;
  TTrack t;
  t.layerParam = HashId("Surface");
  for (int i = 0; i < n; ++i) {
    b.assets.push_back(TAsset{HashId(Name("c", i).c_str()), 1, 48000, Constant(512, step * float(i + 1)), 0, 0});
    TClip c;
    c.target = static_cast<std::uint32_t>(i);
    c.weight = i < static_cast<int>(weights.size()) ? weights[static_cast<std::size_t>(i)] : 1.0f;
    c.switchValue = i == 0 ? 0u : HashId(Name("v", i).c_str());
    t.clips.push_back(c);
  }
  e.tracks.push_back(t);
  b.events.push_back(e);
  return b;
}
// Plays `id` and returns the chosen clip (from the level of the second block).
int PlayOnce(Harness& h, float step, snd_emitter_handle em = snd_emitter_handle{0}, Id sw = 0, Id swv = 0) {
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("Seq"), em, nullptr);
  if (sw != 0u) h.sys->SetSwitchPlayback(p, sw, swv);
  h.Steps(2);
  const float v = h.At(256 + 10, 0) / kCenter;
  h.Steps(2);
  return static_cast<int>(std::lround(v / step)) - 1;
}
}  // namespace

// CLIP.weight: chosen in proportion (deterministic seed, 900 plays); weight 0 never.
SND_TEST(CoreSequence2, WeightedRandomFollowsTheWeights) {
  Harness h;
  h.Load("b", LevelBank(kSeq(bf::SequenceKind::Random), 4, 0.1f, {1.0f, 2.0f, 0.0f, 3.0f}).Build(HashId("B")));
  int count[4] = {0, 0, 0, 0};
  const int plays = 900;
  for (int i = 0; i < plays; ++i) {
    const int c = PlayOnce(h, 0.1f);
    SND_REQUIRE(c >= 0 && c < 4);
    ++count[c];
  }
  snd::test::Note("weights 1:2:0:3 -> %d %d %d %d", count[0], count[1], count[2], count[3]);
  SND_CHECK_EQ(count[2], 0);
  // Expected 150 / 300 / 450 (binomial sd 11 / 14 / 15): within 4 sd.
  SND_CHECK(std::abs(count[0] - 150) < 45);
  SND_CHECK(std::abs(count[1] - 300) < 56);
  SND_CHECK(std::abs(count[3] - 450) < 60);
}

// Every weight 0: the clips are equally likely (none is silently dropped).
SND_TEST(CoreSequence2, AllZeroWeightsAreEquallyLikely) {
  Harness h;
  h.Load("b", LevelBank(kSeq(bf::SequenceKind::Random), 3, 0.1f, {0.0f, 0.0f, 0.0f}).Build(HashId("B")));
  int count[3] = {0, 0, 0};
  for (int i = 0; i < 300; ++i) {
    const int c = PlayOnce(h, 0.1f);
    SND_REQUIRE(c >= 0 && c < 3);
    ++count[c];
  }
  for (int c : count) SND_CHECK(std::abs(c - 100) < 35);
}

// Same seed, same choices (2.5): two systems with the same script choose the same clips.
SND_TEST(CoreSequence2, WeightedRandomIsDeterministic) {
  std::vector<int> runs[2];
  for (auto& r : runs) {
    Harness h;
    h.Load("b", LevelBank(kSeq(bf::SequenceKind::Random), 4, 0.1f, {1.0f, 2.0f, 0.5f, 3.0f}).Build(HashId("B")));
    for (int i = 0; i < 40; ++i) r.push_back(PlayOnce(h, 0.1f));
  }
  SND_CHECK(runs[0] == runs[1]);
}

// RandomNoRepeat with a zero weight: never the zero-weight clip, never the same twice.
SND_TEST(CoreSequence2, RandomNoRepeatSkipsZeroWeights) {
  Harness h;
  h.Load("b", LevelBank(kSeq(bf::SequenceKind::RandomNoRepeat), 3, 0.1f, {1.0f, 0.0f, 1.0f}).Build(HashId("B")));
  int last = -1;
  for (int i = 0; i < 30; ++i) {
    const int c = PlayOnce(h, 0.1f);
    SND_CHECK(c == 0 || c == 2);
    SND_CHECK(c != last);
    last = c;
  }
}

// Switch: Playback scope over Emitter over Global; an unknown value plays the default clip
// (CLIP.switchValue 0); no value at all plays the default too.
SND_TEST(CoreSequence2, SwitchScopesAndDefaultClip) {
  Harness h;
  h.Load("b", LevelBank(kSeq(bf::SequenceKind::Switch), 4, 0.1f).Build(HashId("B")));
  const Id sw = HashId("Surface");
  const snd_emitter_handle em = h.sys->CreateEmitter();
  SND_CHECK_EQ(PlayOnce(h, 0.1f, em), 0);
  h.sys->SetSwitchGlobal(sw, HashId("v1"));
  SND_CHECK_EQ(PlayOnce(h, 0.1f, em), 1);  // global
  h.sys->SetSwitchEmitter(em, sw, HashId("v2"));
  SND_CHECK_EQ(PlayOnce(h, 0.1f, em), 2);                        // emitter over global
  SND_CHECK_EQ(PlayOnce(h, 0.1f, snd_emitter_handle{0}), 1);     // another emitter: global
  SND_CHECK_EQ(PlayOnce(h, 0.1f, em, sw, HashId("v3")), 3);      // playback over emitter
  SND_CHECK_EQ(PlayOnce(h, 0.1f, em, sw, HashId("nothing")), 0);  // unknown value: default clip
  h.sys->SetSwitchGlobal(sw, HashId("none-of-them"));
  SND_CHECK_EQ(PlayOnce(h, 0.1f, snd_emitter_handle{0}), 0);
}

// Combo: the next clip when the play comes within EVNT.comboWindowMs of the previous one
// (inclusive), the first clip again after a longer gap.
SND_TEST(CoreSequence2, ComboWindowBoundaryIsInclusive) {
  Harness h;
  TestBank b = LevelBank(kSeq(bf::SequenceKind::ComboSequential), 3, 0.1f);
  b.events[0].comboWindowMs = 48.005f;  // 2304.24 frames -> 2304 = 9 blocks
  h.Load("b", b.Build(HashId("B")));
  auto at = [&](int gapBlocks) {  // play after gapBlocks blocks since the previous play
    h.Clear();
    h.sys->Play(HashId("Seq"), snd_emitter_handle{0}, nullptr);
    h.Steps(2);
    const int c = static_cast<int>(std::lround(h.At(256 + 10, 0) / kCenter / 0.1f)) - 1;
    h.Steps(static_cast<std::uint32_t>(gapBlocks - 2));
    return c;
  };
  SND_CHECK_EQ(at(9), 0);
  SND_CHECK_EQ(at(9), 1);   // exactly 9 blocks = the window: next
  SND_CHECK_EQ(at(10), 2);  // next (within the window of the previous)
  SND_CHECK_EQ(at(9), 0);   // 10 blocks after the previous: back to the first
}

// Sequential: the position belongs to the event, not to a playback - overlapping and
// stopped playbacks advance it alike; a new load of the bank starts again.
SND_TEST(CoreSequence2, SequentialStateIsPerEventAcrossPlaybacks) {
  Harness h;
  const auto bytes = LevelBank(kSeq(bf::SequenceKind::Sequential), 3, 0.1f).Build(HashId("B"));
  snd_bank_handle bank = h.Load("b", bytes);
  SND_CHECK_EQ(PlayOnce(h, 0.1f), 0);
  // Two overlapping playbacks in one block take 1 and 2 (sum 0.2 + 0.3).
  h.Clear();
  const snd_playback_handle p1 = h.sys->Play(HashId("Seq"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("Seq"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  SND_CHECK_NEAR(h.At(256 + 10, 0), kCenter * 0.5f, 1e-4f);
  h.sys->Stop(p1, 0.0f, nullptr);
  h.Steps(2);
  SND_CHECK_EQ(PlayOnce(h, 0.1f), 0);  // wrapped
  SND_CHECK_EQ(PlayOnce(h, 0.1f), 1);
  h.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD);
  h.Steps(10);
  bank = h.Load("b2", bytes);
  SND_CHECK_EQ(PlayOnce(h, 0.1f), 0);  // a new load: a new position
}

// D-141: shuffle of 100 clips (no limit): each round plays every clip once, rounds differ,
// a round never starts with the clip the previous round ended with; same seed same order.
SND_TEST(CoreSequence2, ShuffleWithoutLimitIsAPermutationPerRound) {
  std::vector<int> orders[2];
  for (auto& order : orders) {
    Harness h;
    h.Load("b", LevelBank(kSeq(bf::SequenceKind::Shuffle), 100, 0.005f).Build(HashId("B")));
    for (int i = 0; i < 300; ++i) order.push_back(PlayOnce(h, 0.005f));
  }
  SND_CHECK(orders[0] == orders[1]);
  const std::vector<int>& o = orders[0];
  for (int round = 0; round < 3; ++round) {
    std::set<int> seen(o.begin() + round * 100, o.begin() + round * 100 + 100);
    SND_CHECK_EQ(seen.size(), std::size_t{100});
    SND_CHECK(*seen.begin() == 0 && *seen.rbegin() == 99);
  }
  SND_CHECK(!std::equal(o.begin(), o.begin() + 100, o.begin() + 100));
  SND_CHECK(!std::equal(o.begin() + 100, o.begin() + 200, o.begin() + 200));
  for (std::size_t i = 1; i < o.size(); ++i) SND_CHECK(o[i] != o[i - 1]);
}

// D-141 with 2 clips: the boundary rule alternates them (a, b, a, b, ...).
SND_TEST(CoreSequence2, ShuffleOfTwoNeverRepeats) {
  Harness h;
  h.Load("b", LevelBank(kSeq(bf::SequenceKind::Shuffle), 2, 0.1f).Build(HashId("B")));
  int last = -1;
  for (int i = 0; i < 20; ++i) {
    const int c = PlayOnce(h, 0.1f);
    SND_CHECK(c != last);
    last = c;
  }
}

// --- Random ranges (4.2) ------------------------------------------------------------------
namespace {
struct RangeRun {
  Harness h;
  explicit RangeRun(const TClip& clip, std::vector<float> data, std::uint32_t rate = 48000) {
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("a"), 1, rate, std::move(data), 0, 0});
    TEvent e = SimpleEvent("R", 0);
    e.tracks[0].clips[0] = clip;
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
  }
  // Plays once; returns the samples of the play's blocks 1..3 (stereo interleaved).
  std::vector<float> Once() {
    h.Clear();
    const snd_playback_handle p = h.sys->Play(HashId("R"), snd_emitter_handle{0}, nullptr);
    h.Steps(4);
    std::vector<float> v(h.all.begin() + 256 * 2, h.all.end());
    h.sys->Stop(p, 0.0f, nullptr);  // one play at a time
    h.Steps(4);
    return v;
  }
};
double Rms(const std::vector<float>& v, std::uint32_t ch) {
  double s = 0;
  std::size_t n = 0;
  for (std::size_t i = ch; i < v.size(); i += 2) {
    s += double(v[i]) * v[i];
    ++n;
  }
  return std::sqrt(s / double(n));
}
double Frequency(const std::vector<float>& v) {  // zero crossings of channel 0
  int crossings = 0;
  std::size_t first = 0, last = 0;
  for (std::size_t i = 2; i < v.size(); i += 2) {
    if (v[i - 2] < 0.0f && v[i] >= 0.0f) {
      if (crossings == 0) first = i / 2;
      last = i / 2;
      ++crossings;
    }
  }
  return crossings > 1 ? 48000.0 * double(crossings - 1) / double(last - first) : 0.0;
}
}  // namespace

SND_TEST(CoreSequence2, RandomVolumeStaysInRange) {
  TClip c;
  c.randVolumeDb = 6.0f;
  RangeRun r(c, Constant(48000, 0.5f));
  std::set<long> distinct;
  for (int i = 0; i < 40; ++i) {
    const double db = 20.0 * std::log10(Rms(r.Once(), 0) / (0.5 * kCenter));
    SND_CHECK_MSG(db >= -6.001 && db <= 6.001, "%g dB", db);
    distinct.insert(std::lround(db * 100.0));
  }
  SND_CHECK(distinct.size() > 10u);
  TClip z;  // width 0: no jitter
  RangeRun r0(z, Constant(48000, 0.5f));
  for (int i = 0; i < 5; ++i) SND_CHECK_NEAR(Rms(r0.Once(), 0), 0.5 * kCenter, 1e-5);
}

SND_TEST(CoreSequence2, RandomPitchStaysInRange) {
  TClip c;
  c.randPitchCents = 100.0f;
  RangeRun r(c, Sine(96000, 1000.0, 48000.0, 0.5f));
  std::set<long> distinct;
  for (int i = 0; i < 30; ++i) {
    const double hz = Frequency(r.Once());
    // +-100 cents, and the measurement (+-1 frame over ~16 periods, ~3 cents).
    SND_CHECK_MSG(hz > 1000.0 * std::pow(2.0, -104.0 / 1200.0) && hz < 1000.0 * std::pow(2.0, 104.0 / 1200.0), "%g Hz", hz);
    distinct.insert(std::lround(hz));
  }
  SND_CHECK(distinct.size() > 8u);
  TClip z;
  RangeRun r0(z, Sine(96000, 1000.0, 48000.0, 0.5f));
  for (int i = 0; i < 3; ++i) SND_CHECK_NEAR(Frequency(r0.Once()), 1000.0, 2.0);
}

SND_TEST(CoreSequence2, RandomPanStaysInRange) {
  TClip c;
  c.randPan = 0.5f;
  RangeRun r(c, Constant(48000, 0.5f));
  const double maxRatio = std::tan((0.5 + 1.0) * 0.25 * 3.14159265358979) + 1e-3;  // pan +0.5 (constant power)
  std::set<long> distinct;
  for (int i = 0; i < 40; ++i) {
    const std::vector<float> v = r.Once();
    const double ratio = Rms(v, 1) / Rms(v, 0);
    SND_CHECK_MSG(ratio <= maxRatio && ratio >= 1.0 / maxRatio, "R/L %g", ratio);
    distinct.insert(std::lround(ratio * 1000.0));
  }
  SND_CHECK(distinct.size() > 10u);
  TClip z;
  RangeRun r0(z, Constant(48000, 0.5f));
  const std::vector<float> v0 = r0.Once();
  SND_CHECK_NEAR(Rms(v0, 1) / Rms(v0, 0), 1.0, 1e-5);
}

SND_TEST(CoreSequence2, RandomStartStaysInRange) {
  TClip c;
  c.randStartMs = 10.0f;  // 0 .. 480 frames
  std::vector<float> ramp(48000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(double(i + 1) / 65536.0);
  RangeRun r(c, ramp);
  std::set<long> distinct;
  for (int i = 0; i < 40; ++i) {
    const std::vector<float> v = r.Once();  // block 1 of the play: frame 256 + start
    const long start = std::lround(double(v[0]) / kCenter * 65536.0) - 1 - 256;
    SND_CHECK_MSG(start >= 0 && start <= 480, "start %ld", start);
    distinct.insert(start);
  }
  SND_CHECK(distinct.size() > 10u);
}

SND_TEST(CoreSequence2, RandomFilterStaysInRange) {
  // Cutoff in [20 kHz - 15 kHz, 20 kHz]: a 300 Hz tone passes, a 15 kHz tone varies.
  TClip c;
  c.randFilterHz = 15000.0f;
  std::vector<float> two = Sine(96000, 300.0, 48000.0, 0.25f);
  const std::vector<float> hi = Sine(96000, 15000.0, 48000.0, 0.25f);
  for (std::size_t i = 0; i < two.size(); ++i) two[i] += hi[i];
  RangeRun r(c, two);
  std::set<long> distinct;
  for (int i = 0; i < 30; ++i) {
    const std::vector<float> v = r.Once();
    const double low = ToneAmplitude(v, 2, 0, 256, 512, 300.0, 48000.0) / kCenter;
    const double high = ToneAmplitude(v, 2, 0, 256, 512, 15000.0, 48000.0) / kCenter;
    SND_CHECK_MSG(low > 0.25 * 0.95 && low < 0.25 * 1.05, "300 Hz %g", low);
    SND_CHECK(high < 0.25 * 1.05);
    distinct.insert(std::lround(high * 1000.0));
  }
  SND_CHECK(distinct.size() > 5u);
}

// --- D-142 loop counts, CLIP.fadeOutMs ----------------------------------------------------
namespace {
struct LoopRun {
  Harness h;
  snd_playback_handle p{};
  LoopRun(std::uint32_t loops, std::size_t frames, std::uint64_t lb = 0, std::uint64_t le = 0, float fadeOutMs = 0.0f) {
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(frames, 0.5f), lb, le});
    TEvent e = SimpleEvent("L", 0);
    e.tracks[0].clips[0].loopCount = loops;
    e.tracks[0].clips[0].fadeOutMs = fadeOutMs;
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
    h.Clear();
    p = h.sys->Play(HashId("L"), snd_emitter_handle{0}, nullptr);
  }
};
}  // namespace

SND_TEST(CoreSequence2, LoopCountPlaysNPassesAndCutsAtTheEnd) {
  LoopRun r(3, 1000);  // 3 passes of 1000 frames, cut at frame 3000
  r.h.Steps(16);
  SND_CHECK_NEAR(r.h.At(2990, 0), 0.5f * kCenter, 1e-4f);
  SND_CHECK_NEAR(r.h.At(2999, 0), 0.5f * kCenter, 1e-3f);
  for (std::size_t f = 3000; f < 3100; ++f) SND_CHECK_EQ(r.h.At(f, 0), 0.0f);
  SND_CHECK(r.h.ends.Has(r.p, SND_END_COMPLETED));
  SND_CHECK_EQ(r.h.ends.CountOf(r.p), 1u);
}

SND_TEST(CoreSequence2, LoopCountWithLoopPointsCountsTheLoopRegion) {
  LoopRun r(2, 2000, 500, 1500);  // [0, 1500) then [500, 1500) once more: 2500 frames
  r.h.Steps(16);
  SND_CHECK_NEAR(r.h.At(2490, 0), 0.5f * kCenter, 1e-4f);
  for (std::size_t f = 2500; f < 2600; ++f) SND_CHECK_EQ(r.h.At(f, 0), 0.0f);
  SND_CHECK(r.h.ends.Has(r.p, SND_END_COMPLETED));
}

SND_TEST(CoreSequence2, LoopCountZeroIsEndlessAndOneIsOnePass) {
  LoopRun endless(0, 1000);
  endless.h.Steps(40);  // 10240 frames
  SND_CHECK_EQ(endless.h.ends.CountOf(endless.p), 0u);
  SND_CHECK_NEAR(endless.h.At(9000, 0), 0.5f * kCenter, 1e-4f);
  LoopRun once(1, 1000);
  once.h.Steps(10);
  SND_CHECK(once.h.ends.Has(once.p, SND_END_COMPLETED));
  SND_CHECK_NEAR(once.h.At(900, 0), 0.5f * kCenter, 1e-4f);
  SND_CHECK_NEAR(once.h.At(1100, 0), 0.0f, 1e-3f);
}

// CLIP.fadeOutMs: a linear fade over the last fadeOutMs before the clip's end (here the
// end of a counted loop; 2400 frames of 50 ms before frame 4800).
SND_TEST(CoreSequence2, ClipFadeOutRampsToTheEnd) {
  LoopRun r(2, 2400, 0, 0, 50.0f);
  r.h.Steps(24);
  SND_CHECK_NEAR(r.h.At(2000, 0), 0.5f * kCenter, 1e-4f);
  SND_CHECK_NEAR(r.h.At(3600, 0), 0.25f * kCenter, 0.01f);
  SND_CHECK_NEAR(r.h.At(4200, 0), 0.125f * kCenter, 0.01f);
  for (std::size_t f = 2400; f < 4800; ++f) SND_CHECK(r.h.At(f, 0) <= r.h.At(f - 1, 0) + 1e-6f);
  for (std::size_t f = 4800; f < 4900; ++f) SND_CHECK_EQ(r.h.At(f, 0), 0.0f);
  LoopRun single(1, 4800, 0, 0, 50.0f);  // a plain clip: toward the asset's end
  single.h.Steps(24);
  SND_CHECK_NEAR(single.h.At(3600, 0), 0.25f * kCenter, 0.01f);
}

// --- D-143 layer crossfade -------------------------------------------------------------
namespace {
// Clip A: stereo (0.1, 0) range [lo0, hi0]; clip B: (0, 0.1) range [lo1, hi1]: L tells A's
// weight, R tells B's.
struct LayerRun {
  Harness h;
  float ref = 0;
  LayerRun(float lo0, float hi0, float lo1, float hi1, float rampRate) {
    TestBank b = BasicBank();
    std::vector<float> l(2 * 200000, 0.0f), r(2 * 200000, 0.0f);
    for (std::size_t i = 0; i < 200000; ++i) {
      l[2 * i] = 0.1f;
      r[2 * i + 1] = 0.1f;
    }
    b.assets.push_back(TAsset{HashId("l"), 2, 48000, l, 0, 0});
    b.assets.push_back(TAsset{HashId("r"), 2, 48000, r, 0, 0});
    b.params.push_back(TParam{HashId("X"), 0, 0.0f, 1.0f, 0.0f, rampRate});
    TEvent e;
    e.id = HashId("Layer");
    e.sequence = kSeq(bf::SequenceKind::LayerCrossfade);
    TTrack t;
    t.layerParam = HashId("X");
    TClip a;
    a.target = 0;
    a.layerMin = lo0;
    a.layerMax = hi0;
    TClip c = a;
    c.target = 1;
    c.layerMin = lo1;
    c.layerMax = hi1;
    t.clips = {a, c};
    e.tracks.push_back(t);
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
    h.sys->SetParamGlobal(HashId("X"), 0.0f);
    h.sys->Play(HashId("Layer"), snd_emitter_handle{0}, nullptr);
    h.Steps(4);
    ref = h.out[2 * 128];  // A alone at x = 0
  }
  float WA(std::size_t f) const { return h.At(f, 0) / ref; }
  float WB(std::size_t f) const { return h.At(f, 1) / ref; }
};
}  // namespace

SND_TEST(CoreSequence2, LayerCrossfadeIsEqualPowerInTheOverlap) {
  LayerRun r(0.0f, 0.6f, 0.4f, 1.0f, 0.0f);
  auto at = [&](float x) {
    r.h.sys->SetParamGlobal(HashId("X"), x);
    r.h.Clear();
    r.h.Steps(8);  // the short-fade limiter settles
    return std::pair<float, float>{r.WA(7 * 256 + 100), r.WB(7 * 256 + 100)};
  };
  auto w = at(0.3f);
  SND_CHECK_NEAR(w.first, 1.0f, 1e-4f);
  SND_CHECK_NEAR(w.second, 0.0f, 1e-6f);
  w = at(0.5f);  // the middle of [0.4, 0.6]
  SND_CHECK_NEAR(w.first, std::cos(0.25f * 3.14159265f), 1e-4f);
  SND_CHECK_NEAR(w.second, std::sin(0.25f * 3.14159265f), 1e-4f);
  SND_CHECK_NEAR(w.first * w.first + w.second * w.second, 1.0f, 1e-4f);
  w = at(0.45f);
  SND_CHECK_NEAR(w.first, std::cos(0.125f * 3.14159265f), 1e-4f);
  SND_CHECK_NEAR(w.second, std::sin(0.125f * 3.14159265f), 1e-4f);
  w = at(0.8f);
  SND_CHECK_NEAR(w.first, 0.0f, 1e-6f);
  SND_CHECK_NEAR(w.second, 1.0f, 1e-4f);
}

// Following the parameter's ramp (PARM.rampRate) the weights move without clicks.
SND_TEST(CoreSequence2, LayerCrossfadeFollowsTheRampWithoutClicks) {
  LayerRun r(0.0f, 0.6f, 0.4f, 1.0f, 2.0f);  // 2 units/s: 0 -> 1 in 0.5 s
  r.h.sys->SetParamGlobal(HashId("X"), 1.0f);
  r.h.Clear();
  r.h.Steps(120);
  float maxStep = 0.0f;
  for (std::size_t f = 1; f < r.h.TotalFrames(); ++f) {
    maxStep = std::max(maxStep, std::fabs(r.h.At(f, 0) - r.h.At(f - 1, 0)));
    maxStep = std::max(maxStep, std::fabs(r.h.At(f, 1) - r.h.At(f - 1, 1)));
  }
  snd::test::Note("largest sample step %g (of %g)", maxStep, r.ref);
  SND_CHECK(maxStep < r.ref * 0.002f);
  SND_CHECK_NEAR(r.WB(r.h.TotalFrames() - 1), 1.0f, 1e-4f);
}

// Ranges that only touch switch at the shared value over the short fade (D-15).
SND_TEST(CoreSequence2, LayerCrossfadeTouchingRangesSwitchWithTheShortFade) {
  LayerRun r(0.0f, 0.5f, 0.5f, 1.0f, 0.0f);
  r.h.sys->SetParamGlobal(HashId("X"), 0.7f);
  r.h.Clear();
  r.h.Steps(4);
  float maxStep = 0.0f;
  for (std::size_t f = 1; f < r.h.TotalFrames(); ++f) maxStep = std::max(maxStep, std::fabs(r.h.At(f, 0) - r.h.At(f - 1, 0)));
  SND_CHECK(maxStep < r.ref * 0.01f);  // over >= 1 block, not a step
  SND_CHECK_NEAR(r.WA(3 * 256), 0.0f, 1e-6f);
  SND_CHECK_NEAR(r.WB(3 * 256), 1.0f, 1e-4f);
  r.h.sys->SetParamGlobal(HashId("X"), 0.5f);  // the shared value: the clip above
  r.h.Clear();
  r.h.Steps(4);
  SND_CHECK_NEAR(r.WA(3 * 256), 0.0f, 1e-6f);
  SND_CHECK_NEAR(r.WB(3 * 256), 1.0f, 1e-4f);
}

// --- D-144 nested events -------------------------------------------------------------
namespace {
// Buses: Sfx (row 0) and Other (row 1) into Master (row 2). P (bus Sfx) has one clip: the
// nested event C (bus Other, limit group G of 1 "never"), whose clip plays DC 0.2.
TestBank NestedBank(float parentClipDb, float childClipDb) {
  TestBank b;
  b.buses.push_back(TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Other"), 2u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.groups.push_back(TGroup{HashId("G"), 1, 3});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(4096, 0.2f), 0, 0});
  TEvent p;
  p.id = HashId("P");
  p.bus = 0;
  TTrack pt;
  TClip nested;
  nested.kind = 1;
  nested.target = 1;  // EVNT row of C
  nested.volumeDb = parentClipDb;
  pt.clips.push_back(nested);
  p.tracks.push_back(pt);
  TEvent c = SimpleEvent("C", 0, 1, childClipDb);
  c.group = 0;
  b.events = {p, c};
  return b;
}
}  // namespace

SND_TEST(CoreSequence2, NestedEventUsesItsOwnRoutingAndTheParentClipGain) {
  Harness h;
  h.Load("b", NestedBank(-6.0f, -3.0f).Build(HashId("B")));
  const float expect = kCenter * 0.2f * std::pow(10.0f, -9.0f / 20.0f);  // parent clip x child clip
  const snd_playback_handle p = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 300, 700, 0), expect, 1e-4f);
  h.sys->SetBusVolume(HashId("Sfx"), 0.0f, 0.0f);  // the parent's bus: no effect
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 300, 700, 0), expect, 1e-4f);
  h.sys->SetBusVolume(HashId("Other"), 0.0f, 0.0f);  // the nested event's own bus
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 300, 700, 0), 0.0f, 1e-6f);
  h.sys->SetBusVolume(HashId("Other"), 1.0f, 0.0f);
  // The nested event's limit group (1 voice, never): a second P is refused there.
  const snd_playback_handle p2 = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  SND_CHECK(h.ends.Has(p2, SND_END_REJECTED, SND_END_DETAIL_LIMIT_GROUP));
  // One end for the parent once the nested voice is done (4096 frames).
  h.Steps(20);
  SND_CHECK(h.ends.Has(p, SND_END_COMPLETED));
  SND_CHECK_EQ(h.ends.CountOf(p), 1u);
}

SND_TEST(CoreSequence2, NestedEventsDeeperThanTheLimitDoNotPlay) {
  auto chain = [](int first) {  // E0 -> E1 -> ... -> E5 (asset): played from E<first>
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(48000, 0.2f), 0, 0});
    for (int i = 0; i < 6; ++i) {
      TEvent e = SimpleEvent(Name("E", i).c_str(), 0);
      if (i < 5) {
        e.tracks[0].clips[0].kind = 1;
        e.tracks[0].clips[0].target = static_cast<std::uint32_t>(i + 1);
      }
      b.events.push_back(e);
    }
    Harness* h = new Harness();
    h->Load("b", b.Build(HashId("B")));
    h->Clear();
    h->sys->Play(HashId(Name("E", first).c_str()), snd_emitter_handle{0}, nullptr);
    h->Steps(3);
    return h;
  };
  Harness* ok = chain(1);  // 4 levels of nesting: plays
  SND_CHECK_NEAR(Mean(*ok, 300, 700, 0), kCenter * 0.2f, 1e-4f);
  SND_CHECK(ok->ends.errors.empty());
  delete ok;
  Harness* deep = chain(0);  // 5 levels: nothing, with a warning
  SND_CHECK_NEAR(Mean(*deep, 300, 700, 0), 0.0f, 1e-7f);
  bool warned = false;
  for (snd_result r : deep->ends.errors) warned = warned || r == SND_ERR_LIMIT_REACHED;
  SND_CHECK(warned);
  delete deep;
}

// --- D-145 action tracks ---------------------------------------------------------------
namespace {
constexpr float kMsPerFrame = 1000.0f / 48000.0f;
// "P" holds a silent asset and the given actions; "K" is an impulse; "Long" a DC 0.3.
struct ActionBank {
  TestBank b = BasicBank();
  ActionBank() {
    b.assets.push_back(TAsset{HashId("silent"), 1, 48000, Constant(200000, 0.0f), 0, 0});
    b.assets.push_back(TAsset{HashId("imp"), 1, 48000, Impulse(64, 0, 1.0f), 0, 0});
    b.assets.push_back(TAsset{HashId("long"), 1, 48000, Constant(200000, 0.3f), 0, 0});
    b.events.push_back(SimpleEvent("P", 0));
    b.events.push_back(SimpleEvent("K", 1));
    b.events.push_back(SimpleEvent("Long", 2));
  }
  void Act(std::uint32_t frame, bf::ActionKind kind, const char* target, float value = 0.0f) {
    b.events[0].tracks[0].actions.push_back(
        TAction{static_cast<float>(frame) * kMsPerFrame, static_cast<std::uint32_t>(kind), HashId(target), value});
  }
};
// Frames of the impulse peaks (resampled impulse: a local maximum above 0.3).
std::vector<std::size_t> Peaks(const Harness& h) {
  std::vector<std::size_t> p;
  for (std::size_t f = 0; f + 1 < h.TotalFrames(); ++f) {
    const float v = h.At(f, 0);
    if (v > 0.3f && (f == 0 || v >= h.At(f - 1, 0)) && v > h.At(f + 1, 0)) p.push_back(f);
  }
  return p;
}
}  // namespace

// A Play action starts its event at the action's sample, the one at time 0 in the start
// block; actions are looked up one block ahead.
SND_TEST(CoreSequence2, ActionPlayStartsAtTheActionSample) {
  Harness h;
  ActionBank ab;
  ab.Act(0, bf::ActionKind::PlayEvent, "K");
  ab.Act(1000, bf::ActionKind::PlayEvent, "K");
  ab.Act(1999, bf::ActionKind::PlayEvent, "K");
  h.Load("b", ab.b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(12);
  const std::vector<std::size_t> peaks = Peaks(h);
  SND_REQUIRE_EQ(peaks.size(), std::size_t{3});
  SND_CHECK_EQ(peaks[0], std::size_t{0});
  SND_CHECK_EQ(peaks[1], std::size_t{1000});
  SND_CHECK_EQ(peaks[2], std::size_t{1999});
  SND_CHECK_EQ(h.ends.CountOf(p), 0u);
  // The children are one-shots: no end callback for them.
  h.Steps(4);
  SND_CHECK_EQ(h.ends.list.size(), std::size_t{0});
}

// Parameter actions write the owning playback's scope (ACTN has no scope column), in the
// block that contains their time; another playback is not affected.
SND_TEST(CoreSequence2, ActionSetParamWritesThePlaybackScope) {
  Harness h;
  ActionBank ab;
  ab.b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 0.5f), 0, 0});
  ab.b.events[0].tracks[0].clips[0].target = 3;  // P plays DC 0.5
  ab.b.params.push_back(TParam{HashId("Q"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  ab.b.curves.push_back(TCurve{HashId("Q"), static_cast<std::uint32_t>(bf::CurveTarget::Volume), 0, {{0.0f, 0.0f}, {1.0f, -20.0f}}});
  ab.b.events[0].curves = {0};
  TEvent other = SimpleEvent("O", 3);
  other.curves = {0};
  ab.b.events.push_back(other);
  ab.Act(0, bf::ActionKind::SetParam, "Q", 1.0f);
  ab.Act(1000, bf::ActionKind::SetParam, "Q", 0.0f);
  h.Load("b", ab.b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK_NEAR(h.At(100, 0), 0.05f * kCenter, 1e-4f);  // the start block
  SND_CHECK_NEAR(h.At(700, 0), 0.05f * kCenter, 1e-4f);
  // D-145: a parameter action applies from the first block starting at or after its time
  // (frame 1000 -> the block of 1024, ramping over it like snd_set_param): not before.
  SND_CHECK_NEAR(h.At(1000, 0), 0.05f * kCenter, 1e-4f);
  SND_CHECK_NEAR(h.At(1023, 0), 0.05f * kCenter, 1e-4f);
  SND_CHECK_NEAR(h.At(1300, 0), 0.5f * kCenter, 1e-4f);
  h.Clear();
  h.sys->Play(HashId("O"), snd_emitter_handle{0}, nullptr);  // Q stays at its default for O
  h.Steps(3);
  SND_CHECK(h.At(600, 0) > 0.5f * kCenter * 1.9f);  // P (0 dB again) + O (0 dB)
}

// A snapshot action takes its weight in the start block; a StopEvent action stops the
// target's playbacks in the block of its time.
SND_TEST(CoreSequence2, ActionSnapshotAndStopApplyInTheirBlock) {
  Harness h;
  ActionBank ab;
  ab.b.snapshots.push_back(TSnapshot{HashId("S"), {{0u, 0u, 0u, -20.0f}}});
  ab.Act(0, bf::ActionKind::ApplySnapshot, "S", 1.0f);
  ab.Act(600, bf::ActionKind::StopEvent, "Long");
  h.Load("b", ab.b.Build(HashId("B")));
  const snd_playback_handle lp = h.sys->Play(HashId("Long"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  h.Clear();
  h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK(h.At(0, 0) > 0.29f * kCenter);                       // the bus ramps from 0 dB ...
  SND_CHECK_NEAR(h.At(300, 0), 0.03f * kCenter, 1e-4f);           // ... to -20 dB within the start block
  SND_CHECK_NEAR(h.At(511, 0), 0.03f * kCenter, 1e-4f);           // before the stop's block
  SND_CHECK_NEAR(h.At(4 * 256, 0), 0.0f, 1e-6f);                  // stopped (short fade) in block 2
  SND_CHECK(h.ends.Has(lp, SND_END_STOPPED));
}

// Paused, the action time does not advance; stopped, the rest never fires.
SND_TEST(CoreSequence2, ActionsWaitWhilePausedAndNeverFireAfterAStop) {
  Harness h;
  ActionBank ab;
  ab.Act(2000, bf::ActionKind::PlayEvent, "K");
  h.Load("b", ab.b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  h.sys->Pause(p, 0.0f);
  h.Steps(20);
  SND_CHECK(Peaks(h).empty());
  h.sys->ResumePlayback(p, 0.0f);
  h.Steps(20);
  const std::vector<std::size_t> peaks = Peaks(h);
  SND_REQUIRE_EQ(peaks.size(), std::size_t{1});
  SND_CHECK(peaks[0] > 2000u + 18u * 256u);
  const snd_playback_handle q = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(2);
  h.sys->Stop(q, 0.0f, nullptr);
  h.Steps(20);
  SND_CHECK(Peaks(h).empty());
  SND_CHECK(h.ends.Has(q, SND_END_STOPPED));
}

// A play refused by its event's instance limit does not run the actions of its start block.
SND_TEST(CoreSequence2, ActionsOfARefusedPlayAreCancelled) {
  Harness h;
  ActionBank ab;
  ab.b.events[0].maxInstances = 1;
  ab.Act(0, bf::ActionKind::PlayEvent, "K");
  ab.Act(100, bf::ActionKind::PlayEvent, "K");
  h.Load("b", ab.b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle a = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  const snd_playback_handle b2 = h.sys->Play(HashId("P"), snd_emitter_handle{0}, nullptr);
  h.Steps(6);
  SND_CHECK(h.ends.Has(b2, SND_END_REJECTED, SND_END_DETAIL_EVENT_LIMIT));
  SND_CHECK_EQ(h.ends.CountOf(a), 0u);
  const std::vector<std::size_t> peaks = Peaks(h);
  SND_REQUIRE_EQ(peaks.size(), std::size_t{2});
  SND_CHECK_EQ(peaks[0], std::size_t{0});
  SND_CHECK_EQ(peaks[1], std::size_t{100});
  SND_CHECK(h.At(0, 0) < 1.0f);  // one impulse at frame 0, not two
}

// A scheduled play (CLOCK_SAMPLE): its first block's actions come at most one block late;
// the later ones are sample accurate.
SND_TEST(CoreSequence2, ActionsOfAScheduledPlay) {
  Harness h;
  ActionBank ab;
  ab.Act(0, bf::ActionKind::PlayEvent, "K");
  ab.Act(1000, bf::ActionKind::PlayEvent, "K");
  h.Load("b", ab.b.Build(HashId("B")));
  h.Steps(1);
  h.sys->Update();  // the stats of the block just rendered: its end is the next block's start
  snd_stats st{};
  h.sys->GetStats(&st);
  h.Clear();
  snd_when w{};
  w.kind = SND_WHEN_CLOCK_SAMPLE;
  w.value = st.clock_sample + 256 + 100;  // inside the second block
  h.sys->Play(HashId("P"), snd_emitter_handle{0}, &w);
  h.Steps(10);
  const std::vector<std::size_t> peaks = Peaks(h);
  SND_REQUIRE_EQ(peaks.size(), std::size_t{2});
  SND_CHECK(peaks[0] >= 356u && peaks[0] <= 356u + 256u);
  SND_CHECK_EQ(peaks[1], std::size_t{1356});
}

// --- D-146 fade-in of a play ---------------------------------------------------------------
SND_TEST(CoreSequence2, PlayFadeRampsLinearlyFromZero) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("F", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle p = h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, 0.1f);  // 4800 frames
  SND_CHECK(p.value != 0u);
  h.Steps(24);
  // Linear from 0 (the playback fade moves per block: exact at whole blocks; the last
  // partial block of 4800 / 256 = 18.75 reaches 1 at the block's end).
  for (std::size_t f : {std::size_t{100}, std::size_t{1200}, std::size_t{2400}, std::size_t{3600}, std::size_t{4500}}) {
    SND_CHECK_NEAR(h.At(f, 0), 0.5f * kCenter * float(f) / 4800.0f, 2e-4f);
  }
  SND_CHECK_NEAR(h.At(0, 0), 0.0f, 1e-7f);
  for (std::size_t f = 32; f < 4800; ++f) SND_CHECK(h.At(f, 0) >= h.At(f - 1, 0));  // (resampler onset: 32)
  SND_CHECK_NEAR(h.At(5200, 0), 0.5f * kCenter, 1e-5f);
  // Invalid fades.
  SND_CHECK_EQ(h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, -1.0f).value, 0u);
  SND_CHECK_EQ(h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, std::nanf("")).value, 0u);
}

// Stopping during the fade-in fades out from the level reached (never above it).
SND_TEST(CoreSequence2, StopDuringTheFadeInFadesFromTheCurrentLevel) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("F", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  const snd_playback_handle p = h.sys->PlayFade(HashId("F"), snd_emitter_handle{0}, nullptr, 0.2f);
  h.Steps(10);
  const float reached = h.At(h.TotalFrames() - 1, 0);
  SND_CHECK_NEAR(reached, 0.5f * kCenter * 2560.0f / 9600.0f, 2e-4f);
  h.sys->Stop(p, 0.05f, nullptr);
  const std::size_t from = h.TotalFrames();
  h.Steps(16);
  // (A block's gain ramp ends one sample before its end value: 1/256 of a block's step.)
  for (std::size_t f = from; f < h.TotalFrames(); ++f) {
    SND_CHECK(h.At(f, 0) <= reached + 5e-5f);
    SND_CHECK(h.At(f, 0) <= h.At(f - 1, 0) + 5e-5f);
  }
  SND_CHECK(h.ends.Has(p, SND_END_STOPPED));
}
