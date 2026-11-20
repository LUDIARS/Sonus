// Independent tests, stage 3 group 4: sequence kinds (4.2, D-141 .. D-145), N loops,
// clip fade-out, layers, nested events, action tracks. See indep_g4_fixture.hpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>

#include "indep_g4_fixture.hpp"

using namespace g4;

namespace {

// Clip i of a selection event: a 64-frame constant asset of level (i + 1) / kLevels.
constexpr float kLevels = 512.0f;
float LevelOf(std::uint32_t i) { return (float(i) + 1.0f) / kLevels; }

void AddSelectionEvent(B& bk, const char* name, std::uint32_t clips, std::uint32_t sequence,
                       const std::vector<float>& weights = {}, float comboMs = 0) {
  std::vector<TClip> cl;
  for (std::uint32_t i = 0; i < clips; ++i) {
    char an[48];
    std::snprintf(an, sizeof an, "%s.a%u", name, i);
    const std::uint32_t a = bk.Asset(an, Constant(64, LevelOf(i)));
    TClip c = B::AssetClip(a);
    if (i < weights.size()) c.weight = weights[i];
    cl.push_back(c);
  }
  TEvent& e = bk.Event(name, cl, sequence);
  e.comboWindowMs = comboMs;
}

// Plays `event` once, renders 2 blocks and returns the index of the clip heard (-1: none,
// -2: not a clip level).
int PlayAndIdentify(Rig& r, Id event) {
  const std::size_t from = r.Now();
  r.sys->Play(event, NoEm(), nullptr);
  r.Steps(2);
  // The middle of the 64-frame clip (the resampler rings at the edges).
  const std::size_t first = r.FirstAbove(from, r.Now(), 0.25f / kLevels);
  if (first + 32 >= r.Now()) return -1;
  const float m = std::fabs(r.At(first + 32, 0)) / kC;
  const float idx = m * kLevels - 1.0f;
  const float rounded = std::round(idx);
  if (std::fabs(idx - rounded) > 0.05f) return -2;
  return static_cast<int>(rounded);
}

}  // namespace

// Calibration of the fixture: a 64-frame asset at pitch 1 comes out exactly (centre pan),
// starting at the block start, once.
SND_TEST(IndepG4Calib, AssetComesOutExactlyAtPitchOne) {
  Rig r;
  B bk;
  std::vector<float> ramp(1000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(i + 1) / 1024.0f;
  bk.Asset("ramp", ramp);
  bk.Event("E", {B::AssetClip(0)});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("E"), NoEm(), nullptr);
  r.Steps(8);
  // Interior frames exact (the resampler rings a few frames at the material's edges).
  std::size_t bad = 0;
  float edge = 0;
  for (std::size_t i = 16; i < 984; ++i) bad += std::fabs(r.At(t0 + i) - ramp[i] * kC) > 1e-4f ? 1u : 0u;
  for (std::size_t i = 0; i < 1000; ++i) edge = std::max(edge, std::fabs(r.At(t0 + i) - ramp[i] * kC));
  snd::test::Note("max |error| incl. edges %g; tail frames above 1e-3: %llu", double(edge),
                  static_cast<unsigned long long>(r.CountAbove(t0 + 1000, r.Now(), 1e-3f)));
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK_EQ(r.CountAbove(t0 + 1024, r.Now(), 1e-7f), 0u);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_COMPLETED));
}

// ---------------------------------------------------------------------------
// Random (weighted), RandomNoRepeat, Sequential, Shuffle, Combo
// ---------------------------------------------------------------------------

// B1: weights 0, 1, 2, 3, 4 -> shares 0, .1, .2, .3, .4 (chi-square, 3 dof, p = 0.001).
SND_TEST(IndepG4Seq, WeightedRandomFollowsTheWeights) {
  Rig r;
  B bk;
  AddSelectionEvent(bk, "R", 5, 1, {0.0f, 1.0f, 2.0f, 3.0f, 4.0f});
  r.Load("b", bk.Build("B"));
  const int n = 3000;
  int count[5] = {0, 0, 0, 0, 0};
  int other = 0;
  for (int i = 0; i < n; ++i) {
    const int k = PlayAndIdentify(r, HashId("R"));
    if (k >= 0 && k < 5) {
      ++count[k];
    } else {
      ++other;
    }
  }
  snd::test::Note("counts %d %d %d %d %d other %d", count[0], count[1], count[2], count[3], count[4], other);
  SND_CHECK_EQ(other, 0);
  SND_CHECK_EQ(count[0], 0);  // weight 0 is never chosen
  double chi = 0;
  for (int k = 1; k < 5; ++k) {
    const double e = n * k / 10.0;
    chi += (count[k] - e) * (count[k] - e) / e;
  }
  snd::test::Note("chi2 = %.2f", chi);
  SND_CHECK(chi < 16.27);
}

// B2: RandomNoRepeat never repeats the previous clip (2 and 5 clips); one clip always plays.
SND_TEST(IndepG4Seq, RandomNoRepeatNeverRepeatsAndOneClipAlwaysPlays) {
  Rig r;
  B bk;
  AddSelectionEvent(bk, "N2", 2, 2);
  AddSelectionEvent(bk, "N5", 5, 2);
  AddSelectionEvent(bk, "N1", 1, 2);
  r.Load("b", bk.Build("B"));
  for (const char* name : {"N2", "N5"}) {
    int prev = -1, repeats = 0, bad = 0;
    std::set<int> seen;
    for (int i = 0; i < 300; ++i) {
      const int k = PlayAndIdentify(r, HashId(name));
      if (k < 0) ++bad;
      if (k == prev) ++repeats;
      seen.insert(k);
      prev = k;
    }
    SND_CHECK_EQ(bad, 0);
    SND_CHECK_EQ(repeats, 0);
    SND_CHECK_EQ(seen.size(), std::string(name) == "N2" ? 2u : 5u);
  }
  for (int i = 0; i < 20; ++i) SND_CHECK_EQ(PlayAndIdentify(r, HashId("N1")), 0);
}

// B5: Sequential: 0, 1, 2, 0, 1, 2, 0.
SND_TEST(IndepG4Seq, SequentialCyclesInOrder) {
  Rig r;
  B bk;
  AddSelectionEvent(bk, "S", 3, 3);
  r.Load("b", bk.Build("B"));
  for (int i = 0; i < 10; ++i) SND_CHECK_EQ(PlayAndIdentify(r, HashId("S")), i % 3);
}

namespace {
// Shuffle of `clips` clips over `rounds` rounds: every round is a permutation and the
// boundary never repeats (D-141). Returns the sequence.
// interleave: another playback (of a silent event) before every call, so the playbacks of
// the shuffle event get other handles (other playback seeds).
std::vector<int> ShuffleRun(std::uint32_t clips, int rounds, const char* name = "SH", bool interleave = false) {
  Rig r(Config());
  B bk;
  AddSelectionEvent(bk, name, clips, 4);
  bk.Event("Silent", {B::SynthClip(bk.Synth(0.0f))});
  r.Load("b", bk.Build("B"));
  std::vector<int> seq;
  for (int i = 0; i < int(clips) * rounds; ++i) {
    if (interleave) {
      const snd_playback_handle s = r.sys->Play(HashId("Silent"), NoEm(), nullptr);
      r.sys->Stop(s, 0.0f, nullptr);
    }
    seq.push_back(PlayAndIdentify(r, HashId(name)));
  }
  return seq;
}
void CheckShuffle(const std::vector<int>& seq, std::uint32_t clips, int rounds) {
  int notPerm = 0, boundaryRepeats = 0, bad = 0;
  for (int k = 0; k < rounds; ++k) {
    std::vector<int> round(seq.begin() + k * clips, seq.begin() + (k + 1) * clips);
    for (int v : round) bad += (v < 0 || v >= int(clips)) ? 1 : 0;
    std::sort(round.begin(), round.end());
    for (std::uint32_t i = 0; i < clips; ++i) notPerm += round[i] != int(i) ? 1 : 0;
    if (k > 0 && clips >= 2u && seq[k * clips] == seq[k * clips - 1]) ++boundaryRepeats;
  }
  snd::test::Note("shuffle %u clips: bad %d, not-permutation entries %d, boundary repeats %d", clips, bad, notPerm,
                  boundaryRepeats);
  SND_CHECK_EQ(bad, 0);
  SND_CHECK_EQ(notPerm, 0);
  SND_CHECK_EQ(boundaryRepeats, 0);
}
}  // namespace

// B3: 1, 2, 3, 7 clips: each round a permutation, no repeat at the boundary.
SND_TEST(IndepG4Seq, ShuffleIsAPermutationPerRoundForSmallCounts) {
  for (std::uint32_t n : {1u, 2u, 3u, 7u}) {
    const int rounds = n == 1u ? 5 : 40;
    CheckShuffle(ShuffleRun(n, rounds), n, rounds);
  }
}

// B3: 300 clips (no upper bound on the clip count, D-141): 3 rounds.
SND_TEST(IndepG4Seq, ShuffleOfThreeHundredClips) {
  const std::vector<int> seq = ShuffleRun(300, 3);
  CheckShuffle(seq, 300, 3);
  // Not the identity order (it is shuffled).
  int inOrder = 0;
  for (int i = 0; i < 300; ++i) inOrder += seq[i] == i ? 1 : 0;
  SND_CHECK(inOrder < 30);
}

// B4: the same script gives the same order; another event (another seed) another order.
SND_TEST(IndepG4Seq, ShuffleIsDeterministicPerSeed) {
  const std::vector<int> a = ShuffleRun(20, 3, "SH");
  const std::vector<int> b = ShuffleRun(20, 3, "SH");
  SND_CHECK(a == b);
  const std::vector<int> c = ShuffleRun(20, 3, "SH_other");
  const std::vector<int> d = ShuffleRun(20, 3, "SH", true);
  snd::test::Note("other event id: %s; other playback handles: %s", a == c ? "same order" : "other order",
                  a == d ? "same order" : "other order");
  SND_CHECK(a != c || a != d);
  CheckShuffle(c, 20, 3);
  CheckShuffle(d, 20, 3);
}

// B4 for Random: the same script, the same choices; another event id, other choices.
SND_TEST(IndepG4Seq, RandomIsDeterministicPerSeed) {
  auto run = [](const char* name, bool interleave) {
    Rig r;
    B bk;
    AddSelectionEvent(bk, name, 8, 1);
    bk.Event("Silent", {B::SynthClip(bk.Synth(0.0f))});
    r.Load("b", bk.Build("B"));
    std::vector<int> seq;
    for (int i = 0; i < 64; ++i) {
      if (interleave) {
        const snd_playback_handle s = r.sys->Play(HashId("Silent"), NoEm(), nullptr);
        r.sys->Stop(s, 0.0f, nullptr);
      }
      seq.push_back(PlayAndIdentify(r, HashId(name)));
    }
    return seq;
  };
  const std::vector<int> a = run("RA", false), b = run("RA", false), c = run("RB", false), d = run("RA", true);
  SND_CHECK(a == b);
  snd::test::Note("other event id: %s; other playback handles: %s", a == c ? "same choices" : "other choices",
                  a == d ? "same choices" : "other choices");
  SND_CHECK(a != d);  // the seed is the playback's (2.5)
}

// B6: Combo: calls within comboWindowMs advance, a pause longer than the window restarts.
SND_TEST(IndepG4Seq, ComboAdvancesInsideTheWindowAndRestartsAfter) {
  Rig r;
  B bk;
  AddSelectionEvent(bk, "C", 3, 6, {}, 100.0f);  // 100 ms = 4800 frames = 18.75 blocks
  r.Load("b", bk.Build("B"));
  // Every call 2 blocks apart (PlayAndIdentify renders 2 blocks).
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 0);
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 1);
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 2);
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 0);  // wraps like Sequential
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 1);
  r.Steps(30);  // 32 blocks after the last call > 100 ms
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 0);
  r.Steps(10);  // 12 blocks (64 ms) < 100 ms: still inside
  SND_CHECK_EQ(PlayAndIdentify(r, HashId("C")), 1);
}

// ---------------------------------------------------------------------------
// Clip random ranges (4.2)
// ---------------------------------------------------------------------------

// B8: volume within +-randVolumeDb, both directions used, playbacks differ.
SND_TEST(IndepG4Rand, VolumeStaysInsideTheRange) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(2400, 0.5f)));
  c.randVolumeDb = 6.0f;
  bk.Event("V", {c});
  r.Load("b", bk.Build("B"));
  double lo = 1e9, hi = -1e9;
  int outside = 0;
  std::set<int> distinct;
  for (int i = 0; i < 200; ++i) {
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("V"), NoEm(), nullptr);
    r.Steps(11);
    const double db = 20.0 * std::log10(std::fabs(r.At(t0 + 1200)) / (0.5 * kC));
    lo = std::min(lo, db);
    hi = std::max(hi, db);
    outside += std::fabs(db) > 6.01 ? 1 : 0;
    distinct.insert(int(std::lround(db * 100)));
  }
  snd::test::Note("volume offsets [%.2f, %.2f] dB, %llu distinct", lo, hi, static_cast<unsigned long long>(distinct.size()));
  SND_CHECK_EQ(outside, 0);
  SND_CHECK(distinct.size() > 50u);
  SND_CHECK(lo < -3.0 && hi > 3.0);  // +- range (both directions)
}

// B8: pitch within +-randPitchCents: the output length of a 4800-frame clip is 4800 / ratio.
SND_TEST(IndepG4Rand, PitchStaysInsideTheRange) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(4800, 0.5f)));
  c.randPitchCents = 1200.0f;
  bk.Event("P", {c});
  r.Load("b", bk.Build("B"));
  double lo = 1e9, hi = -1e9;
  int outside = 0;
  for (int i = 0; i < 100; ++i) {
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(44);
    const double len = double(r.CountAbove(t0, r.Now(), 0.25f * kC));
    const double cents = 1200.0 * std::log2(4800.0 / len);
    lo = std::min(lo, cents);
    hi = std::max(hi, cents);
    outside += std::fabs(cents) > 1203.0 ? 1 : 0;
  }
  snd::test::Note("pitch offsets [%.1f, %.1f] cents", lo, hi);
  SND_CHECK_EQ(outside, 0);
  SND_CHECK(lo < -600.0 && hi > 600.0);
}

// B8: pan within +-randPan (constant power: L = cos, R = sin of (p + 1) * pi / 4).
SND_TEST(IndepG4Rand, PanStaysInsideTheRange) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(2400, 0.5f)));
  c.randPan = 0.5f;
  bk.Event("N", {c});
  r.Load("b", bk.Build("B"));
  double lo = 1e9, hi = -1e9;
  int outside = 0, powerOff = 0;
  for (int i = 0; i < 200; ++i) {
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("N"), NoEm(), nullptr);
    r.Steps(11);
    const double L = r.At(t0 + 1200, 0), R = r.At(t0 + 1200, 1);
    const double p = std::atan2(R, L) / (3.14159265358979 / 4.0) - 1.0;
    powerOff += std::fabs(L * L + R * R - 0.25) > 0.01 ? 1 : 0;
    lo = std::min(lo, p);
    hi = std::max(hi, p);
    outside += std::fabs(p) > 0.501 ? 1 : 0;
  }
  snd::test::Note("pan [%.3f, %.3f], power off %d", lo, hi, powerOff);
  SND_CHECK_EQ(outside, 0);
  SND_CHECK(lo < -0.25 && hi > 0.25);
}

// B8: start position within [0, randStartMs]: the first value of a ramp reveals it.
SND_TEST(IndepG4Rand, StartPositionStaysInsideTheRange) {
  Rig r;
  B bk;
  std::vector<float> ramp(4800);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = 0.25f + 0.5f * float(i) / 4800.0f;
  TClip c = B::AssetClip(bk.Asset("a", ramp));
  c.randStartMs = 10.0f;  // 480 frames
  bk.Event("S", {c});
  r.Load("b", bk.Build("B"));
  double lo = 1e9, hi = -1e9;
  int outside = 0;
  for (int i = 0; i < 100; ++i) {
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("S"), NoEm(), nullptr);
    r.Steps(20);
    const double v = r.At(t0 + 32) / kC;
    const double start = (v - 0.25) / 0.5 * 4800.0 - 32.0;
    lo = std::min(lo, start);
    hi = std::max(hi, start);
    outside += (start < -0.5 || start > 480.5) ? 1 : 0;
  }
  snd::test::Note("start frames [%.1f, %.1f]", lo, hi);
  SND_CHECK_EQ(outside, 0);
  SND_CHECK(hi - lo > 200.0);
}

// ---------------------------------------------------------------------------
// N loops (D-142) and the clip fade-out
// ---------------------------------------------------------------------------
namespace {
struct LoopRun {
  std::size_t t0 = 0, end = 0, count = 0;
  std::uint32_t ends = 0;
  snd_end_reason reason = 0;
  float minInside = 0;
};
// Constant 0.5 asset of `frames`, loopCount `n`, pitch `cents`; renders `blocks`.
LoopRun RunLoop(std::uint64_t frames, std::uint32_t n, float cents = 0, std::uint32_t blocks = 80,
                std::uint64_t loopStart = 0, std::uint64_t loopEnd = 0) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(frames, 0.5f), loopStart, loopEnd));
  c.loopCount = n;
  c.pitchCents = cents;
  bk.Event("L", {c});
  r.Load("b", bk.Build("B"));
  LoopRun lr;
  lr.t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("L"), NoEm(), nullptr);
  r.Steps(blocks);
  lr.end = r.EndAbove(lr.t0, r.Now(), 0.25f * kC) - lr.t0;
  lr.count = r.CountAbove(lr.t0, r.Now(), 0.25f * kC);
  lr.ends = r.CountEnds(h.value);
  const End* e = r.EndOf(h.value);
  lr.reason = e != nullptr ? e->reason : 0;
  lr.minInside = 1e9f;
  for (std::size_t f = lr.t0 + 16; f + 16 < lr.t0 + lr.end; ++f) lr.minInside = std::min(lr.minInside, std::fabs(r.At(f)));
  return lr;
}
}  // namespace

// loopCount 1 = once, 3 = three times without a gap, ends exactly at 3 x length (+-1 frame).
SND_TEST(IndepG4Loop, NLoopsEndAtNTimesTheLength) {
  const LoopRun one = RunLoop(1000, 1);
  SND_CHECK(one.end >= 999 && one.end <= 1001);
  SND_CHECK_EQ(one.ends, 1u);
  SND_CHECK_EQ(one.reason, SND_END_COMPLETED);
  const LoopRun three = RunLoop(1000, 3);
  snd::test::Note("loopCount 3: end %llu count %llu min %g", static_cast<unsigned long long>(three.end), static_cast<unsigned long long>(three.count), double(three.minInside));
  SND_CHECK(three.end >= 2999 && three.end <= 3001);
  SND_CHECK(three.count >= 2998 && three.count <= 3001);
  SND_CHECK(three.minInside > 0.45f * kC);
  SND_CHECK_EQ(three.ends, 1u);
  SND_CHECK_EQ(three.reason, SND_END_COMPLETED);
}

// Loop end exactly on a block boundary (256 x 2 = 512) and over many blocks (100 x 7).
SND_TEST(IndepG4Loop, NLoopsAtTheBlockBoundaryAndShortClips) {
  const LoopRun b = RunLoop(256, 2);
  snd::test::Note("256 x 2: end %llu", static_cast<unsigned long long>(b.end));
  SND_CHECK(b.end >= 511 && b.end <= 513);
  SND_CHECK_EQ(b.ends, 1u);
  const LoopRun s = RunLoop(100, 7);
  snd::test::Note("100 x 7: end %llu", static_cast<unsigned long long>(s.end));
  SND_CHECK(s.end >= 699 && s.end <= 701);
  SND_CHECK_EQ(s.ends, 1u);
  const LoopRun big = RunLoop(1000, 40, 0, 200);
  SND_CHECK(big.end >= 39999 && big.end <= 40001);
}

// loopCount 0 = endless.
SND_TEST(IndepG4Loop, LoopCountZeroIsEndless) {
  const LoopRun z = RunLoop(1000, 0, 0, 120);
  SND_CHECK_EQ(z.ends, 0u);
  SND_CHECK(z.end >= 120u * 256u - 2u);
}

// Pitch 2 (1200 cents): 3 loops of 1000 frames take 1500 output frames.
SND_TEST(IndepG4Loop, NLoopsWithPitch) {
  const LoopRun p = RunLoop(1000, 3, 1200.0f);
  snd::test::Note("pitch 2: end %llu", static_cast<unsigned long long>(p.end));
  SND_CHECK(p.end >= 1498 && p.end <= 1502);
  SND_CHECK_EQ(p.ends, 1u);
  const LoopRun q = RunLoop(1000, 3, -1200.0f);
  snd::test::Note("pitch 0.5: end %llu", static_cast<unsigned long long>(q.end));
  SND_CHECK(q.end >= 5996 && q.end <= 6004);
}

// Loop points [200, 800) with 3 loops: records what is played (the spec does not say
// whether the material after loopEnd follows the last loop).
SND_TEST(IndepG4Loop, SpecHole_NLoopsWithLoopPoints) {
  const LoopRun p = RunLoop(1000, 3, 0, 80, 200, 800);
  snd::test::Note("loop points [200, 800) x 3: end %llu, count %llu (2200 = tail played after the last loop, "
                  "2000 = stops at loopEnd)", static_cast<unsigned long long>(p.end), static_cast<unsigned long long>(p.count));
  SND_CHECK_EQ(p.ends, 1u);
  SND_CHECK(p.end >= 1999 && p.end <= 2201);
}

// A pause in the middle does not change the number of loops: the played frames stay 3 x length.
SND_TEST(IndepG4Loop, NLoopsAcrossAPause) {
  Rig r;
  B bk;
  std::vector<float> ramp(1000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = 0.3f + 0.4f * float(i) / 1000.0f;
  TClip c = B::AssetClip(bk.Asset("a", ramp));
  c.loopCount = 3;
  bk.Event("L", {c});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("L"), NoEm(), nullptr);
  r.Steps(6);  // 1536 frames: inside the second loop
  r.sys->Pause(h, 0.0f);
  r.Steps(10);
  const std::size_t resumed = r.Now();
  r.sys->ResumePlayback(h, 0.0f);
  r.Steps(30);
  const std::size_t end = r.EndAbove(t0, r.Now(), 0.1f);
  // The material after the resume is the continuation: the last frame heard is the clip's last
  // frame (value 0.6996) and the frames heard after the resume plus before the pause make 3000.
  const std::size_t heard = r.CountAbove(t0, r.Now(), 0.1f);
  snd::test::Note("end %llu (resume at %llu), heard %llu, last value %g", static_cast<unsigned long long>(end - t0), static_cast<unsigned long long>(resumed - t0), static_cast<unsigned long long>(heard),
                  double(r.At(end - 4) / kC));
  SND_CHECK(heard >= 3000 - 2 * 240 && heard <= 3000 + 2 * 240);
  SND_CHECK_NEAR(r.At(end - 8) / kC, 0.3f + 0.4f * 991.0f / 1000.0f, 0.01f);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_COMPLETED));
}

// A virtual phase in the middle: the voice keeps time (4.4) and the loops still end after
// 3 x length on the clock (the virtual part counts its loops too).
SND_TEST(IndepG4Loop, NLoopsAcrossAVirtualPhase) {
  Rig r(Config(1));
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(4800, 0.5f)));
  c.loopCount = 3;
  bk.Event("L", {c}).priority = 100;
  bk.Event("H", {B::SynthClip(bk.Synth(300.0f))}).priority = 200;  // DC 0.3
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("L"), NoEm(), nullptr);
  r.Steps(4);
  const snd_playback_handle hi = r.sys->Play(HashId("H"), NoEm(), nullptr);
  r.Steps(20);  // L virtual through its first loop boundary (4800 = 18.75 blocks)
  r.sys->Stop(hi, 0.0f, nullptr);
  r.Steps(60);
  const End* e = r.EndOf(h.value);
  SND_REQUIRE(e != nullptr);
  const std::size_t endHeard = r.EndAbove(t0, r.Now(), 0.4f * kC);
  snd::test::Note("ended at block %llu (clock %llu); last loud frame %llu; expected 14400",
                  static_cast<unsigned long long>(e->block), static_cast<unsigned long long>(std::size_t(e->block) * 256u - t0), static_cast<unsigned long long>(endHeard - t0));
  // +-24: the resampler's look-ahead (16 source frames read ahead of the output) is lost or
  // kept across the virtual phase; the loops themselves are not lost or added (+-4800).
  SND_CHECK(endHeard - t0 >= 14400 - 24 && endHeard - t0 <= 14400 + 24);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK_EQ(e->reason, SND_END_COMPLETED);
}

// CLIP.fadeOutMs: full level, then down to 0 at the clip's end, monotone.
SND_TEST(IndepG4Loop, ClipFadeOutEndsAtTheClipEnd) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(4800, 1.0f)));
  c.fadeOutMs = 50.0f;  // 2400 frames
  bk.Event("F", {c});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("F"), NoEm(), nullptr);
  r.Steps(24);
  // Coarse properties here (one block of slack); sample alignment: Defect_ClipFadeOutIsNotSampleAligned.
  SND_CHECK_NEAR(r.At(t0 + 1000) / kC, 1.0f, 1e-3f);
  SND_CHECK_NEAR(r.At(t0 + 2300) / kC, 1.0f, 0.01f);
  int rises = 0;
  for (std::size_t f = t0 + 2300; f < t0 + 4790; ++f) rises += r.At(f + 1) > r.At(f) + 1e-3f ? 1 : 0;
  SND_CHECK_EQ(rises, 0);
  SND_CHECK(std::fabs(r.At(t0 + 4790)) / kC < 0.05f);
  SND_CHECK(r.EndAbove(t0, r.Now(), 1e-3f) - t0 <= 4810);
  snd::test::Note("fade-out midpoint %g (linear: 0.5)", double(r.At(t0 + 3600) / kC));
  SND_CHECK(std::fabs(r.At(t0 + 3600)) / kC < 0.9f);
}

// fadeOutMs longer than the material: decreasing over the whole clip, silent at the end.
SND_TEST(IndepG4Loop, ClipFadeOutLongerThanTheMaterial) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(4800, 1.0f)));
  c.fadeOutMs = 200.0f;  // 9600 frames
  bk.Event("F", {c});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("F"), NoEm(), nullptr);
  r.Steps(24);
  int rises = 0;
  for (std::size_t f = t0 + 16; f < t0 + 4790; ++f) rises += r.At(f + 1) > r.At(f) + 1e-3f ? 1 : 0;
  snd::test::Note("start level %g, middle %g", double(r.At(t0 + 16) / kC), double(r.At(t0 + 2400) / kC));
  SND_CHECK_EQ(rises, 0);
  SND_CHECK(r.At(t0 + 2400) < r.At(t0 + 16));
  SND_CHECK(std::fabs(r.At(t0 + 4790)) / kC < 0.02f);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
}

// fadeOutMs with 3 loops: only the end of the last loop fades.
SND_TEST(IndepG4Loop, ClipFadeOutWithNLoopsFadesTheLastLoopOnly) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(2400, 1.0f)));
  c.fadeOutMs = 25.0f;  // 1200 frames
  c.loopCount = 3;
  bk.Event("F", {c});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("F"), NoEm(), nullptr);
  r.Steps(32);
  float minFull = 1e9f;
  for (std::size_t f = t0 + 16; f < t0 + 5880; ++f) minFull = std::min(minFull, r.At(f) / kC);  // one block of slack
  snd::test::Note("min level before the last 1200 frames %g; level at 6600 %g", double(minFull),
                  double(r.At(t0 + 6600) / kC));
  SND_CHECK(minFull > 0.99f);
  SND_CHECK(r.At(t0 + 6600) / kC < 0.9f);
  SND_CHECK(std::fabs(r.At(t0 + 7190)) / kC < 0.05f);
  SND_CHECK(r.EndAbove(t0, r.Now(), 1e-3f) - t0 <= 7210);
}

// DEFECT: the clip fade-out (CLIP.fadeOutMs) is evaluated per block: it starts up to a block
// before `clip end - fadeOutMs` and has not reached 0 at the clip's end (the clip then stops
// with a step of about fade slope x the rest of the block). Expected: the fade runs over the
// clip's last fadeOutMs frames and reaches 0 at the last frame (a fade towards the end).
SND_TEST(IndepG4Loop, Defect_ClipFadeOutIsNotSampleAligned) {
  Rig r;
  B bk;
  TClip c = B::AssetClip(bk.Asset("a", Constant(4800, 1.0f)));
  c.fadeOutMs = 50.0f;  // frames [2400, 4800)
  bk.Event("F", {c});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("F"), NoEm(), nullptr);
  r.Steps(24);
  const float before = r.At(t0 + 2390) / kC;  // 10 frames before the fade
  const float last = r.At(t0 + 4795) / kC;    // 5 frames before the end: linear 5 / 2400
  snd::test::Note("level 10 frames before the fade %g (expected 1), 5 frames before the end %g (expected 0.002)",
                  double(before), double(last));
  SND_CHECK(before > 0.995f);
  SND_CHECK(std::fabs(last) < 0.01f);
}

// ---------------------------------------------------------------------------
// Layers with parameter crossfade (D-143)
// ---------------------------------------------------------------------------
namespace {
// Event "<name>": LayerCrossfade on parameter "L"; clips are endless DC synths.
struct Layer {
  float level, lo, hi;
};
void AddLayerEvent(B& bk, const char* name, const std::vector<Layer>& layers) {
  std::vector<TClip> cl;
  for (const Layer& l : layers) {
    TClip c = B::SynthClip(bk.Synth(l.level * 1000.0f));
    c.layerMin = l.lo;
    c.layerMax = l.hi;
    cl.push_back(c);
  }
  TEvent& e = bk.Event(name, cl, 7);
  e.tracks[0].layerParam = HashId("L");
}
void AddLayerParam(B& bk) {
  TParam p;
  p.id = HashId("L");
  p.minValue = 0;
  p.maxValue = 1;
  bk.b.params.push_back(p);
}
// Level of event `name` with L = v (Global), in units of the clips' DC (0 dB = clip level).
float LayerLevel(Rig& r, const char* name, float v) {
  r.sys->SetParamGlobal(HashId("L"), v);
  const snd_playback_handle h = r.sys->Play(HashId(name), NoEm(), nullptr);
  r.Steps(4);
  const float m = r.Mean(r.Now() - 256, r.Now()) / kC;
  r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(3);
  return m;
}
}  // namespace

// Two layers A [0, 0.6], B [0.4, 1]: inside alone 1, outside 0, overlap equal power, edges exact.
SND_TEST(IndepG4Layer, TwoLayersEqualPowerInTheOverlap) {
  Rig r;
  B bk;
  AddLayerParam(bk);
  AddLayerEvent(bk, "OnlyA", {{0.5f, 0.0f, 0.6f}, {0.0f, 0.4f, 1.0f}});
  AddLayerEvent(bk, "OnlyB", {{0.0f, 0.0f, 0.6f}, {0.5f, 0.4f, 1.0f}});
  r.Load("b", bk.Build("B"));
  struct P {
    float v, a, b;
  };
  const float s = 0.70710678f;
  const P pts[] = {{0.0f, 1, 0}, {0.2f, 1, 0}, {0.39f, 1, 0}, {0.4f, 1, 0}, {0.5f, s, s}, {0.6f, 0, 1},
                   {0.61f, 0, 1}, {0.8f, 0, 1}, {1.0f, 0, 1}};
  for (const P& p : pts) {
    const float ga = LayerLevel(r, "OnlyA", p.v) / 0.5f, gb = LayerLevel(r, "OnlyB", p.v) / 0.5f;
    snd::test::Note("v %.2f: gA %.4f gB %.4f (sum of squares %.4f)", double(p.v), double(ga), double(gb),
                    double(ga * ga + gb * gb));
    SND_CHECK_NEAR(ga, p.a, 0.01f);
    SND_CHECK_NEAR(gb, p.b, 0.01f);
  }
  for (float v : {0.42f, 0.45f, 0.5f, 0.55f, 0.58f}) {
    const float ga = LayerLevel(r, "OnlyA", v) / 0.5f, gb = LayerLevel(r, "OnlyB", v) / 0.5f;
    SND_CHECK_NEAR(ga * ga + gb * gb, 1.0f, 0.02f);
    SND_CHECK(ga > 0.0f && gb > 0.0f);
  }
  // Monotone through the overlap.
  float prevA = 2, prevB = -1;
  for (int i = 0; i <= 10; ++i) {
    const float v = 0.4f + 0.02f * float(i);
    const float ga = LayerLevel(r, "OnlyA", v), gb = LayerLevel(r, "OnlyB", v);
    SND_CHECK(ga <= prevA + 1e-4f);
    SND_CHECK(gb >= prevB - 1e-4f);
    prevA = ga;
    prevB = gb;
  }
}

// A value outside every layer: silence.
SND_TEST(IndepG4Layer, OutsideEveryLayerIsSilent) {
  Rig r;
  B bk;
  AddLayerParam(bk);
  AddLayerEvent(bk, "G", {{0.5f, 0.2f, 0.4f}, {0.5f, 0.6f, 0.7f}});
  r.Load("b", bk.Build("B"));
  SND_CHECK_NEAR(LayerLevel(r, "G", 0.1f), 0.0f, 1e-4f);
  SND_CHECK_NEAR(LayerLevel(r, "G", 0.5f), 0.0f, 1e-4f);
  SND_CHECK_NEAR(LayerLevel(r, "G", 0.9f), 0.0f, 1e-4f);
  SND_CHECK_NEAR(LayerLevel(r, "G", 0.3f), 0.5f, 0.005f);
  SND_CHECK_NEAR(LayerLevel(r, "G", 0.65f), 0.5f, 0.005f);
}

// Three overlapping layers: records the gains (D-143 defines the pairwise overlap only).
SND_TEST(IndepG4Layer, SpecHole_ThreeOverlappingLayers) {
  Rig r;
  B bk;
  AddLayerParam(bk);
  AddLayerEvent(bk, "A", {{0.5f, 0.0f, 0.5f}, {0.0f, 0.3f, 0.7f}, {0.0f, 0.4f, 1.0f}});
  AddLayerEvent(bk, "Bx", {{0.0f, 0.0f, 0.5f}, {0.5f, 0.3f, 0.7f}, {0.0f, 0.4f, 1.0f}});
  AddLayerEvent(bk, "Cx", {{0.0f, 0.0f, 0.5f}, {0.0f, 0.3f, 0.7f}, {0.5f, 0.4f, 1.0f}});
  r.Load("b", bk.Build("B"));
  for (float v : {0.35f, 0.45f, 0.6f}) {
    const float a = LayerLevel(r, "A", v) / 0.5f, b = LayerLevel(r, "Bx", v) / 0.5f, c = LayerLevel(r, "Cx", v) / 0.5f;
    snd::test::Note("v %.2f: gains %.4f %.4f %.4f, sum of squares %.4f", double(v), double(a), double(b), double(c),
                    double(a * a + b * b + c * c));
    SND_CHECK(a >= 0.0f && b >= 0.0f && c >= 0.0f && a <= 1.0f && b <= 1.0f && c <= 1.0f);
  }
}

// A jump of the parameter from one end to the other in one block does not click (D-15
// short fade): the second difference of the output stays small, the level ends at B's.
SND_TEST(IndepG4Layer, JumpAcrossTheRangeDoesNotClick) {
  Rig r;
  B bk;
  AddLayerParam(bk);
  AddLayerEvent(bk, "J", {{0.8f, 0.0f, 0.45f}, {0.2f, 0.55f, 1.0f}});
  r.Load("b", bk.Build("B"));
  r.sys->SetParamGlobal(HashId("L"), 0.0f);
  const snd_playback_handle h = r.sys->Play(HashId("J"), NoEm(), nullptr);
  r.Steps(4);
  const std::size_t from = r.Now() - 4;
  r.sys->SetParamPlayback(h, HashId("L"), 1.0f);
  r.Steps(4);
  r.sys->SetParamPlayback(h, HashId("L"), 0.0f);
  r.Steps(4);
  float d2 = 0;
  for (std::size_t f = from + 1; f + 1 < r.Now(); ++f) d2 = std::max(d2, std::fabs(r.At(f + 1) - 2 * r.At(f) + r.At(f - 1)));
  snd::test::Note("max |second difference| %g (a step would be ~0.4)", double(d2));
  SND_CHECK(d2 < 0.01f);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.8f, 0.005f);
  SND_CHECK_NEAR(r.Mean(r.Now() - 5 * 256, r.Now() - 4 * 256) / kC, 0.2f, 0.005f);
}

// Scope: the playback's value wins over the global one (4.3).
SND_TEST(IndepG4Layer, PlaybackScopeWinsOverGlobal) {
  Rig r;
  B bk;
  AddLayerParam(bk);
  AddLayerEvent(bk, "S", {{0.8f, 0.0f, 0.45f}, {0.2f, 0.55f, 1.0f}});
  r.Load("b", bk.Build("B"));
  r.sys->SetParamGlobal(HashId("L"), 0.0f);
  const snd_playback_handle h = r.sys->Play(HashId("S"), NoEm(), nullptr);
  r.sys->SetParamPlayback(h, HashId("L"), 1.0f);
  const snd_playback_handle g = r.sys->Play(HashId("S"), NoEm(), nullptr);  // global only
  r.Steps(6);
  // Sum of both: h at B (0.2), g at A (0.8).
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 1.0f, 0.005f);
  r.sys->Stop(g, 0.0f, nullptr);
  r.Steps(3);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.2f, 0.005f);
}

// ---------------------------------------------------------------------------
// Nested events (D-144)
// ---------------------------------------------------------------------------
namespace {
// Buses: 0 "Sfx" -> 2, 1 "Kid" -> 2, 2 "Master".
B NestBank() {
  B bk;
  bk.b.buses.clear();
  bk.b.buses.push_back(TBus{HashId("Sfx"), 2u, 0, 0, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Kid"), 2u, 0, 0, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  return bk;
}
TClip NestClip(std::uint32_t row, float volumeDb = 0, float cents = 0) {
  TClip c;
  c.kind = 1;
  c.target = row;
  c.volumeDb = volumeDb;
  c.pitchCents = cents;
  return c;
}
float Peak(Rig& r, const char* bus) {
  snd_meter m{};
  r.sys->Update();
  if (r.sys->GetMeter(HashId(bus), &m) != SND_OK) return -1.0f;
  return m.peak[0];
}
}  // namespace

// The child plays on its own bus, with the parent clip's volume.
SND_TEST(IndepG4Nest, ChildUsesItsOwnBusAndTheParentClipVolume) {
  Rig r;
  B bk = NestBank();
  bk.Event("P", {NestClip(1, -6.0206f)}, 0, 0);                   // row 0, bus Sfx
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1);          // row 1, bus Kid, DC 0.5
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.25f, 0.002f);
  const float kid = Peak(r, "Kid"), sfx = Peak(r, "Sfx");
  snd::test::Note("peak Kid %g, Sfx %g", double(kid), double(sfx));
  SND_CHECK_NEAR(kid, 0.25f * kC, 0.002f);
  SND_CHECK_NEAR(sfx, 0.0f, 1e-6f);
  // Stopping the parent stops the child; one end for the parent.
  r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(4);
  SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_STOPPED));
  SND_CHECK_EQ(r.ends.size(), 1u);
}

// The child inherits the parent clip's pitch: a 4800-frame asset at +1200 cents takes 2400.
SND_TEST(IndepG4Nest, ChildInheritsTheParentClipPitch) {
  Rig r;
  B bk = NestBank();
  bk.Event("P", {NestClip(1, 0, 1200.0f)});
  bk.Event("C", {B::AssetClip(bk.Asset("a", Constant(4800, 0.5f)))}, 0, 1);
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(30);
  const std::size_t end = r.EndAbove(t0, r.Now(), 0.25f * kC) - t0;
  snd::test::Note("child length %llu (expected 2400)", static_cast<unsigned long long>(end));
  SND_CHECK(end >= 2398 && end <= 2402);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_COMPLETED));
}

// The child's category applies, not the parent's (D-144).
SND_TEST(IndepG4Nest, ChildUsesItsOwnCategory) {
  Rig r;
  B bk = NestBank();
  bk.b.categories.push_back(TCategory{HashId("Loud"), kNone, 0, 0.0f});
  bk.b.categories.push_back(TCategory{HashId("Quiet"), kNone, 0, -20.0f});
  bk.Event("P", {NestClip(1)}).category = 0;
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1).category = 1;
  bk.Event("P2", {NestClip(3)}).category = 1;
  bk.Event("C2", {B::SynthClip(bk.Synth(500.0f))}, 0, 1).category = 0;
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.05f, 0.001f);
  r.sys->Stop(h, 0.0f, nullptr);
  r.Steps(3);
  r.sys->Play(HashId("P2"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.5f, 0.002f);
}

// The child's priority decides the real voice (one real voice): child 200 beats Q 100
// even though the parent has 50.
SND_TEST(IndepG4Nest, ChildUsesItsOwnPriority) {
  Rig r(Config(1));
  B bk = NestBank();
  bk.Event("P", {NestClip(1)}).priority = 50;
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1).priority = 200;
  bk.Event("Q", {B::SynthClip(bk.Synth(300.0f))}).priority = 100;
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("Q"), NoEm(), nullptr);
  r.Steps(3);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.3f, 0.002f);
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.5f, 0.002f);
}

// Pausing the parent pauses the child; resuming brings it back.
SND_TEST(IndepG4Nest, PausingTheParentPausesTheChild) {
  Rig r;
  B bk = NestBank();
  bk.Event("P", {NestClip(1)});
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1);
  r.Load("b", bk.Build("B"));
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(3);
  r.sys->Pause(h, 0.0f);
  r.Steps(4);
  SND_CHECK_NEAR(r.MaxAbs(r.Now() - 256, r.Now()), 0.0f, 1e-6f);
  r.sys->ResumePlayback(h, 0.0f);
  r.Steps(4);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.5f, 0.002f);
  SND_CHECK_EQ(r.CountEnds(h.value), 0u);
}

// The child's limit group (max 1, "never") rejects the second child; the first plays on.
// Records how the second parent ends (its only voice was refused).
SND_TEST(IndepG4Nest, ChildLimitGroupFullRefusesTheSecondChild) {
  Rig r;
  B bk = NestBank();
  bk.b.groups.push_back(TGroup{HashId("G"), 1, 3});
  bk.Event("P", {NestClip(1)});
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1).group = 0;
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(2);
  const snd_playback_handle b = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.Mean(r.Now() - 256, r.Now()) / kC, 0.5f, 0.002f);
  SND_CHECK_EQ(r.CountEnds(a.value), 0u);
  const End* e = r.EndOf(b.value);
  snd::test::Note("second parent: %s reason %u detail %u", e != nullptr ? "ended" : "still playing (silent)",
                  e != nullptr ? e->reason : 0u, e != nullptr ? e->detail : 0u);
  SND_CHECK(r.CountEnds(b.value) <= 1u);
}

// A parent whose own voice is stolen (its group, oldest) while its nested child plays on:
// records whether the playback ends (STOLEN, child silenced) or only loses that voice.
// 4.1 lists "stolen" as a playback end; 4.4 steals voices. Not decided for a playback
// with several voices.
SND_TEST(IndepG4Nest, SpecHole_StealOfOneVoiceOfAMultiVoiceParent) {
  Rig r;
  B bk = NestBank();
  bk.b.groups.push_back(TGroup{HashId("G"), 1, 0});
  // P: its own clip (DC 0.1, group G) and the nested child (DC 0.5).
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(100.0f)), NestClip(1)});
  p.group = 0;
  bk.Event("C", {B::SynthClip(bk.Synth(500.0f))}, 0, 1);
  r.Load("b", bk.Build("B"));
  const snd_playback_handle a = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(3);
  const float one = r.Mean(r.Now() - 256, r.Now()) / kC;
  const snd_playback_handle b = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  const float after = r.Mean(r.Now() - 256, r.Now()) / kC;
  snd::test::Note("one parent %g, after the second %g; first parent ends %u (1.1 = only its own voice stolen, "
                  "0.6 = the whole first playback stolen)", double(one), double(after), r.CountEnds(a.value));
  SND_CHECK(r.CountEnds(a.value) <= 1u);
  SND_CHECK_EQ(r.CountEnds(b.value), 0u);
}

namespace {
// Chain E0 -> E1 -> ... -> E<n-1> (leaf: DC 0.5). Returns the level heard for E<k>
// (nesting depth n - 1 - k) and the number of errors posted.
void Chain(B& bk, int n) {
  for (int i = 0; i < n; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "E%d", i);
    if (i + 1 < n) {
      bk.Event(name, {NestClip(std::uint32_t(i + 1))});
    } else {
      bk.Event(name, {B::SynthClip(bk.Synth(500.0f))});
    }
  }
}
}  // namespace

// Depth: records the deepest chain that sounds; a too deep chain sounds nothing and warns.
SND_TEST(IndepG4Nest, DepthLimitPlaysUpToTheLimitAndWarnsBeyond) {
  Rig r;
  B bk = NestBank();
  const int n = 12;
  Chain(bk, n);
  r.Load("b", bk.Build("B"));
  int deepestHeard = -1, firstSilent = -1;
  for (int depth = 0; depth < n; ++depth) {
    char name[16];
    std::snprintf(name, sizeof name, "E%d", n - 1 - depth);
    const std::size_t errs = r.errors.size();
    const snd_playback_handle h = r.sys->Play(HashId(name), NoEm(), nullptr);
    r.Steps(4);
    const float lvl = r.Mean(r.Now() - 256, r.Now()) / kC;
    const bool heard = std::fabs(lvl - 0.5f) < 0.002f;
    if (heard) deepestHeard = depth;
    if (!heard && firstSilent < 0) firstSilent = depth;
    snd::test::Note("depth %d: level %g, errors +%llu", depth, double(lvl), static_cast<unsigned long long>(r.errors.size() - errs));
    if (!heard) SND_CHECK(r.errors.size() > errs);  // "鳴らさず警告"
    SND_CHECK(heard || std::fabs(lvl) < 1e-6f);     // all or nothing
    r.sys->Stop(h, 0.0f, nullptr);
    r.Steps(3);
  }
  snd::test::Note("deepest chain heard: %d, first silent: %d", deepestHeard, firstSilent);
  SND_CHECK_EQ(deepestHeard, 4);  // the limit is not in the spec; 4 observed (pinned, see the report)
  SND_CHECK(firstSilent < 0 || firstSilent == deepestHeard + 1);
}

// A nested clip that names its own event or an earlier one is refused by the bank check (6.0).
SND_TEST(IndepG4Nest, SelfReferenceAndBackReferenceAreRejectedAtLoad) {
  for (std::uint32_t target : {0u, 1u}) {
    Rig r;
    B bk = NestBank();
    bk.Event("X", {B::SynthClip(bk.Synth(500.0f))});
    bk.Event("Y", {NestClip(target)});  // row 1: target 0 = earlier, 1 = itself
    r.Load("b", bk.Build("B"));
    SND_REQUIRE(!r.loads.empty());
    snd::test::Note("target row %u: load result %d", target, int(r.loads.back()));
    SND_CHECK(r.loads.back() != SND_OK);
  }
}

// ---------------------------------------------------------------------------
// Action tracks (D-145)
// ---------------------------------------------------------------------------
namespace {
TAction Act(float ms, std::uint32_t kind, Id target, float value = 0) {
  TAction a;
  a.timeMs = ms;
  a.action = kind;
  a.target = target;
  a.value = value;
  return a;
}
// "P": a silent endless clip and the given actions; "X": DC 0.5, "Y": DC 0.25 (endless).
B ActionBank(const std::vector<TAction>& actions, std::uint32_t pLoop = 1) {
  B bk;
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(0.0f))});
  p.tracks[0].actions = actions;
  p.tracks[0].clips[0].loopCount = pLoop;
  bk.Event("X", {B::SynthClip(bk.Synth(500.0f))});
  bk.Event("Y", {B::SynthClip(bk.Synth(250.0f))});
  return bk;
}
constexpr std::uint32_t kPlay = 0, kStopA = 1, kSetParam = 2, kSnap = 3;
}  // namespace

// PlayEvent at 0, at 10 ms (480, inside a block) and at 16 ms (768, a block boundary):
// the child starts exactly at that frame.
SND_TEST(IndepG4Action, PlayEventStartsOnTheExactFrame) {
  for (float ms : {0.0f, 10.0f, 16.0f, 37.5f}) {
    Rig r;
    B bk = ActionBank({Act(ms, kPlay, HashId("X"))});
    r.Load("b", bk.Build("B"));
    r.Steps(1);
    const std::size_t t0 = r.Now();
    const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(12);
    const std::size_t first = r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0;
    const std::size_t expected = static_cast<std::size_t>(ms * 48.0f + 0.5f);
    snd::test::Note("action at %.1f ms: child from frame %llu (expected %llu)", double(ms), static_cast<unsigned long long>(first), static_cast<unsigned long long>(expected));
    SND_CHECK_EQ(first, expected);
    SND_CHECK_NEAR(r.At(t0 + expected + 1) / kC, 0.5f, 1e-4f);
    // The children have no handle: no end for them, the parent has not ended.
    SND_CHECK_EQ(r.ends.size(), 0u);
    r.sys->Stop(h, 0.0f, nullptr);
    r.Steps(3);
    SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  }
}

// Two actions at the same time both fire on the same frame.
SND_TEST(IndepG4Action, TwoActionsAtTheSameTime) {
  Rig r;
  B bk = ActionBank({Act(10.0f, kPlay, HashId("X")), Act(10.0f, kPlay, HashId("Y"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_NEAR(r.At(t0 + 479) / kC, 0.0f, 1e-6f);
  SND_CHECK_NEAR(r.At(t0 + 480) / kC, 0.75f, 1e-4f);
}

// StopEvent at 30 ms stops the child started at 10 ms: silent a short fade after 1440.
SND_TEST(IndepG4Action, StopEventStopsTheChild) {
  Rig r;
  B bk = ActionBank({Act(10.0f, kPlay, HashId("X")), Act(30.0f, kStopA, HashId("X"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(12);
  SND_CHECK_NEAR(r.At(t0 + 1279) / kC, 0.5f, 1e-4f);
  SND_CHECK_NEAR(r.MaxAbs(t0 + 1440 + 241, r.Now()), 0.0f, 1e-6f);
  SND_CHECK_EQ(r.ends.size(), 0u);
}

// DEFECT (D-145 "時刻のサンプルで正確に効く"): a StopEvent action at 30 ms (frame 1440, inside
// block 5 = 1280..1535) starts the stop fade at the block's first frame (1280), 160 frames
// before its time. Expected: full level up to frame 1439, the fade from 1440.
SND_TEST(IndepG4Action, Defect_StopEventActionTakesEffectBeforeItsTime) {
  Rig r;
  B bk = ActionBank({Act(10.0f, kPlay, HashId("X")), Act(30.0f, kStopA, HashId("X"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(12);
  snd::test::Note("X at 1439 %g, 1440 %g, 1560 %g, 1700 %g", double(r.At(t0 + 1439) / kC), double(r.At(t0 + 1440) / kC),
                  double(r.At(t0 + 1560) / kC), double(r.At(t0 + 1700) / kC));
  SND_CHECK_NEAR(r.At(t0 + 1439) / kC, 0.5f, 1e-4f);
  SND_CHECK(r.At(t0 + 1441) / kC < 0.5f);
  SND_CHECK_NEAR(r.MaxAbs(t0 + 1440 + 241, r.Now()), 0.0f, 1e-6f);
}

// A StopEvent naming the playback's own event stops it (one end, STOPPED). The frame
// accuracy of the stop is Defect_StopEventActionTakesEffectBeforeItsTime.
SND_TEST(IndepG4Action, StopActionStopsItsOwnPlayback) {
  Rig r;
  B bk;
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(500.0f))});
  p.tracks[0].actions = {Act(10.0f, kStopA, HashId("P"))};
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(8);
  SND_CHECK_NEAR(r.At(t0 + 255) / kC, 0.5f, 1e-4f);
  SND_CHECK_NEAR(r.MaxAbs(t0 + 480 + 241, r.Now()), 0.0f, 1e-6f);
  SND_CHECK_EQ(r.CountEnds(h.value), 1u);
  SND_CHECK(r.Ended(h.value, SND_END_STOPPED));
}

namespace {
// P has a volume curve on "V" (0 -> 0 dB, 1 -> -20 dB); the action sets V = 1 at 20 ms (960).
B SetParamBank() {
  Rig r;
  B bk;
  TCurve cv;
  cv.input = HashId("V");
  cv.target = 0;   // Volume
  cv.compose = 0;  // AddDb
  cv.points = {{0.0f, 0.0f}, {1.0f, -20.0f}};
  bk.b.curves.push_back(cv);
  TParam pv;
  pv.id = HashId("V");
  bk.b.params.push_back(pv);
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(500.0f))});
  p.tracks[0].actions = {Act(20.0f, kSetParam, HashId("V"), 1.0f)};
  p.curves = {0};
  return bk;
}
}  // namespace

// SetParam: by default the scope is the playback that owns the action (D-145).
SND_TEST(IndepG4Action, SetParamActsOnItsOwnPlayback) {
  Rig r;
  B bk = SetParamBank();
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(8);
  SND_CHECK_NEAR(r.At(t0 + 767) / kC, 0.5f, 1e-3f);
  SND_CHECK_NEAR(r.At(t0 + 1279) / kC, 0.05f, 1e-3f);  // in effect within a block after it
  // Another playback started now is not affected (not Global).
  const std::size_t t1 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(2);
  SND_CHECK_NEAR(r.At(t1 + 100) / kC - r.At(t0 + 1279) / kC, 0.5f, 2e-3f);
}

// DEFECT (D-145): the SetParam action at 20 ms (frame 960, block 3 = 768..1023) ramps the
// volume over the whole block 3: at frame 959 the level is already 0.16 instead of 0.5.
// Expected: no effect before frame 960.
SND_TEST(IndepG4Action, Defect_SetParamActionTakesEffectBeforeItsTime) {
  Rig r;
  B bk = SetParamBank();
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(8);
  snd::test::Note("P at 800 %g, 959 %g, 960 %g, 1023 %g", double(r.At(t0 + 800) / kC), double(r.At(t0 + 959) / kC),
                  double(r.At(t0 + 960) / kC), double(r.At(t0 + 1023) / kC));
  SND_CHECK_NEAR(r.At(t0 + 959) / kC, 0.5f, 1e-3f);
}

// After the parent stopped, its actions do not fire.
SND_TEST(IndepG4Action, StopCancelsTheActions) {
  {
    Rig r;
    B bk = ActionBank({Act(30.0f, kPlay, HashId("X"))});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(2);
    r.sys->Stop(h, 0.0f, nullptr);
    r.Steps(20);
    SND_CHECK_NEAR(r.MaxAbs(t0, r.Now()), 0.0f, 1e-6f);
  }
}

// DEFECT (D-145 "一時停止中は時刻が進まない"): the parent is paused at frame 256, before its
// action at 10 ms (480), and resumed 10 blocks later; the action fires at 480 while the
// parent is paused. Expected: the child starts 224 frames of track time after the resume
// (3040, a short fade of slack).
SND_TEST(IndepG4Action, Defect_PausedParentStillFiresItsActions) {
  {
    Rig r;
    B bk = ActionBank({Act(10.0f, kPlay, HashId("X"))});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(1);                 // 256 frames of track time
    r.sys->Pause(h, 0.0f);      // applied at t0 + 256
    r.Steps(10);
    r.sys->ResumePlayback(h, 0.0f);  // applied at t0 + 2816
    r.Steps(6);
    const std::size_t first = r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0;
    snd::test::Note("paused parent: child from %llu (expected 2816 + 224 = 3040)", static_cast<unsigned long long>(first));
    SND_CHECK(first >= 3040 && first <= 3040 + 240);
  }
}

// A parent with a scheduled start (snd_when clock) counts the track time from its start.
SND_TEST(IndepG4Action, ScheduledParentCountsFromItsStart) {
  Rig r;
  B bk = ActionBank({Act(10.0f, kPlay, HashId("X"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_when w = AtClock(t0 + 1000);
  r.sys->Play(HashId("P"), NoEm(), &w);
  r.Steps(10);
  const std::size_t first = r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0;
  snd::test::Note("scheduled parent: child from %llu (expected 1480)", static_cast<unsigned long long>(first));
  SND_CHECK_EQ(first, 1480u);
}

// Many actions at time 0: bounded (CoreLimits.maxActionChildren); records how many sound.
SND_TEST(IndepG4Action, ManyActionsAreBounded) {
  snd_config cfg = Config(128);
  cfg.max_virtual_voices = 256;
  cfg.max_playbacks = 256;
  Rig r(cfg);
  B bk;
  std::vector<TAction> acts;
  for (int i = 0; i < 100; ++i) acts.push_back(Act(0.0f, kPlay, HashId("Z")));
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(0.0f))});
  p.tracks[0].actions = acts;
  bk.Event("Z", {B::SynthClip(bk.Synth(1.0f))});  // DC 0.001
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(4);
  const float n = r.Mean(r.Now() - 256, r.Now()) / kC / 0.001f;
  snd::test::Note("children sounding: %.1f (of 100 actions, limit 64)", double(n));
  SND_CHECK(n <= 64.5f);
  SND_CHECK(n >= 1.0f);
}

// An action naming an event that is not loaded: nothing plays, nothing breaks.
SND_TEST(IndepG4Action, UnknownTargetIsHarmless) {
  Rig r;
  B bk = ActionBank({Act(0.0f, kPlay, HashId("Nope")), Act(5.0f, kPlay, HashId("X"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(6);
  SND_CHECK_EQ(r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0, 240u);
  SND_CHECK_EQ(r.CountEnds(h.value), 0u);
}

// The track time of a looping or pitched parent: records when a 50 ms action fires.
SND_TEST(IndepG4Action, SpecHole_TrackTimeOfALoopingOrPitchedParent) {
  {
    Rig r;
    B bk;
    TEvent& p = bk.Event("P", {B::AssetClip(bk.Asset("p", Constant(960, 0.0f)))});  // 20 ms, loops forever
    p.tracks[0].clips[0].loopCount = 0;
    p.tracks[0].actions = {Act(10.0f, kPlay, HashId("X"))};
    bk.Event("X", {B::AssetClip(bk.Asset("x", Constant(48, 0.5f)))});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(20);
    std::vector<std::size_t> starts;
    for (std::size_t f = t0 + 1; f < r.Now(); ++f) {
      if (std::fabs(r.At(f)) > 0.25f * kC && std::fabs(r.At(f - 1)) <= 0.25f * kC) starts.push_back(f - t0);
    }
    snd::test::Note("looping parent (20 ms clip), action at 10 ms: %llu firing(s), first at %llu", static_cast<unsigned long long>(starts.size()),
                    static_cast<unsigned long long>(starts.empty() ? 0 : starts[0]));
    SND_CHECK(!starts.empty());
  }
  {
    Rig r;
    B bk;
    TEvent& p = bk.Event("P", {B::AssetClip(bk.Asset("p", Constant(48000, 0.0f)))});
    p.tracks[0].clips[0].pitchCents = 1200.0f;
    p.tracks[0].actions = {Act(10.0f, kPlay, HashId("X"))};
    bk.Event("X", {B::SynthClip(bk.Synth(500.0f))});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    r.sys->Play(HashId("P"), NoEm(), nullptr);
    r.Steps(6);
    snd::test::Note("pitched parent (x2), action at 10 ms: child from %llu (480 = clock time, 240 = material time)",
                    static_cast<unsigned long long>(r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0));
  }
}

// DEFECT (D-145): ApplySnapshot at 20 ms (frame 960) already lowers the bus over block 3
// (768..1023): 0.16 at frame 959. Expected: the level is unchanged before frame 960 (and lower after).
SND_TEST(IndepG4Action, Defect_SnapshotActionTakesEffectBeforeItsTime) {
  Rig r;
  B bk;
  TSnapshot sn;
  sn.id = HashId("Quiet");
  sn.values.push_back(TSnapshot::Value{0, 0, 0, -20.0f});  // bus Sfx volume
  bk.b.snapshots.push_back(sn);
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(500.0f))});
  p.tracks[0].actions = {Act(20.0f, kSnap, HashId("Quiet"), 1.0f)};
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(30);
  snd::test::Note("level at 959 %g, 1279 %g, end %g", double(r.At(t0 + 959) / kC), double(r.At(t0 + 1279) / kC),
                  double(r.At(r.Now() - 1) / kC));
  SND_CHECK_NEAR(r.At(t0 + 959) / kC, 0.5f, 1e-3f);
  SND_CHECK(r.At(r.Now() - 1) / kC < 0.4f);
}

// ---------------------------------------------------------------------------
// Added after reading the implementation (step 3)
// ---------------------------------------------------------------------------

// DEFECT (D-145 "停止後は発火しない"): actions are taken one block ahead; a Stop of the parent
// that arrives in the block before the action's time does not cancel it. The parent is
// stopped at frame 256; its action at 10 ms (480) still starts X at 480.
SND_TEST(IndepG4Action, Defect_ActionFiresAfterTheParentStopped) {
  Rig r;
  B bk = ActionBank({Act(10.0f, kPlay, HashId("X"))});
  r.Load("b", bk.Build("B"));
  const std::size_t t0 = r.Now();
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(1);
  r.sys->Stop(h, 0.0f, nullptr);  // applied at frame 256
  r.Steps(10);
  snd::test::Note("child heard from %llu (expected never)", static_cast<unsigned long long>(r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0));
  SND_CHECK_NEAR(r.MaxAbs(t0, r.Now()), 0.0f, 1e-6f);
}

// DEFECT (D-145 "時刻のサンプルで正確に効く", "予約付き開始"): a parent scheduled at frame 1000
// (inside block 3) with actions at 0 ms and 2 ms: the children start at the next block's
// first frame (1024) instead of 1000 and 1096.
SND_TEST(IndepG4Action, Defect_ScheduledParentFirstBlockActionsAreLate) {
  for (float ms : {0.0f, 2.0f}) {
    Rig r;
    B bk = ActionBank({Act(ms, kPlay, HashId("X"))});
    r.Load("b", bk.Build("B"));
    const std::size_t t0 = r.Now();
    const snd_when w = AtClock(t0 + 1000);
    r.sys->Play(HashId("P"), NoEm(), &w);
    r.Steps(10);
    const std::size_t first = r.FirstAbove(t0, r.Now(), 0.25f * kC) - t0;
    snd::test::Note("scheduled at 1000, action at %.0f ms: child from %llu (expected %llu)", double(ms), static_cast<unsigned long long>(first),
                    static_cast<unsigned long long>(std::size_t(1000 + ms * 48.0f)));
    SND_CHECK_EQ(first, std::size_t(1000 + ms * 48.0f));
  }
}

// DEFECT (D-145; a play that does not start has no actions: ActionsOfARefusedPlayAreCancelled
// cancels its Play children): a refused play (limit group full, "never") still applies its
// first-block ApplySnapshot: the bus drops by 20 dB although the parent never started.
SND_TEST(IndepG4Action, Defect_RefusedPlayStillAppliesItsFirstBlockSnapshot) {
  Rig r;
  B bk;
  bk.b.groups.push_back(TGroup{HashId("G"), 1, 3});
  TSnapshot sn;
  sn.id = HashId("Quiet");
  sn.values.push_back(TSnapshot::Value{0, 0, 0, -20.0f});
  bk.b.snapshots.push_back(sn);
  TEvent& p = bk.Event("P", {B::SynthClip(bk.Synth(500.0f))});
  p.group = 0;
  p.tracks[0].actions = {Act(0.0f, kSnap, HashId("Quiet"), 1.0f)};
  bk.Event("Other", {B::SynthClip(bk.Synth(500.0f))}).group = 0;
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("Other"), NoEm(), nullptr);  // takes the group's only voice
  r.Steps(3);
  const float before = r.Mean(r.Now() - 256, r.Now()) / kC;
  const snd_playback_handle h = r.sys->Play(HashId("P"), NoEm(), nullptr);  // refused
  r.Steps(30);
  const float after = r.Mean(r.Now() - 256, r.Now()) / kC;
  snd::test::Note("refused: %s; level before %g, after %g", r.Ended(h.value, SND_END_REJECTED) ? "yes" : "no",
                  double(before), double(after));
  SND_CHECK(r.Ended(h.value, SND_END_REJECTED));
  SND_CHECK_NEAR(after, before, 1e-3f);
}

// Many action children in one block beyond CoreLimits.maxActionChildren (first block) and the
// deferred queue (later blocks): bounded; records how many sound and whether a warning is posted.
SND_TEST(IndepG4Action, SpecHole_ActionChildrenBeyondTheLimits) {
  snd_config cfg = Config(128);
  cfg.max_virtual_voices = 512;
  cfg.max_playbacks = 1024;
  Rig r(cfg);
  B bk;
  std::vector<TAction> acts;
  for (int i = 0; i < 100; ++i) acts.push_back(Act(10.0f, kPlay, HashId("Z")));
  for (int k = 0; k < 3; ++k) {
    char n[8];
    std::snprintf(n, sizeof n, "P%d", k);
    TEvent& p = bk.Event(n, {B::SynthClip(bk.Synth(0.0f))});
    p.tracks[0].actions = acts;
  }
  bk.Event("Z", {B::SynthClip(bk.Synth(1.0f))});  // DC 0.001
  r.Load("b", bk.Build("B"));
  for (int k = 0; k < 3; ++k) {
    char n[8];
    std::snprintf(n, sizeof n, "P%d", k);
    r.sys->Play(HashId(n), NoEm(), nullptr);
  }
  const std::size_t errs = r.errors.size();
  r.Steps(6);
  const float n = r.Mean(r.Now() - 256, r.Now()) / kC / 0.001f;
  snd::test::Note("300 actions at 10 ms in one block: %.1f children sound (real voices 128), warnings +%llu", double(n),
                  static_cast<unsigned long long>(r.errors.size() - errs));
  SND_CHECK(n <= 300.5f);
}

// Shuffle: several plays of the same event in one block still take a permutation (7 clips of
// levels 2^k / 128: the sum of a round played in one block is 127 / 128 only for a permutation).
SND_TEST(IndepG4Seq, ShuffleRoundsPlayedWithinOneBlock) {
  Rig r(Config(16));
  B bk;
  std::vector<TClip> cl;
  for (int k = 0; k < 7; ++k) {
    char an[16];
    std::snprintf(an, sizeof an, "s%d", k);
    cl.push_back(B::AssetClip(bk.Asset(an, Constant(2400, float(1 << k) / 128.0f))));
  }
  bk.Event("SH", cl, 4);
  r.Load("b", bk.Build("B"));
  for (int round = 0; round < 12; ++round) {
    const std::size_t t0 = r.Now();
    for (int i = 0; i < 7; ++i) r.sys->Play(HashId("SH"), NoEm(), nullptr);
    r.Steps(12);
    SND_CHECK_NEAR(r.At(t0 + 1200) / kC, 127.0f / 128.0f, 1e-4f);
  }
}

// loopCount 1 ("既定") on an asset with loop points: records whether it loops endlessly
// (D-142 says 1 = default without saying what the default is for an asset with loop points).
SND_TEST(IndepG4Loop, SpecHole_LoopCountOneOnAnAssetWithLoopPoints) {
  const LoopRun one = RunLoop(1000, 1, 0, 80, 200, 800);
  snd::test::Note("loopCount 1, loop points [200, 800): end %llu, ends %u (endless: end = 20480)", static_cast<unsigned long long>(one.end), one.ends);
  SND_CHECK(one.ends <= 1u);
}

// Nested: the child event's own instance limit (EVNT.maxInstances) - D-144 says the child uses
// its own limits; records whether a second parent's child is refused.
SND_TEST(IndepG4Nest, SpecHole_ChildEventInstanceLimit) {
  Rig r;
  B bk = NestBank();
  bk.Event("P", {NestClip(1)});
  bk.Event("C", {B::SynthClip(bk.Synth(300.0f))}, 0, 1).maxInstances = 1;
  r.Load("b", bk.Build("B"));
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(2);
  r.sys->Play(HashId("P"), NoEm(), nullptr);
  r.Steps(4);
  snd::test::Note("two parents, child maxInstances 1: level %g (0.3 = limited, 0.6 = not applied)",
                  double(r.Mean(r.Now() - 256, r.Now()) / kC));
}
