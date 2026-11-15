// Independent effect tests, part 2: behaviour against analytic expectations
// from the parameter table in effects.hpp (delay times in samples, feedback
// decay, compressor static curve and time constants, limiter ceiling and
// latency, EQ / filter responses, pitch ratio, distortion symmetry, reverb RT60).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp_test_util.hpp"
#include "indep_fx_util.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/meter.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using namespace indepfx;
using dsptest::Audio;

namespace {

// Runs `fx` over a mono input vector in blocks; returns the output.
std::vector<float> RunMono(const Fx& fx, const std::vector<float>& x, std::uint32_t block = 256) {
  std::vector<float> y(x);
  for (std::size_t done = 0; done < y.size(); done += block) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, y.size() - done));
    float* p[1] = {y.data() + done};
    Run(fx, p, p, 1, n);
  }
  return y;
}

std::vector<float> Impulse(std::size_t n, float a = 1.0f, std::size_t at = 0) {
  std::vector<float> v(n, 0.0f);
  v[at] = a;
  return v;
}

std::vector<float> Sine(std::size_t n, double f, double rate, float amp) {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2 * dsptest::kPi * f * i / rate));
  return v;
}

double LsAmp(const std::vector<float>& y, std::size_t start, std::size_t n, double f, double fs) {
  double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
  for (std::size_t i = start; i < start + n; ++i) {
    const double w = 2 * dsptest::kPi * f * i / fs, s = std::sin(w), c = std::cos(w);
    ss += s * s; cc += c * c; sc += s * c; ys += y[i] * s; yc += y[i] * c;
  }
  const double det = ss * cc - sc * sc;
  return std::hypot((ys * cc - yc * sc) / det, (yc * ss - ys * sc) / det);
}

std::size_t ArgMaxAbs(const std::vector<float>& v, std::size_t from = 0) {
  std::size_t best = from;
  for (std::size_t i = from; i < v.size(); ++i)
    if (std::fabs(v[i]) > std::fabs(v[best])) best = i;
  return best;
}

}  // namespace

// Echo: echo k (k >= 1) of a unit impulse at k * Delay * fs / 1000 frames with
// amplitude WetMix * Decay^(k-1); fractional delays interpolate linearly.
SND_TEST(EffectsBehaviour, EchoTimingAndDecay) {
  for (std::uint32_t rate : {44100u, 48000u, 96000u}) {
    for (float ms : {10.0f, 37.5f, 123.4f}) {
      Fx fx(EffectKind::Echo, 1, 256, rate, {{0, ms}, {1, 0.6f}, {2, 1.0f}, {3, 0.8f}, {4, 1000.0f}});
      SND_REQUIRE(fx.ok);
      const double d = ms * rate / 1000.0;
      const auto y = RunMono(fx, Impulse(static_cast<std::size_t>(6 * d) + 10));
      SND_CHECK_EQ(y[0], 1.0f);
      // Echo k is the impulse through k fractional reads (linear interpolation
      // keeps the sum and the centroid): sum = WetMix * Decay^(k-1), centroid = k * d.
      for (int k = 1; k <= 5; ++k) {
        const double pos = k * d;
        const std::size_t lo = static_cast<std::size_t>(std::floor(pos)) - static_cast<std::size_t>(k) - 1;
        const std::size_t hi = static_cast<std::size_t>(std::floor(pos)) + static_cast<std::size_t>(k) + 2;
        double sum = 0.0, moment = 0.0;
        for (std::size_t i = lo; i <= hi; ++i) {
          sum += y[i];
          moment += y[i] * static_cast<double>(i);
        }
        const double expect = 0.8 * std::pow(0.6, k - 1);
        SND_CHECK_MSG(std::fabs(sum - expect) < 1e-5, "rate %u delay %.1f ms echo %d: sum %.6f, expected %.6f", rate,
                      static_cast<double>(ms), k, sum, expect);
        SND_CHECK_MSG(std::fabs(moment / sum - pos) < 1e-2, "rate %u delay %.1f ms echo %d: centroid %.4f, expected %.4f", rate,
                      static_cast<double>(ms), k, moment / sum, pos);
        if (std::floor(d) == d) SND_CHECK_NEAR(y[static_cast<std::size_t>(pos)], expect, 1e-5);
      }
      // Nothing between the echoes.
      float stray = 0.0f;
      for (std::size_t i = 1; i < static_cast<std::size_t>(5.5 * d); ++i) {
        const double ph = std::fmod(static_cast<double>(i), d);
        if (ph > 9.0 && ph < d - 9.0) stray = std::max(stray, std::fabs(y[i]));
      }
      SND_CHECK_MSG(stray < 1e-6f, "rate %u delay %.1f ms: %g between echoes", rate, static_cast<double>(ms), static_cast<double>(stray));
    }
  }
}

// Delay: first echo at Time, then Feedback per pass (damping open); dry/wet in dB;
// ping-pong alternates channels.
SND_TEST(EffectsBehaviour, DelayTimingFeedbackPingPong) {
  {
    Fx fx(EffectKind::Delay, 1, 256, 48000, {{0, 250.0f}, {1, 0.5f}, {2, 0.0f}, {3, -6.0f}, {4, 20000.0f}});
    const auto y = RunMono(fx, Impulse(48000));
    const double wet = std::pow(10.0, -6.0 / 20.0);
    SND_CHECK_EQ(y[0], 1.0f);
    SND_CHECK_NEAR(y[12000], wet, 1e-6);
    SND_CHECK_NEAR(y[24000], wet * 0.5, 1e-6);
    SND_CHECK_NEAR(y[36000], wet * 0.25, 1e-6);
    SND_CHECK_EQ(y[11999], 0.0f);
    SND_CHECK_EQ(y[12001], 0.0f);
  }
  {
    // Ping-pong 1: left impulse -> echo 1 left, echo 2 right, echo 3 left.
    Fx fx(EffectKind::Delay, 2, 256, 48000, {{0, 10.0f}, {1, 0.5f}, {2, 0.0f}, {3, 0.0f}, {4, 20000.0f}, {5, 1.0f}});
    Audio a(2, 2000);
    a.ch(0)[0] = 1.0f;
    for (std::uint32_t done = 0; done < 2000; done += 250) {
      float* p[2] = {a.ch(0) + done, a.ch(1) + done};
      Run(fx, p, p, 2, 250);
    }
    SND_CHECK_NEAR(a.ch(0)[480], 1.0, 1e-6);
    SND_CHECK_NEAR(a.ch(1)[480], 0.0, 1e-6);
    SND_CHECK_NEAR(a.ch(1)[960], 0.5, 1e-6);
    SND_CHECK_NEAR(a.ch(0)[960], 0.0, 1e-6);
    SND_CHECK_NEAR(a.ch(0)[1440], 0.25, 1e-6);
  }
  {
    // Time 0 ms (documented minimum): measured delay.
    Fx fx(EffectKind::Delay, 1, 256, 48000, {{0, 0.0f}, {1, 0.0f}, {2, -96.0f}, {3, 0.0f}});
    const auto y = RunMono(fx, Impulse(64));
    snd::test::Note("delay Time=0 ms: wet impulse at frame %zu", ArgMaxAbs(y));
  }
}

// Compressor: static curve on DC (hard and soft knee), attack / release time
// constants (1/e of the gain change in dB), stereo link, sidechain switches.
SND_TEST(EffectsBehaviour, CompressorCurveTimeConstantsLinkSidechain) {
  // Static curve, DC input, several thresholds / ratios / knees.
  for (float thr : {-30.0f, -10.0f}) {
    for (float ratio : {2.0f, 8.0f}) {
      for (float knee : {0.0f, 12.0f}) {
        for (float in : {-40.0f, -32.0f, -24.0f, -12.0f, -3.0f}) {
          Fx fx(EffectKind::Compressor, 1, 256, 48000, {{0, thr}, {1, ratio}, {2, 0.05f}, {3, 1.0f}, {4, knee}, {5, 0.0f}});
          std::vector<float> x(24000, DbToGain(in));
          const auto y = RunMono(fx, x);
          const double over = in - thr;
          double gc;
          if (knee > 0 && 2 * std::fabs(over) <= knee) {
            gc = (1.0 / ratio - 1.0) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
          } else {
            gc = over > 0 ? (1.0 / ratio - 1.0) * over : 0.0;
          }
          SND_CHECK_MSG(std::fabs(GainToDb(y.back()) - (in + gc)) < 0.02, "thr %.0f ratio %.0f knee %.0f in %.0f: out %.3f, expected %.3f",
                        static_cast<double>(thr), static_cast<double>(ratio), static_cast<double>(knee),
                        static_cast<double>(in), static_cast<double>(GainToDb(y.back())), in + gc);
        }
      }
    }
  }
  // Attack / release: DC step -40 -> -10 dB (target -7.5 dB change), then back.
  for (float att : {1.0f, 10.0f, 50.0f}) {
    for (float rel : {20.0f, 200.0f}) {
      Fx fx(EffectKind::Compressor, 1, 256, 48000, {{0, -20.0f}, {1, 4.0f}, {2, att}, {3, rel}, {4, 0.0f}});
      std::vector<float> x(48000 * 3, DbToGain(-40.0f));
      for (std::size_t i = 4800; i < 4800 + 48000; ++i) x[i] = DbToGain(-10.0f);
      const auto y = RunMono(fx, x);
      // Gain change g(t) = out_dB - in_dB; time to 1 - 1/e of -7.5 dB.
      auto timeTo = [&](std::size_t from, double target, bool falling) {
        for (std::size_t i = from; i < y.size(); ++i) {
          const double gc = GainToDb(y[i]) - GainToDb(x[i]);
          if (falling ? gc <= target : gc >= target) return static_cast<double>(i - from) / 48.0;
        }
        return -1.0;
      };
      const double ta = timeTo(4800, -7.5 * (1 - std::exp(-1.0)), true);
      const double tr = timeTo(4800 + 48000, -7.5 * std::exp(-1.0), false);
      snd::test::Note("compressor attack %.0f ms -> 63%% after %.2f ms; release %.0f ms -> 63%% after %.2f ms", static_cast<double>(att),
                      ta, static_cast<double>(rel), tr);
      SND_CHECK_MSG(std::fabs(ta - att) < 0.05 * att + 0.05, "attack %.0f ms measured %.2f ms", static_cast<double>(att), ta);
      SND_CHECK_MSG(std::fabs(tr - rel) < 0.05 * rel + 0.05, "release %.0f ms measured %.2f ms", static_cast<double>(rel), tr);
    }
  }
  // Stereo link: a loud left channel turns the quiet right channel down equally.
  {
    Fx fx(EffectKind::Compressor, 2, 256, 48000, {{0, -20.0f}, {1, 4.0f}, {2, 0.05f}, {3, 10.0f}, {4, 0.0f}});
    Audio a(2, 256);
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t i = 0; i < 256; ++i) {
        a.ch(0)[i] = 0.5f;
        a.ch(1)[i] = 0.01f;
      }
      Run(fx, a.cp(), a.p(), 2, 256);
    }
    SND_CHECK_NEAR(GainToDb(a.ch(0)[255]) - GainToDb(0.5f), GainToDb(a.ch(1)[255]) - GainToDb(0.01f), 1e-3);
    SND_CHECK(GainToDb(a.ch(1)[255]) - GainToDb(0.01f) < -10.0f);
  }
  // UseSidechain 0 ignores the sidechain; SidechainDb shifts the detector level.
  {
    Audio quiet(1, 256), voice(1, 256);
    for (std::uint32_t i = 0; i < 256; ++i) voice.ch(0)[i] = 1.0f;
    EffectSidechain sc{voice.cp(), 1};
    Fx off(EffectKind::Compressor, 1, 256, 48000, {{0, -20.0f}, {1, 4.0f}, {2, 0.05f}, {3, 10.0f}, {4, 0.0f}, {7, 0.0f}});
    Fx on(EffectKind::Compressor, 1, 256, 48000, {{0, -20.0f}, {1, 4.0f}, {2, 0.05f}, {3, 10.0f}, {4, 0.0f}, {6, -10.0f}, {7, 1.0f}});
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t i = 0; i < 256; ++i) quiet.ch(0)[i] = 0.05f;
      Run(off, quiet.cp(), quiet.p(), 1, 256, nullptr, &sc);
    }
    SND_CHECK_NEAR(quiet.ch(0)[255], 0.05, 1e-6);
    for (int b = 0; b < 40; ++b) {
      for (std::uint32_t i = 0; i < 256; ++i) quiet.ch(0)[i] = 0.05f;
      Run(on, quiet.cp(), quiet.p(), 1, 256, nullptr, &sc);
    }
    // Detector 0 dB - 10 dB = -10 dB: 10 dB over -20 at 4:1 -> -7.5 dB.
    SND_CHECK_NEAR(GainToDb(quiet.ch(0)[255]) - GainToDb(0.05f), -7.5, 0.02);
  }
}

// Limiter: never above the ceiling, for every look-ahead, input gain and rate;
// EffectLatencyFrames equals the measured delay; quiet input passes unchanged.
SND_TEST(EffectsBehaviour, LimiterCeilingAndLatency) {
  for (std::uint32_t rate : {44100u, 96000u}) {
    for (float look : {0.0f, 0.5f, 2.0f, 10.0f}) {
      for (float ceilDb : {-0.3f, -12.0f}) {
        Fx fx(EffectKind::Limiter, 2, 128, rate, {{0, ceilDb}, {1, 5.0f}, {2, look}, {3, 24.0f}});
        SND_REQUIRE(fx.ok);
        const std::uint32_t lat = EffectLatencyFrames(fx.state());
        const std::uint32_t expectLat = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::ceil(look * rate / 1000.0))) - 1u;
        SND_CHECK_EQ(lat, expectLat);
        dsptest::Noise nz(rate);
        Audio a(2, 128);
        float maxOut = 0.0f;
        const float ceil = DbToGain(ceilDb);
        for (int b = 0; b < 600; ++b) {
          for (std::uint32_t c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < 128; ++i) {
              float v = nz.Next(0.05f);
              if ((b * 128 + i) % 997 == 0) v = c ? -1.0f : 1.0f;  // isolated full-scale clicks
              if (b % 50 > 40) v *= 20.0f;
              a.ch(c)[i] = v;
            }
          Run(fx, a.cp(), a.p(), 2, 128);
          maxOut = std::max(maxOut, MaxAbs(a, 128));
        }
        SND_CHECK_MSG(maxOut <= ceil * (1.0f + 1e-6f), "rate %u look %.1f ms ceiling %.1f dB: peak %.6f > %.6f", rate,
                      static_cast<double>(look), static_cast<double>(ceilDb), static_cast<double>(maxOut),
                      static_cast<double>(ceil));
        // Latency: an impulse below the ceiling (input gain 0) comes out `lat` frames later.
        Fx q(EffectKind::Limiter, 1, 128, rate, {{0, 0.0f}, {1, 50.0f}, {2, look}, {3, 0.0f}});
        const auto y = RunMono(q, Impulse(4096, 0.5f, 100), 128);
        SND_CHECK_EQ(ArgMaxAbs(y), 100u + EffectLatencyFrames(q.state()));
        SND_CHECK_NEAR(y[100 + EffectLatencyFrames(q.state())], 0.5, 1e-6);
      }
    }
  }
}

// effects.hpp: "Latency in frames (limiter look-ahead), 0 for the others".
// With Pitch = 1 the two-tap shifter reads one tap at delay 0 (weight 0) and
// the other at WindowMs / 2 (weight 1): the output is the input delayed by
// WindowMs / 2 and, with Mix < 1, a comb filter instead of the dry signal.
SND_TEST(EffectsBehaviour, PitchShifterUnityPitchIsTransparent) {
  Fx fx(EffectKind::PitchShifter, 1, 256, 48000, {{0, 1.0f}, {1, 50.0f}, {2, 1.0f}});
  const auto y = RunMono(fx, Impulse(8192, 1.0f, 10));
  const std::size_t at = ArgMaxAbs(y);
  snd::test::Note("pitch 1, window 50 ms: impulse at 10 comes out at %zu (reported latency %u)", at,
                  EffectLatencyFrames(fx.state()));
  SND_CHECK_EQ(at - 10u, static_cast<std::size_t>(EffectLatencyFrames(fx.state())));
  // Mix 0.5 at pitch 1: should be the input (dry + wet at the same pitch).
  Fx half(EffectKind::PitchShifter, 1, 256, 48000, {{0, 1.0f}, {1, 50.0f}, {2, 0.5f}});
  const auto z = RunMono(half, Sine(48000, 410.0, 48000.0, 0.5f));
  const double amp = LsAmp(z, 12000, 24000, 410.0, 48000.0);
  snd::test::Note("pitch 1, mix 0.5: 410 Hz sine comes out at %.2f dB", dsptest::Db(amp / 0.5));
  SND_CHECK_MSG(std::fabs(dsptest::Db(amp / 0.5)) < 0.5, "pitch 1, mix 0.5: 410 Hz at %.2f dB (comb filter)", dsptest::Db(amp / 0.5));
}

// Pitch shifter: the dominant output frequency is Pitch x input frequency.
SND_TEST(EffectsBehaviour, PitchShifterFrequencyRatio) {
  for (float pitch : {0.5f, 0.75f, 1.25f, 1.5f, 2.0f}) {
    Fx fx(EffectKind::PitchShifter, 1, 256, 48000, {{0, pitch}, {1, 50.0f}, {2, 1.0f}});
    const auto y = RunMono(fx, Sine(48000 * 2, 440.0, 48000.0, 0.5f));
    // Scan 200..1000 Hz in 2 Hz steps for the strongest component.
    double bestF = 0, bestA = 0;
    for (double f = 200; f <= 1000; f += 2) {
      const double a = LsAmp(y, 24000, 65536, f, 48000.0);
      if (a > bestA) {
        bestA = a;
        bestF = f;
      }
    }
    SND_CHECK_MSG(std::fabs(bestF - 440.0 * pitch) <= 0.01 * 440.0 * pitch, "pitch %.2f: strongest %.0f Hz, expected %.0f Hz",
                  static_cast<double>(pitch), bestF, 440.0 * pitch);
  }
}

// Parametric EQ: each band type maps to the documented biquad; the cascade's
// response is the product of the band responses.
SND_TEST(EffectsBehaviour, ParamEqResponseIsProductOfBands) {
  const BiquadType map[8] = {BiquadType::Peaking, BiquadType::Peaking,  BiquadType::LowShelf, BiquadType::HighShelf,
                             BiquadType::Lowpass, BiquadType::Highpass, BiquadType::Bandpass, BiquadType::Notch};
  for (int t = 1; t <= 7; ++t) {
    Fx fx(EffectKind::ParamEq, 1, 256, 48000,
          {{0, static_cast<float>(t)}, {1, 900.0f}, {2, 1.5f}, {3, 8.0f},  // band 1: type under test
           {4, 1.0f}, {5, 3000.0f}, {6, 2.0f}, {7, -6.0f},                 // band 2: peaking
           {8, 0.0f}, {12, 0.0f}});                                         // bands 3, 4 off
    const BiquadCoefs c1 = DesignBiquad(map[t], 900.0f, 1.5f, 8.0f, 48000.0f);
    const BiquadCoefs c2 = DesignBiquad(BiquadType::Peaking, 3000.0f, 2.0f, -6.0f, 48000.0f);
    for (double f : {100.0, 900.0, 2000.0, 3000.0, 9000.0}) {
      const auto y = RunMono(fx, Sine(30000, f, 48000.0, 0.25f));
      const double got = dsptest::Db(LsAmp(y, 8000, 20000, f, 48000.0) / 0.25);
      const double expect = dsptest::Db(BiquadMagnitude(c1, f, 48000.0) * BiquadMagnitude(c2, f, 48000.0));
      if (expect < -60.0) continue;
      SND_CHECK_MSG(std::fabs(got - expect) < 0.05, "EQ type %d at %.0f Hz: %.3f dB, expected %.3f dB", t, f, got, expect);
    }
  }
}

// Lowpass / highpass effects: cookbook response with Resonance = Q.
SND_TEST(EffectsBehaviour, LowpassHighpassResponse) {
  for (EffectKind k : {EffectKind::Lowpass, EffectKind::Highpass}) {
    const BiquadType bt = k == EffectKind::Lowpass ? BiquadType::Lowpass : BiquadType::Highpass;
    for (float fc : {200.0f, 2000.0f, 15000.0f}) {
      for (float q : {0.5f, 1.0f, 10.0f}) {
        Fx fx(k, 1, 256, 48000, {{0, fc}, {1, q}});
        const BiquadCoefs c = DesignBiquad(bt, fc, q, 0.0f, 48000.0f);
        for (double f : {static_cast<double>(fc) / 4, static_cast<double>(fc), std::min(20000.0, static_cast<double>(fc) * 3)}) {
          const auto y = RunMono(fx, Sine(30000, f, 48000.0, 0.1f));
          const double got = dsptest::Db(LsAmp(y, 10000, 20000, f, 48000.0) / 0.1);
          const double expect = dsptest::Db(BiquadMagnitude(c, f, 48000.0));
          SND_CHECK_MSG(std::fabs(got - expect) < 0.05, "%s fc %.0f q %.1f at %.0f Hz: %.3f dB, expected %.3f dB", Short(k),
                        static_cast<double>(fc), static_cast<double>(q), f, got, expect);
        }
        SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(c, fc, 48000.0)), dsptest::Db(q), 0.01);
      }
    }
  }
}

// Distortion: odd symmetric (bit-exact), Drive 0 = 0 dB small-signal gain,
// output bounded by the soft clipper (|y| <= OutputGain), Mix and OutputDb.
SND_TEST(EffectsBehaviour, DistortionSymmetryAndLevels) {
  for (float drive : {0.0f, 0.5f, 1.0f}) {
    for (float tone : {800.0f, 20000.0f}) {
      Fx a(EffectKind::Distortion, 1, 256, 48000, {{0, drive}, {3, tone}}), b(EffectKind::Distortion, 1, 256, 48000, {{0, drive}, {3, tone}});
      auto x = Sine(9600, 330.0, 48000.0, 0.8f);
      dsptest::Noise nz(4);
      for (float& v : x) v += nz.Next(0.2f);
      std::vector<float> nx(x);
      for (float& v : nx) v = -v;
      const auto y = RunMono(a, x), ny = RunMono(b, nx);
      std::uint32_t asym = 0;
      float peak = 0.0f;
      for (std::size_t i = 0; i < y.size(); ++i) {
        asym += y[i] != -ny[i];
        peak = std::max(peak, std::fabs(y[i]));
      }
      SND_CHECK_MSG(asym == 0, "drive %.1f tone %.0f: %u asymmetric samples", static_cast<double>(drive), static_cast<double>(tone), asym);
      SND_CHECK(peak <= 1.0f + 1e-6f);
    }
  }
  Fx clean(EffectKind::Distortion, 1, 256, 48000, {{0, 0.0f}, {1, 1.0f}, {2, 6.0f}, {3, 20000.0f}});
  const auto y = RunMono(clean, Sine(9600, 330.0, 48000.0, 0.01f));
  SND_CHECK_NEAR(dsptest::Db(LsAmp(y, 1000, 8000, 330.0, 48000.0) / 0.01), 6.0, 0.01);
}

// Reverb: the late decay follows DecayTime (T30 from the Schroeder backward
// integral, DecayHFRatio 1 so the decay is frequency independent).
SND_TEST(EffectsBehaviour, ReverbRt60FollowsDecayTime) {
  for (float t60 : {0.5f, 1.49f, 4.0f}) {
    Fx fx(EffectKind::Reverb, 1, 256, 48000, {{0, -10000.0f}, {2, 0.0f}, {4, t60}, {5, 1.0f}, {6, -10000.0f}});
    const std::size_t n = static_cast<std::size_t>(48000 * t60 * 1.6f);
    const auto y = RunMono(fx, Impulse(n));
    std::vector<double> edc(n + 1, 0.0);
    for (std::size_t i = n; i-- > 0;) edc[i] = edc[i + 1] + static_cast<double>(y[i]) * y[i];
    auto tAt = [&](double db) {
      for (std::size_t i = 0; i < n; ++i)
        if (10.0 * std::log10(edc[i] / edc[0]) <= db) return static_cast<double>(i) / 48000.0;
      return -1.0;
    };
    const double t5 = tAt(-5.0), t35 = tAt(-35.0);
    const double measured = 2.0 * (t35 - t5);  // T30 extrapolated to 60 dB
    snd::test::Note("reverb DecayTime %.2f s: measured T30 -> RT60 %.3f s", static_cast<double>(t60), measured);
    SND_CHECK_MSG(std::fabs(measured - t60) < 0.15 * t60, "DecayTime %.2f s, measured %.3f s", static_cast<double>(t60), measured);
  }
}

// Surround: Width 0 -> mono fronts, Width 2 -> double side; 5.1 rear delay =
// RearDelayMs; centre at CenterDb of (L+R)/2; LFE untouched.
SND_TEST(EffectsBehaviour, SurroundWidthRearsCentre) {
  Audio a(6, 4800);
  dsptest::Noise nz(2);
  for (std::uint32_t i = 0; i < 4800; ++i) {
    a.ch(0)[i] = 0.0f;
    a.ch(1)[i] = 0.0f;
    a.ch(3)[i] = nz.Next(0.3f);
  }
  a.ch(0)[100] = 1.0f;  // left impulse
  std::vector<float> lfe(a.ch(3), a.ch(3) + 4800);
  Fx fx(EffectKind::Surround, 6, 256, 48000, {{0, 2.0f}, {1, 0.0f}, {2, 20.0f}, {3, 20000.0f}, {4, -6.0f}});
  for (std::uint32_t done = 0; done < 4800; done += 240) {
    float* p[6];
    for (std::uint32_t c = 0; c < 6; ++c) p[c] = a.ch(c) + done;
    Run(fx, p, p, 6, 240);
  }
  // Width 2: L' = mid + 2 side = 0.5 + 1.0 = 1.5, R' = 0.5 - 1.0 = -0.5.
  SND_CHECK_NEAR(a.ch(0)[100], 1.5, 1e-6);
  SND_CHECK_NEAR(a.ch(1)[100], -0.5, 1e-6);
  SND_CHECK_NEAR(a.ch(2)[100], 0.5 * DbToGain(-6.0f), 1e-6);
  for (std::uint32_t i = 0; i < 4800; ++i) SND_CHECK_EQ(a.ch(3)[i], lfe[i]);
  // Rear L (ch 4) gets L - 0.5 R through the (20 kHz) lowpass, delayed by 20 ms = 960 frames.
  std::vector<float> rear(a.ch(4), a.ch(4) + 4800);
  const std::size_t at = ArgMaxAbs(rear);
  snd::test::Note("surround rear peak at %zu (impulse at 100, RearDelayMs 20 = 960 frames)", at);
  SND_CHECK(at >= 100 + 960 && at <= 100 + 962);
}

// Normalize: steady gain = min(MaximumAmp, 1 / max(peak, LowestVolume)).
SND_TEST(EffectsBehaviour, NormalizeSteadyGain) {
  struct C {
    float amp, lowest, maxAmp;
  };
  for (const C& c : {C{0.05f, 0.1f, 20.0f}, C{0.2f, 0.1f, 20.0f}, C{0.01f, 0.0f, 20.0f}, C{0.8f, 0.0f, 100.0f}}) {
    Fx fx(EffectKind::Normalize, 1, 256, 48000, {{0, 50.0f}, {1, c.lowest}, {2, c.maxAmp}});
    const auto y = RunMono(fx, Sine(48000 * 2, 500.0, 48000.0, c.amp));
    const double g = std::min(static_cast<double>(c.maxAmp), 1.0 / std::max(static_cast<double>(c.amp), static_cast<double>(c.lowest)));
    const double got = LsAmp(y, 48000, 48000, 500.0, 48000.0) / c.amp;
    SND_CHECK_MSG(std::fabs(got / g - 1.0) < 0.01, "amp %.2f lowest %.2f max %.0f: gain %.3f, expected %.3f",
                  static_cast<double>(c.amp), static_cast<double>(c.lowest), static_cast<double>(c.maxAmp), got, g);
  }
}

// Echo Decay = 1 (documented maximum) is a lossless loop: with sustained input
// whose period matches the delay the level grows without bound (measured).
SND_TEST(EffectsBehaviour, EchoMaxDecaySustainedInputMeasured) {
  Fx fx(EffectKind::Echo, 1, 256, 48000, {{0, 10.0f}, {1, 1.0f}, {2, 1.0f}, {3, 1.0f}, {4, 100.0f}});
  const auto y = RunMono(fx, Sine(48000 * 10, 100.0, 48000.0, 0.1f));  // 10 ms = one period of 100 Hz
  float p1 = 0.0f, p10 = 0.0f;
  for (std::size_t i = 0; i < 48000; ++i) p1 = std::max(p1, std::fabs(y[i]));
  for (std::size_t i = 48000 * 9; i < y.size(); ++i) p10 = std::max(p10, std::fabs(y[i]));
  snd::test::Note("echo Decay=1, 10 ms, 0.1 sine at 100 Hz: peak %.1f after 1 s, %.1f after 10 s", static_cast<double>(p1),
                  static_cast<double>(p10));
}
