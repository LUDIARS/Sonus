// Stage 3 group 2 (core): every standard effect kind on a bus chain, through core,
// does what it is set for (design 7 "エフェクト"). Banks are built in the test; the
// checks are analytic (delays and levels, comb notches, static curves, ceilings,
// frequency ratios, channel routing), not golden audio.
#include <cmath>
#include <vector>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;
constexpr double kRate = 48000.0;

double Amp(const Harness& h, std::size_t from, std::size_t n, double hz, std::uint32_t ch = 0) {
  return ToneAmplitude(h.all, h.Ch(), ch, from, n, hz, kRate);
}
double MaxAbs(const Harness& h, std::size_t from, std::size_t to, std::uint32_t ch = 0) {
  double m = 0;
  for (std::size_t f = from; f < to && f < h.TotalFrames(); ++f) m = std::max(m, double(std::fabs(h.At(f, ch))));
  return m;
}
double Db(double x) { return 20.0 * std::log10(x > 1e-30 ? x : 1e-30); }

// Plays asset `data` on the "Sfx" bus whose chain is the one effect (kind, params).
void RunOne(Harness& h, snd::Id kind, std::vector<float> params, std::vector<float> data, std::uint32_t blocks,
            std::uint32_t busChannels = 0, std::uint32_t assetChannels = 1) {
  TestBank b = BasicBank();
  b.effects.push_back(TEffect{kind, std::move(params)});
  b.buses[0].effects.assign(1, 0u);
  b.buses[0].channels = busChannels;
  if (busChannels > 2u) b.buses[1].channels = busChannels;  // master of the same layout (no downmix)
  b.assets.push_back(TAsset{HashId("a"), assetChannels, 48000, std::move(data), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(blocks);
}
std::vector<float> Tone(std::size_t frames, double hz, float amp) { return Sine(frames, hz, kRate, amp); }
}  // namespace

SND_TEST(CoreFxKinds, EchoPutsDecayingCopiesAtTheDelay) {
  Harness h;
  // 10 ms (480 frames), decay 0.6, dry 1, wet 0.8.
  RunOne(h, snd::dsp::kEffectEcho, {10.0f, 0.6f, 1.0f, 0.8f, 20.0f}, Impulse(48000), 16);
  for (int n = 1; n <= 5; ++n) {
    for (std::size_t j = 0; j < 6; ++j) {
      SND_CHECK_NEAR(h.At(480u * n + j, 0), h.At(j, 0) * 0.8f * std::pow(0.6f, float(n - 1)), 1e-5f);
    }
  }
}

SND_TEST(CoreFxKinds, DelayTimeFeedbackAndLevels) {
  Harness h;
  // 5 ms (240 frames), feedback 0.5, dry 0 dB, wet -6 dB, no damping, no ping-pong.
  RunOne(h, snd::dsp::kEffectDelay, {5.0f, 0.5f, 0.0f, -6.0f, 20000.0f, 0.0f, 20.0f}, Impulse(48000), 16);
  const float wet = std::pow(10.0f, -6.0f / 20.0f);
  for (int n = 1; n <= 4; ++n) {
    for (std::size_t j = 0; j < 6; ++j) {
      SND_CHECK_NEAR(h.At(240u * n + j, 0), h.At(j, 0) * wet * std::pow(0.5f, float(n - 1)), 2e-5f);
    }
  }
}

SND_TEST(CoreFxKinds, FlangerAndChorusAreCombFiltersAroundTheirDelay) {
  // Rate 0, depth 0: a fixed delay of 1 ms mixed with the dry signal: notches at 500 Hz
  // (odd multiples), peaks at 1 kHz (multiples).
  for (int chorus = 0; chorus < 2; ++chorus) {
    for (double hz : {500.0, 1000.0}) {
      Harness h;
      const std::vector<float> p = chorus != 0 ? std::vector<float>{0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f}
                                               : std::vector<float>{0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f};
      RunOne(h, chorus != 0 ? snd::dsp::kEffectChorus : snd::dsp::kEffectFlanger, p, Tone(96000, hz, 0.5f), 24);
      const double a = Amp(h, 2048, 4096, hz);
      snd::test::Note("%s %g Hz: %.4f", chorus != 0 ? "chorus" : "flanger", hz, a);
      if (hz == 500.0) {
        SND_CHECK(a < 0.5 * kCenter * 0.01);
      } else {
        SND_CHECK_NEAR(a, 0.5 * kCenter, 0.01);
      }
    }
  }
  // With depth and rate, the delay moves: the 500 Hz notch is not steady any more.
  Harness h;
  RunOne(h, snd::dsp::kEffectFlanger, {0.5f, 0.5f, 1.0f, 2.0f, 0.8f, 0.0f}, Tone(96000, 500.0, 0.5f), 120);
  double lo = 1e9, hi = 0;
  for (std::size_t w = 0; w < 24; ++w) {
    const double a = Amp(h, 4096 + w * 1024, 1024, 500.0);
    lo = std::min(lo, a);
    hi = std::max(hi, a);
  }
  SND_CHECK(hi > 0.2 && hi > lo * 3.0);
}

SND_TEST(CoreFxKinds, DistortionAddsOddHarmonicsWithDrive) {
  double h3[2];
  for (int k = 0; k < 2; ++k) {
    Harness h;
    RunOne(h, snd::dsp::kEffectDistortion, {k == 0 ? 0.0f : 1.0f, 1.0f, 0.0f, 20000.0f}, Tone(96000, 500.0, 0.8f), 24);
    h3[k] = Amp(h, 2048, 4096, 1500.0) / Amp(h, 2048, 4096, 500.0);
    SND_CHECK(MaxAbs(h, 0, h.TotalFrames()) <= 1.0001);
    SND_CHECK(Amp(h, 2048, 4096, 1000.0) < Amp(h, 2048, 4096, 1500.0) * 0.1 + 1e-4);  // odd-symmetric clipper
  }
  snd::test::Note("3rd harmonic: drive 0 %.1f dB, drive 1 %.1f dB", Db(h3[0]), Db(h3[1]));
  SND_CHECK(h3[1] > 0.05);           // clearly distorted
  SND_CHECK(h3[1] > h3[0] * 4.0);    // more drive, more harmonics
}

SND_TEST(CoreFxKinds, CompressorFollowsItsStaticCurve) {
  // DC 0.5 on the voice -> 0.5 * kCenter on the bus = -9.03 dBFS. Threshold -20, ratio 4,
  // knee 0, makeup 3: gain = -(11.03)(0.75) + 3 dB.
  Harness h;
  RunOne(h, snd::dsp::kEffectCompressor, {-20.0f, 4.0f, 1.0f, 50.0f, 0.0f, 3.0f, 0.0f, 0.0f}, Constant(96000, 0.5f), 40);
  const double in = 20.0 * std::log10(0.5 * kCenter);
  const double want = in + (-(in + 20.0) * 0.75 + 3.0);
  const double got = 20.0 * std::log10(std::fabs(h.At(39 * 256, 0)));
  snd::test::Note("compressor: in %.2f dB, out %.2f dB, static curve %.2f dB", in, got, want);
  SND_CHECK_NEAR(got, want, 0.05);
}

SND_TEST(CoreFxKinds, LimiterNeverExceedsTheCeiling) {
  Harness h;
  // Ceiling -6 dB, input +12 dB.
  RunOne(h, snd::dsp::kEffectLimiter, {-6.0f, 50.0f, 2.0f, 12.0f}, Tone(96000, 300.0, 0.9f), 60);
  const double m = MaxAbs(h, 0, h.TotalFrames());
  SND_CHECK(m <= std::pow(10.0, -6.0 / 20.0) * 1.0001);
  SND_CHECK(m > std::pow(10.0, -6.0 / 20.0) * 0.9);
}

SND_TEST(CoreFxKinds, NormalizeLiftsAQuietInput) {
  Harness h;
  // Fade-in 0 ms, lowest volume 0.01, maximum amplification 20.
  RunOne(h, snd::dsp::kEffectNormalize, {0.0f, 0.01f, 20.0f}, Tone(96000, 300.0, 0.01f), 80);
  const double in = 0.01 * kCenter;
  const double out = Amp(h, 80 * 256 - 4096, 4096, 300.0);
  snd::test::Note("normalize: %.4f -> %.4f", in, out);
  SND_CHECK(out > in * 10.0);   // lifted (up to the 20x limit)
  SND_CHECK(out <= in * 20.5);
}

SND_TEST(CoreFxKinds, PitchShifterMovesTheFrequencyByTheRatio) {
  Harness h;
  RunOne(h, snd::dsp::kEffectPitchShifter, {1.5f, 50.0f, 1.0f}, Tone(96000, 400.0, 0.5f), 48);
  const double shifted = Amp(h, 4096, 8192, 600.0), orig = Amp(h, 4096, 8192, 400.0);
  snd::test::Note("pitch 1.5: 600 Hz %.4f, 400 Hz %.4f", shifted, orig);
  SND_CHECK(shifted > 0.15);
  SND_CHECK(shifted > orig * 5.0);
}

SND_TEST(CoreFxKinds, SurroundFeedsTheRearChannelsOfASixChannelBus) {
  snd_config cfg = SmallConfig();
  cfg.output_channels = 6;
  Harness h(cfg);
  // Width 1, rear -6 dB, rear delay 10 ms, rear lowpass 20 kHz, centre off. A stereo source
  // with the tone on the left only: rear left = -6 dB of (L - R / 2) = L, rear right = -6 dB of -L / 2.
  std::vector<float> st(2 * 96000, 0.0f);
  const std::vector<float> tone = Tone(96000, 500.0, 0.5f);
  for (std::size_t i = 0; i < 96000; ++i) st[2 * i] = tone[i];
  RunOne(h, snd::dsp::kEffectSurround, {1.0f, -6.0f, 10.0f, 20000.0f, -60.0f}, st, 24, 6u, 2u);
  SND_REQUIRE_EQ(h.Ch(), 6u);
  const double front = Amp(h, 2048, 4096, 500.0, 0);
  const double rearL = Amp(h, 2048, 4096, 500.0, 4), rearR = Amp(h, 2048, 4096, 500.0, 5);
  snd::test::Note("surround: front %.4f, rears %.4f %.4f", front, rearL, rearR);
  SND_CHECK(front > 0.1);
  SND_CHECK_NEAR(Db(rearL / front), -6.0, 0.5);
  SND_CHECK_NEAR(Db(rearR / front), -12.0, 0.5);
  // Nothing in the rears during the rear delay.
  SND_CHECK(MaxAbs(h, 0, 400, 4) < 1e-6 && MaxAbs(h, 0, 400, 5) < 1e-6);
  // A stereo bus has no rear channel: the effect only widens it.
  Harness s;
  RunOne(s, snd::dsp::kEffectSurround, {1.0f, -6.0f, 10.0f, 20000.0f, -60.0f}, Tone(96000, 500.0, 0.5f), 24, 2u);
  SND_CHECK_EQ(s.Ch(), 2u);
  for (std::size_t f = 0; f < s.TotalFrames(); ++f) SND_CHECK(std::isfinite(s.At(f, 0)));
}

SND_TEST(CoreFxKinds, ParamEqPeakAndFiltersOnABus) {
  {
    Harness h;  // band 2 (500 Hz default) peaking +12 dB, Q 2; the others 0 dB
    RunOne(h, snd::dsp::kEffectParamEq,
           {1.0f, 100.0f, 0.707f, 0.0f, 1.0f, 1000.0f, 2.0f, 12.0f, 1.0f, 2000.0f, 0.707f, 0.0f, 1.0f, 8000.0f, 0.707f, 0.0f},
           Tone(96000, 1000.0, 0.1f), 24);
    SND_CHECK_NEAR(Db(Amp(h, 2048, 4096, 1000.0) / (0.1 * kCenter)), 12.0, 0.2);
  }
  {
    Harness h;
    std::vector<float> mix = Tone(96000, 100.0, 0.3f);
    const std::vector<float> hiTone = Tone(96000, 8000.0, 0.3f);
    for (std::size_t i = 0; i < mix.size(); ++i) mix[i] += hiTone[i];
    RunOne(h, snd::dsp::kEffectHighpass, {2000.0f, 0.707f}, mix, 24);
    SND_CHECK(Amp(h, 2048, 4096, 100.0) < 0.3 * kCenter * 0.01);
    SND_CHECK_NEAR(Amp(h, 2048, 4096, 8000.0), 0.3 * kCenter, 0.01);
  }
}

SND_TEST(CoreFxKinds, ReverbTailOutlastsTheInputAndDecays) {
  Harness h;
  // Dry off, reflections and late on, 1 s decay.
  RunOne(h, snd::dsp::kEffectReverb,
         {-10000.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, -1000.0f, 0.01f, 0.0f, 0.02f, 100.0f, 100.0f, 5000.0f, 250.0f},
         Impulse(48000), 200);
  auto energy = [&](double t0, double t1) {
    double e = 0;
    for (std::size_t f = std::size_t(t0 * kRate); f < std::size_t(t1 * kRate); ++f) e += double(h.At(f, 0)) * h.At(f, 0);
    return e;
  };
  const double e1 = energy(0.05, 0.25), e2 = energy(0.55, 0.75);
  snd::test::Note("reverb energy 50-250 ms %.3g, 550-750 ms %.3g (%.1f dB over 0.5 s)", e1, e2, 10.0 * std::log10(e2 / e1));
  SND_CHECK(e1 > 1e-4);
  // DecayTime 1 s: -60 dB per second, -30 dB over 0.5 s (within a few dB).
  SND_CHECK_NEAR(10.0 * std::log10(e2 / e1), -30.0, 5.0);
}
