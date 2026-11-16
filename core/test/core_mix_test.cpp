// core: gains (clip, bus, category, snapshot, ducking), pitch / resampling,
// pan, fades, timing (sample-accurate start, delays, scheduled stop),
// parameters (3 scopes, ramps), bus effects and sends. Expectations are
// analytic (levels, frequencies, pan law, fade shapes).
#include <cmath>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"

using namespace coretest;
using snd::HashId;
using snd::ClockSample;

namespace {
constexpr float kCenter = 0.70710678f;  // mono -> stereo downmix gain (dsp pan.hpp)

ClockSample NextBlockClock(Harness& h) {
  h.sys->Update();
  snd_stats st{};
  h.sys->GetStats(&st);
  return st.clock_sample;
}
float Db(float db) { return std::pow(10.0f, db / 20.0f); }
// Mean of channel c over [from, to) frames of h.all.
float Mean(const Harness& h, std::size_t from, std::size_t to, std::uint32_t c) {
  double s = 0;
  for (std::size_t f = from; f < to; ++f) s += h.At(f, c);
  return static_cast<float>(s / double(to - from));
}
// Frame index (in h.all) of the first |x| > eps on channel 0 after `from`.
std::size_t FirstNonZero(const Harness& h, std::size_t from, float eps = 1e-6f) {
  for (std::size_t f = from; f < h.TotalFrames(); ++f) {
    if (std::fabs(h.At(f, 0)) > eps) return f;
  }
  return SIZE_MAX;
}
}  // namespace

SND_TEST(CoreMix, ClipBusAndCategoryGainsMultiply) {
  Harness h;
  TestBank b = BasicBank();
  b.buses[0].volumeDb = -6.0f;
  b.categories.push_back(TCategory{HashId("Cat"), kNone, 0, -3.0f});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(96000, 1.0f), 0, 0});
  TEvent e = SimpleEvent("E", 0, 0, -2.0f);
  e.category = 0;
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(6);
  const float g = kCenter * Db(-2.0f) * Db(-6.0f) * Db(-3.0f);
  SND_CHECK_NEAR(Mean(h, 4 * 256, 6 * 256, 0), g, 1e-4f);
  // Category volume 0.5 (instant), then mute, then the "all categories" volume (D-30).
  h.sys->SetCategoryVolume(HashId("Cat"), 0.5f, 0.0f);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 256, 3 * 256, 0), g * 0.5f, 1e-4f);
  h.sys->SetCategoryVolume(0, 0.25f, 0.0f);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 256, 3 * 256, 0), g * 0.5f * 0.25f, 1e-4f);
  h.sys->MuteCategory(HashId("Cat"), true);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 256, 3 * 256, 0), 0.0f, 1e-6f);
}

SND_TEST(CoreMix, BusVolumeFadeIsLinearInGain) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  // 0 -> 0.2 over 10 blocks (2560 frames): per-block targets 1 - 0.08 k.
  const float fade = 2560.0f / 48000.0f;
  h.sys->SetBusVolume(HashId("Sfx"), 0.2f, fade);
  h.Clear();
  h.Steps(14);
  for (int k = 1; k <= 10; ++k) {
    const float expect = kCenter * (1.0f - 0.08f * static_cast<float>(k));
    SND_CHECK_NEAR(h.At(static_cast<std::size_t>(k) * 256 - 1, 0), expect, 2e-3f);
  }
  SND_CHECK_NEAR(Mean(h, 12 * 256, 14 * 256, 0), kCenter * 0.2f, 1e-4f);
}

SND_TEST(CoreMix, StopFadeRampsToSilenceAndEnds) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  h.sys->Stop(p, 4.0f * 256.0f / 48000.0f, nullptr);  // 4 blocks
  h.Clear();
  h.Steps(8);
  // Block k (0-based) ramps from 1 - k/4 to 1 - (k+1)/4.
  for (int k = 0; k < 4; ++k) {
    SND_CHECK_NEAR(h.At(static_cast<std::size_t>(k) * 256, 0), kCenter * (1.0f - 0.25f * k), 1e-3f);
    SND_CHECK_NEAR(h.At(static_cast<std::size_t>(k) * 256 + 128, 0), kCenter * (1.0f - 0.25f * k - 0.125f), 2e-3f);
  }
  SND_CHECK_NEAR(Mean(h, 4 * 256, 8 * 256, 0), 0.0f, 1e-6f);
  h.sys->Update();
  SND_CHECK(h.ends.Has(p, SND_END_STOPPED));
}

SND_TEST(CoreMix, PauseAndResumeKeepPosition) {
  Harness h;
  TestBank b = BasicBank();
  std::vector<float> ramp(100000);
  for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 100000.0f;
  b.assets.push_back(TAsset{HashId("ramp"), 1, 48000, ramp, 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  h.sys->Pause(p, 0.0f);
  h.Steps(3);
  h.Clear();
  h.Steps(5);
  SND_CHECK_EQ(h.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PAUSED);
  SND_CHECK_NEAR(Mean(h, 0, 5 * 256, 0), 0.0f, 1e-7f);
  snd_position a{}, c{};
  h.sys->GetPosition(p, &a);
  h.sys->ResumePlayback(p, 0.0f);
  h.Clear();
  h.Steps(6);
  h.sys->GetPosition(p, &c);
  SND_CHECK_EQ(h.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  SND_CHECK(c.frames > a.frames);
  // The material continues where it stopped: the level is still near the start of the ramp.
  SND_CHECK(Mean(h, 4 * 256, 6 * 256, 0) < kCenter * 0.03f);
  SND_CHECK(Mean(h, 4 * 256, 6 * 256, 0) > 0.0f);
}

SND_TEST(CoreMix, PitchAndSourceRateGiveTheExpectedFrequency) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("s48"), 1, 48000, Sine(96000, 1000.0, 48000.0), 0, 0});
  b.assets.push_back(TAsset{HashId("s24"), 1, 24000, Sine(48000, 1000.0, 24000.0), 0, 0});
  TEvent up = SimpleEvent("Up", 0);
  up.tracks[0].clips[0].pitchCents = 1200.0f;  // one octave: 2 kHz
  b.events.push_back(up);
  b.events.push_back(SimpleEvent("Half", 1));  // 24 kHz source: still 1 kHz after resampling
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("Up"), snd_emitter_handle{0}, nullptr);
  h.Steps(30);
  const double a2k = ToneAmplitude(h.all, 2, 0, 2048, 4096, 2000.0, 48000.0);
  const double a1k = ToneAmplitude(h.all, 2, 0, 2048, 4096, 1000.0, 48000.0);
  SND_CHECK_NEAR(a2k, kCenter, 0.02);
  SND_CHECK(a1k < 0.01);
  Harness h2;
  h2.Load("b", b.Build(HashId("B")));
  h2.Clear();
  h2.sys->Play(HashId("Half"), snd_emitter_handle{0}, nullptr);
  h2.Steps(30);
  SND_CHECK_NEAR(ToneAmplitude(h2.all, 2, 0, 2048, 4096, 1000.0, 48000.0), kCenter, 0.02);
}

SND_TEST(CoreMix, OscillatorFollowsPitchCurveOnAParameter) {
  Harness h;
  TestBank b = BasicBank();
  b.synths.push_back(TSynth{0, 0, 1, 500.0f});
  b.params.push_back(TParam{HashId("Speed"), 0, 0.0f, 2.0f, 0.0f, 0.0f});
  b.curves.push_back(TCurve{HashId("Speed"), static_cast<std::uint32_t>(bf::CurveTarget::Pitch),
                            static_cast<std::uint32_t>(bf::CurveCompose::AddCents), {{0.0f, 0.0f}, {1.0f, 1200.0f}}});
  TEvent e;
  e.id = HashId("Osc");
  e.curves.assign(1, 0u);
  TTrack t;
  TClip c;
  c.kind = 2;
  c.target = 0;
  t.clips.push_back(c);
  e.tracks.push_back(t);
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("Osc"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(10);
  SND_CHECK_NEAR(ToneAmplitude(h.all, 2, 0, 1024, 1024, 500.0, 48000.0), kCenter, 0.03);
  h.sys->SetParamPlayback(p, HashId("Speed"), 1.0f);
  h.Clear();
  h.Steps(10);
  SND_CHECK_NEAR(ToneAmplitude(h.all, 2, 0, 1024, 1024, 1000.0, 48000.0), kCenter, 0.03);
}

SND_TEST(CoreMix, ParameterScopesPlaybackOverEmitterOverGlobal) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.params.push_back(TParam{HashId("Vol"), 0, -60.0f, 0.0f, 0.0f, 0.0f});
  b.curves.push_back(TCurve{HashId("Vol"), static_cast<std::uint32_t>(bf::CurveTarget::Volume),
                            static_cast<std::uint32_t>(bf::CurveCompose::AddDb), {{-60.0f, -60.0f}, {0.0f, 0.0f}}});
  TEvent e = SimpleEvent("E", 0);
  e.curves.assign(1, 0u);
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  const snd_emitter_handle em = h.sys->CreateEmitter();
  const snd_playback_handle p = h.sys->Play(HashId("E"), em, nullptr);
  auto level = [&]() {
    h.Clear();
    h.Steps(3);
    return Mean(h, 2 * 256, 3 * 256, 0) / kCenter;
  };
  SND_CHECK_NEAR(level(), 1.0f, 1e-4f);  // default 0 dB
  h.sys->SetParamGlobal(HashId("Vol"), -6.0f);
  SND_CHECK_NEAR(level(), Db(-6.0f), 1e-4f);
  h.sys->SetParamEmitter(em, HashId("Vol"), -12.0f);
  SND_CHECK_NEAR(level(), Db(-12.0f), 1e-4f);
  h.sys->SetParamPlayback(p, HashId("Vol"), -20.0f);
  SND_CHECK_NEAR(level(), Db(-20.0f), 1e-4f);
  h.sys->SetParamGlobal(HashId("Vol"), 0.0f);  // lower priority: no change
  SND_CHECK_NEAR(level(), Db(-20.0f), 1e-4f);
}

SND_TEST(CoreMix, ParameterRampRate) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 1.0f), 0, 0});
  // 0..1 at 1 unit per second, mapped to a linear gain curve via dB? use multiply on volume: dB curve.
  b.params.push_back(TParam{HashId("P"), 0, 0.0f, 1.0f, 0.0f, 1.0f});
  b.curves.push_back(TCurve{HashId("P"), static_cast<std::uint32_t>(bf::CurveTarget::Volume),
                            static_cast<std::uint32_t>(bf::CurveCompose::AddDb), {{0.0f, -40.0f}, {1.0f, 0.0f}}});
  TEvent e = SimpleEvent("E", 0);
  e.curves.assign(1, 0u);
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  h.sys->SetParamGlobal(HashId("P"), 1.0f);
  h.Clear();
  h.Steps(100);
  // After k blocks the value is k * 256 / 48000 (rate 1/s): -40 + 40 * value dB.
  for (int k : {10, 50, 90}) {
    const float v = static_cast<float>(k) * 256.0f / 48000.0f;
    SND_CHECK_NEAR(h.At(static_cast<std::size_t>(k) * 256 - 1, 0), kCenter * Db(-40.0f + 40.0f * v), 1e-3f);
  }
}

SND_TEST(CoreMix, SampleAccurateStartAndDelay) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("imp"), 1, 48000, Impulse(2000, 0, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("I", 0));
  h.Load("b", b.Build(HashId("B")));
  const ClockSample base = NextBlockClock(h);
  h.Clear();
  snd_when w{};
  w.kind = SND_WHEN_CLOCK_SAMPLE;
  w.value = base + 1000;  // block 3, frame 232
  h.sys->Play(HashId("I"), snd_emitter_handle{0}, &w);
  snd_when dly{};
  dly.kind = SND_WHEN_DELAY_US;
  dly.value = 50000;  // 2400 frames after the apply block start
  h.Steps(6);
  // Sinc resampler at ratio 1 (cutoff 0.45 fs): the impulse peaks at its frame, slightly lower.
  SND_CHECK_EQ(FirstNonZero(h, 0), std::size_t{1000});
  std::size_t peakAt = 0;
  for (std::size_t f = 0; f < h.TotalFrames(); ++f) {
    if (std::fabs(h.At(f, 0)) > std::fabs(h.At(peakAt, 0))) peakAt = f;
  }
  SND_CHECK_EQ(peakAt, std::size_t{1000});
  SND_CHECK(h.At(1000, 0) > 0.8f * kCenter);
  const ClockSample base2 = NextBlockClock(h);
  h.Clear();
  h.sys->Play(HashId("I"), snd_emitter_handle{0}, &dly);
  h.Steps(14);
  SND_CHECK_EQ(FirstNonZero(h, 0), std::size_t{2400});
  (void)base2;
}

SND_TEST(CoreMix, ScheduledStopCutsAtTheClock) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  const snd_playback_handle p = h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  const ClockSample base = NextBlockClock(h);
  snd_when w{};
  w.kind = SND_WHEN_CLOCK_SAMPLE;
  w.value = base + 600;
  h.sys->SetScheduledStop(p, &w, 0.0f);
  h.Clear();
  h.Steps(8);
  SND_CHECK_NEAR(h.At(500, 0), kCenter, 1e-4f);
  SND_CHECK_NEAR(Mean(h, 2 * 256 + 256, 8 * 256, 0), 0.0f, 1e-6f);
  h.sys->Update();
  SND_CHECK(h.ends.Has(p, SND_END_STOPPED));
}

SND_TEST(CoreMix, SnapshotWeightScalesBusVolume) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  b.snapshots.push_back(TSnapshot{HashId("Quiet"), {{0u, 0u, 0u, -12.0f}}});
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  h.sys->ApplySnapshot(HashId("Quiet"), 0.5f, 0.0f);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 2 * 256, 3 * 256, 0), kCenter * Db(-6.0f), 1e-4f);
  h.sys->ApplySnapshot(HashId("Quiet"), 1.0f, 0.0f);
  h.Clear();
  h.Steps(3);
  SND_CHECK_NEAR(Mean(h, 2 * 256, 3 * 256, 0), kCenter * Db(-12.0f), 1e-4f);
}

SND_TEST(CoreMix, RuleDuckingLowersTheTargetBusWhileTheCategoryPlays) {
  Harness h;
  TestBank b;
  b.buses.push_back(TBus{HashId("Music"), 2u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Voice"), 2u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.categories.push_back(TCategory{HashId("Dialog"), kNone, 0, 0.0f});
  b.ducks.push_back(TDuck{1u, 0u, 0u, 10.0f, 1.0f, 1.0f, 0.0f});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(400000, 1.0f), 0, 0});
  b.assets.push_back(TAsset{HashId("dc2"), 1, 48000, Constant(48000, 0.0f), 0, 0});
  b.events.push_back(SimpleEvent("Music", 0, 0));
  TEvent v = SimpleEvent("Line", 1, 1);
  v.category = 0;
  b.events.push_back(v);
  h.Load("b", b.Build(HashId("B")));
  h.sys->Play(HashId("Music"), snd_emitter_handle{0}, nullptr);
  h.Steps(3);
  h.sys->Play(HashId("Line"), snd_emitter_handle{0}, nullptr);
  h.Clear();
  h.Steps(20);
  // Attack 1 ms: fully ducked by -10 dB within a few blocks (bus gain is one block late).
  SND_CHECK_NEAR(Mean(h, 15 * 256, 20 * 256, 0), kCenter * Db(-10.0f), 2e-3f);
}

SND_TEST(CoreMix, BusLowpassEffectAttenuatesHighTones) {
  Harness h;
  TestBank b = BasicBank();
  b.effects.push_back(TEffect{snd::dsp::kEffectLowpass, {500.0f, 0.707f}});
  b.buses[0].effects.assign(1, 0u);
  std::vector<float> mix = Sine(96000, 100.0, 48000.0, 0.5f);
  const std::vector<float> hi = Sine(96000, 8000.0, 48000.0, 0.5f);
  for (std::size_t i = 0; i < mix.size(); ++i) mix[i] += hi[i];
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, mix, 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(20);
  const double lo = ToneAmplitude(h.all, 2, 0, 2048, 2048, 100.0, 48000.0);
  const double hiA = ToneAmplitude(h.all, 2, 0, 2048, 2048, 8000.0, 48000.0);
  SND_CHECK_NEAR(lo, 0.5 * kCenter, 0.02);
  SND_CHECK(hiA < 0.5 * kCenter * 0.01);  // 12 dB/oct, 4 octaves above the cutoff: < -40 dB
}

SND_TEST(CoreMix, PostFaderSendAddsToTheTarget) {
  Harness h;
  TestBank b;
  b.buses.push_back(TBus{HashId("Dry"), 2u, 0, 0, 0.0f, {}, {TBus::Send{1u, -6.0f, 0u}}});
  b.buses.push_back(TBus{HashId("Fx"), 2u, 0, 0, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0, 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  SND_CHECK_NEAR(Mean(h, 3 * 256, 4 * 256, 0), kCenter * (1.0f + Db(-6.0f)), 1e-4f);
}

SND_TEST(CoreMix, PanCurveMovesTheMonoSourceRight) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(200000, 1.0f), 0, 0});
  b.curves.push_back(TCurve{HashId("Pan"), static_cast<std::uint32_t>(bf::CurveTarget::Pan),
                            static_cast<std::uint32_t>(bf::CurveCompose::AddDb), {{-1.0f, -1.0f}, {1.0f, 1.0f}}});
  b.params.push_back(TParam{HashId("Pan"), 0, -1.0f, 1.0f, 0.0f, 0.0f});
  TEvent e = SimpleEvent("E", 0);
  e.curves.assign(1, 0u);
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  h.sys->SetParamGlobal(HashId("Pan"), 1.0f);
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  // Hard right: the right channel carries all the power, the left none.
  const float l = Mean(h, 3 * 256, 4 * 256, 0), r = Mean(h, 3 * 256, 4 * 256, 1);
  SND_CHECK_NEAR(l, 0.0f, 1e-3f);
  SND_CHECK(r > kCenter);
}
