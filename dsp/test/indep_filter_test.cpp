// Independent filter tests (filter.hpp): biquads against closed-form RBJ
// properties and a double-precision reference design, stability of the float
// coefficients over the whole documented range, coefficient sweeps, the voice
// filter against the bilinear Butterworth response, pass-through when open,
// NaN / denormal behaviour.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/filter.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

const BiquadType kTypes[] = {BiquadType::Lowpass,  BiquadType::Highpass, BiquadType::Bandpass,  BiquadType::Notch,
                             BiquadType::Peaking,  BiquadType::LowShelf, BiquadType::HighShelf, BiquadType::Allpass};
const char* TName(BiquadType t) {
  static const char* n[] = {"LP", "HP", "BP", "Notch", "Peak", "LowShelf", "HighShelf", "AP"};
  return n[static_cast<int>(t)];
}

// Cookbook (Bristow-Johnson) magnitude in double, written from the formulas.
double RefMagnitude(BiquadType type, double f0, double q, double gainDb, double fs, double f) {
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2 * dsptest::kPi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
  const bool shelf = type == BiquadType::LowShelf || type == BiquadType::HighShelf;
  const double alpha = (shelf && q <= 0.0) ? sw / 2.0 * std::sqrt(2.0) : sw / (2.0 * q);
  double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  const double sa = 2 * std::sqrt(A) * alpha;
  switch (type) {
    case BiquadType::Lowpass: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BiquadType::Highpass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BiquadType::Bandpass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BiquadType::Notch: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BiquadType::Peaking: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
    case BiquadType::LowShelf:
      b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
      a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
      break;
    case BiquadType::HighShelf:
      b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
      break;
    case BiquadType::Allpass: b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
  }
  const std::complex<double> z1 = std::polar(1.0, -2 * dsptest::kPi * f / fs), z2 = z1 * z1;
  return std::abs((b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2));
}


// Least-squares amplitude of a sine of frequency f in y[start, start + n).
double SineAmp(const float* y, std::uint32_t start, std::uint32_t n, double f, double fs) {
  double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
  for (std::uint32_t i = start; i < start + n; ++i) {
    const double w = 2 * dsptest::kPi * f * i / fs, s = std::sin(w), c = std::cos(w);
    ss += s * s; cc += c * c; sc += s * c; ys += y[i] * s; yc += y[i] * c;
  }
  const double det = ss * cc - sc * sc;
  const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
  return std::hypot(a, b);
}

// Steady-state gain of BiquadProcess for a sine.
double ProcessedGain(const BiquadCoefs& c, double f, double fs, std::uint32_t settle) {
  BiquadState st;
  BiquadReset(st);
  const std::uint32_t m = 16000;
  const std::uint32_t n = settle + m;
  std::vector<float> x(n), y(n);
  for (std::uint32_t i = 0; i < n; ++i) x[i] = 0.25f * static_cast<float>(std::sin(2 * dsptest::kPi * f * i / fs));
  const float* in[1] = {x.data()};
  float* out[1] = {y.data()};
  BiquadProcess(st, c, c, in, out, 1, n);
  return SineAmp(y.data(), settle, m, f, fs) / 0.25;
}

bool StableD(const BiquadCoefs& c) {
  const double a1 = c.a1, a2 = c.a2;
  return std::fabs(a2) < 1.0 && std::fabs(a1) < 1.0 + a2;
}

}  // namespace

// Closed-form properties of the cookbook designs, evaluated by BiquadMagnitude
// on the float coefficients DesignBiquad returns.
SND_TEST(BiquadIndep, CookbookIdentities) {
  // w0 not tiny (float coefficient precision is measured separately below).
  for (double fs : {44100.0, 48000.0, 96000.0}) {
    for (double f0 : {1000.0, 5000.0}) {
      for (double q : {0.5, 0.7071, 2.0, 8.0}) {
        const float ff = static_cast<float>(f0), qq = static_cast<float>(q), sr = static_cast<float>(fs);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Lowpass, ff, qq, 0, sr), f0, fs), q, 1e-3 * q);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Lowpass, ff, qq, 0, sr), 0.0, fs), 1.0, 1e-3);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Lowpass, ff, qq, 0, sr), fs / 2, fs), 0.0, 1e-3);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Highpass, ff, qq, 0, sr), f0, fs), q, 1e-3 * q);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Highpass, ff, qq, 0, sr), fs / 2, fs), 1.0, 1e-3);
        SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Bandpass, ff, qq, 0, sr), f0, fs), 1.0, 1e-3);
        SND_CHECK(BiquadMagnitude(DesignBiquad(BiquadType::Notch, ff, qq, 0, sr), f0, fs) < 3e-3);
        for (double g : {-24.0, -6.0, 6.0, 24.0}) {
          const float gg = static_cast<float>(g);
          SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::Peaking, ff, qq, gg, sr), f0, fs)), g, 0.01);
          // Shelf midpoint: half the gain in dB (cookbook).
          SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::LowShelf, ff, qq, gg, sr), f0, fs)), g / 2, 0.01);
          SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::HighShelf, ff, qq, gg, sr), f0, fs)), g / 2, 0.01);
          SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::LowShelf, ff, 0.0f, gg, sr), 0.0, fs)), g, 0.05);
          SND_CHECK_NEAR(dsptest::Db(BiquadMagnitude(DesignBiquad(BiquadType::HighShelf, ff, 0.0f, gg, sr), fs / 2, fs)), g, 0.05);
        }
        for (double f = 10.0; f < fs / 2; f *= 1.7)
          SND_CHECK_NEAR(BiquadMagnitude(DesignBiquad(BiquadType::Allpass, ff, qq, 0, sr), f, fs), 1.0, 1e-4);
      }
    }
  }
}

// Every type against the double-precision cookbook reference over a grid of
// frequencies; and the processed sine against BiquadMagnitude.
SND_TEST(BiquadIndep, MatchesDoubleReferenceAndProcessing) {
  const double fs = 48000.0;
  double worstDesign = 0.0, worstProc = 0.0;
  for (BiquadType t : kTypes) {
    for (double f0 : {200.0, 2000.0, 12000.0}) {
      for (double q : {0.3, 1.0, 5.0}) {
        const double g = 9.0;
        const BiquadCoefs c = DesignBiquad(t, static_cast<float>(f0), static_cast<float>(q), static_cast<float>(g), 48000.0f);
        for (double f : {50.0, 300.0, 1000.0, 3000.0, 9000.0, 15000.0, 21000.0}) {
          const double ref = RefMagnitude(t, f0, q, g, fs, f);
          const double got = BiquadMagnitude(c, f, fs);
          const double err = std::fabs(dsptest::Db(got) - dsptest::Db(ref));
          if (ref > 1e-3) worstDesign = std::max(worstDesign, err);
          SND_CHECK_MSG(ref <= 1e-3 || err < 0.01, "%s f0 %.0f q %.1f at %.0f Hz: %.4f dB vs ref %.4f dB", TName(t), f0, q, f,
                        dsptest::Db(got), dsptest::Db(ref));
          if (f == 300.0 || f == 3000.0 || f == 15000.0) {
            const double proc = ProcessedGain(c, f, fs, 4096);
            const double perr = std::fabs(dsptest::Db(proc) - dsptest::Db(got));
            if (got > 1e-2) {
              worstProc = std::max(worstProc, perr);
              SND_CHECK_MSG(perr < 0.02, "%s f0 %.0f q %.1f: processed %.4f dB vs %.4f dB at %.0f Hz", TName(t), f0, q,
                            dsptest::Db(proc), dsptest::Db(got), f);
            }
          }
        }
      }
    }
  }
  snd::test::Note("worst design error %.5f dB, worst processed-vs-magnitude %.5f dB", worstDesign, worstProc);
}

// filter.hpp: freqHz clamped to [1 Hz, 0.49 fs], q to [0.05, 100]; BiquadProcess
// relies on the (a1, a2) stability triangle. The float coefficients of every
// design in that range must be strictly inside it (|a2| < 1, |a1| < 1 + a2).
SND_TEST(BiquadIndep, FloatCoefficientsStableDownTo1Hz) {
  std::uint32_t unstable = 0;
  double highestBad = 0.0;
  for (double fs : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
    for (double f0 : {1.0, 2.0, 3.0, 5.0, 10.0, 20.0, 50.0, 0.25 * fs, 0.45 * fs, 0.49 * fs, fs}) {
      for (double q : {0.05, 0.1, 0.7071, 10.0, 100.0}) {
        for (BiquadType t : kTypes) {
          const BiquadCoefs c = DesignBiquad(t, static_cast<float>(f0), static_cast<float>(q), 12.0f, static_cast<float>(fs));
          if (!StableD(c)) {
            highestBad = std::max(highestBad, f0);
            if (unstable++ < 6)
              SND_CHECK_MSG(false, "%s f0 %.2f Hz q %.2f fs %.0f: a1 %.9g a2 %.9g, 1 + a1 + a2 = %.3g (not inside the stability triangle)",
                            TName(t), f0, q, fs, static_cast<double>(c.a1), static_cast<double>(c.a2),
                            1.0 + static_cast<double>(c.a1) + static_cast<double>(c.a2));
          }
        }
      }
    }
  }
  snd::test::Note("%u unstable / marginal float designs, highest cutoff affected %.1f Hz", unstable, highestBad);
  SND_CHECK_EQ(unstable, 0u);
}

// Unit DC gain of the cookbook lowpass at the lowest cutoffs the standard
// effects accept (snd.fx.lowpass 10 Hz, snd.fx.parameq 20 Hz) at 44.1..192 kHz.
// Float direct-form coefficients lose 1 + a1 + a2 ~ w0^2 to rounding.
SND_TEST(BiquadIndep, LowCutoffLowpassKeepsUnitDcGain) {
  for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
    for (double f0 : {10.0, 20.0}) {
      const BiquadCoefs lp = DesignBiquad(BiquadType::Lowpass, static_cast<float>(f0), 0.7071f, 0.0f, static_cast<float>(fs));
      const double dc = dsptest::Db(BiquadMagnitude(lp, 0.0, fs));
      const double proc = dsptest::Db(ProcessedGain(lp, 1.0, fs, static_cast<std::uint32_t>(fs)));
      snd::test::Note("lowpass %.0f Hz @ %.0f: DC gain %+.3f dB (processed 1 Hz: %+.3f dB), |H(f0)| %.3f dB (ideal -3.010)", f0,
                      fs, dc, proc, dsptest::Db(BiquadMagnitude(lp, f0, fs)));
      SND_CHECK_MSG(std::fabs(dc) < 0.5, "lowpass %.0f Hz at %.0f Hz: DC gain %+.3f dB", f0, fs, dc);
    }
  }
}

// Coefficient interpolation between extreme designs every block: bounded output.
SND_TEST(BiquadIndep, ExtremeSweepsStayBounded) {
  BiquadState st;
  BiquadReset(st);
  dsptest::Noise nz(3);
  std::vector<float> x(64);
  BiquadCoefs prev = DesignBiquad(BiquadType::Lowpass, 20.0f, 100.0f, 0.0f, 48000.0f);
  float worst = 0.0f;
  for (int b = 0; b < 4000; ++b) {
    const BiquadType t = kTypes[b % 8];
    const float f = (b / 8) % 2 ? 23000.0f : 5.0f + static_cast<float>(b % 13) * 50.0f;
    const float q = (b / 3) % 2 ? 100.0f : 0.05f;
    const BiquadCoefs next = DesignBiquad(t, f, q, (b % 5) * 12.0f - 24.0f, 48000.0f);
    for (float& v : x) v = nz.Next(1.0f);
    const float* in[1] = {x.data()};
    float* out[1] = {x.data()};  // in place
    {
      SND_RT_SCOPE();
      BiquadProcess(st, prev, next, in, out, 1, 64);
    }
    for (float v : x) {
      if (!std::isfinite(v)) worst = INFINITY;
      worst = std::max(worst, std::fabs(v));
    }
    prev = next;
  }
  snd::test::Note("extreme coefficient sweeps: peak output %.1f", static_cast<double>(worst));
  SND_CHECK(std::isfinite(worst));
  SND_CHECK(worst < 1.0e4f);
}

// Ramp split: a constant design gives bit-identical output whatever the split.
SND_TEST(BiquadIndep, ConstantCoefficientsSplitInvariantAndMultiChannel) {
  const BiquadCoefs c = DesignBiquad(BiquadType::Peaking, 1234.0f, 3.0f, 7.0f, 48000.0f);
  Audio a(8, 999), b(8, 999);
  dsptest::Noise nz(9);
  for (std::uint32_t i = 0; i < 999; ++i) {
    const float v = nz.Next();
    for (std::uint32_t ch = 0; ch < 8; ++ch) a.ch(ch)[i] = b.ch(ch)[i] = v;
  }
  BiquadState s1, s2;
  BiquadReset(s1);
  BiquadReset(s2);
  BiquadProcess(s1, c, c, a.cp(), a.p(), 8, 999);
  const std::uint32_t chunks[] = {1, 2, 5, 17, 64, 1, 300, 609};
  std::uint32_t done = 0;
  for (std::uint32_t n : chunks) {
    const float* in[8];
    float* out[8];
    for (std::uint32_t ch = 0; ch < 8; ++ch) {
      out[ch] = b.ch(ch) + done;
      in[ch] = out[ch];
    }
    BiquadProcess(s2, c, c, in, out, 8, n);
    done += n;
  }
  SND_REQUIRE_EQ(done, 999u);
  std::uint32_t diff = 0;
  for (std::uint32_t ch = 0; ch < 8; ++ch) diff += dsptest::MaxAbsDiff(a.ch(ch), b.ch(ch), 999) != 0.0f;
  for (std::uint32_t ch = 1; ch < 8; ++ch) diff += dsptest::MaxAbsDiff(a.ch(0), a.ch(ch), 999) != 0.0f;
  SND_CHECK_EQ(diff, 0u);
}

SND_TEST(BiquadIndep, OctaveBandwidthToQ) {
  for (float n : {0.1f, 0.5f, 1.0f, 2.0f, 3.0f}) {
    const double p = std::pow(2.0, static_cast<double>(n));
    SND_CHECK_NEAR(OctaveBandwidthToQ(n), std::sqrt(p) / (p - 1.0), 1e-4 * std::sqrt(p) / (p - 1.0));
  }
  SND_CHECK_NEAR(OctaveBandwidthToQ(1.0f), 1.41421356, 1e-5);
}

// A long silence after an impulse: no denormals reach the output or the state,
// even without FTZ/DAZ ("BiquadFlush ... no denormals").
SND_TEST(BiquadIndep, NoDenormalsInDecay) {
  for (BiquadType t : kTypes) {
    const BiquadCoefs c = DesignBiquad(t, 30.0f, 20.0f, 12.0f, 48000.0f);
    BiquadState st;
    BiquadReset(st);
    std::vector<float> x(4096, 0.0f);
    x[0] = 1.0f;
    bool den = false;
    for (int b = 0; b < 200; ++b) {
      const float* in[1] = {x.data()};
      float* out[1] = {x.data()};
      BiquadProcess(st, c, c, in, out, 1, 4096);
      den |= HasDenormal(x.data(), 4096);
      den |= std::fpclassify(st.z1[0]) == FP_SUBNORMAL || std::fpclassify(st.z2[0]) == FP_SUBNORMAL;
      std::fill(x.begin(), x.end(), 0.0f);
    }
    SND_CHECK_MSG(!den, "%s: denormal in the decay", TName(t));
    // Not documented: the float TDF-II may settle in a tiny zero-input limit
    // cycle instead of exact zero (harmless: far above the denormal range).
    if (st.z1[0] != 0.0f || st.z2[0] != 0.0f)
      snd::test::Note("%s 30 Hz Q 20: zero-input residue %.3g / %.3g after 17 s", TName(t), static_cast<double>(st.z1[0]),
                      static_cast<double>(st.z2[0]));
  }
}

// NaN behaviour (not documented): one NaN sample. Reported, then Reset recovers.
SND_TEST(BiquadIndep, NanSampleBehaviour) {
  const BiquadCoefs c = DesignBiquad(BiquadType::Lowpass, 1000.0f, 0.7f, 0.0f, 48000.0f);
  BiquadState st;
  BiquadReset(st);
  std::vector<float> x(48000, 0.1f);
  x[10] = std::numeric_limits<float>::quiet_NaN();
  const float* in[1] = {x.data()};
  float* out[1] = {x.data()};
  BiquadProcess(st, c, c, in, out, 1, 48000);
  const bool recovered = std::isfinite(x.back());
  snd::test::Note("biquad: after one NaN input sample the output %s within 1 s", recovered ? "recovers" : "stays NaN");
  BiquadReset(st);
  std::fill(x.begin(), x.end(), 0.1f);
  BiquadProcess(st, c, c, in, out, 1, 48000);
  SND_CHECK(!HasNonFinite(x.data(), 48000));

  snd::FilterState vf;
  VoiceFilterInit(vf, snd::FilterTarget{2000.0f, 50.0f});
  Audio a(1, 4800);
  for (std::uint32_t i = 0; i < 4800; ++i) a.ch(0)[i] = 0.1f;
  a.ch(0)[3] = std::numeric_limits<float>::quiet_NaN();
  VoiceFilterProcess(vf, a.p(), 1, 4800, snd::FilterTarget{2000.0f, 50.0f}, 48000);
  for (std::uint32_t i = 0; i < 4800; ++i) a.ch(0)[i] = 0.1f;
  VoiceFilterProcess(vf, a.p(), 1, 4800, snd::FilterTarget{2000.0f, 50.0f}, 48000);
  snd::test::Note("voice filter: after one NaN input sample the output %s", std::isfinite(a.ch(0)[4799]) ? "recovers" : "stays NaN");
  VoiceFilterInit(vf, snd::FilterTarget{2000.0f, 50.0f});
  for (std::uint32_t i = 0; i < 4800; ++i) a.ch(0)[i] = 0.1f;
  VoiceFilterProcess(vf, a.p(), 1, 4800, snd::FilterTarget{2000.0f, 50.0f}, 48000);
  SND_CHECK(!HasNonFinite(a.ch(0), 4800));
}

// ---------------------------------------------------------------------------
// Voice filter
// ---------------------------------------------------------------------------
namespace {
// TPT SVF with prewarped Butterworth Q: exactly the bilinear transform of the
// analogue 2nd-order Butterworth, |H| = 1 / sqrt(1 + (t / tc)^4), t = tan(pi f / fs).
double ButterLp(double f, double fc, double fs) {
  const double r = std::tan(dsptest::kPi * f / fs) / std::tan(dsptest::kPi * fc / fs);
  return 1.0 / std::sqrt(1.0 + r * r * r * r);
}
double ButterHp(double f, double fc, double fs) {
  const double r = std::tan(dsptest::kPi * fc / fs) / std::tan(dsptest::kPi * f / fs);
  return 1.0 / std::sqrt(1.0 + r * r * r * r);
}
double VoiceGain(const snd::FilterTarget& t, double f, std::uint32_t fs) {
  snd::FilterState st;
  VoiceFilterInit(st, t);
  const std::uint32_t settle = std::max<std::uint32_t>(8192, static_cast<std::uint32_t>(fs / 5));
  const std::uint32_t m = std::max<std::uint32_t>(16384, static_cast<std::uint32_t>(8.0 * fs / f));
  const std::uint32_t n = settle + m;
  Audio a(1, n);
  for (std::uint32_t i = 0; i < n; ++i) a.ch(0)[i] = 0.25f * static_cast<float>(std::sin(2 * dsptest::kPi * f * i / fs));
  for (std::uint32_t done = 0; done < n; done += 256) {
    float* p[1] = {a.ch(0) + done};
    SND_RT_SCOPE();
    VoiceFilterProcess(st, p, 1, std::min(256u, n - done), t, fs);
  }
  return SineAmp(a.ch(0), settle, m, f, fs) / 0.25;
}
}  // namespace

SND_TEST(VoiceFilterIndep, MatchesBilinearButterworth) {
  double worst = 0.0;
  for (std::uint32_t fs : {22050u, 48000u, 96000u}) {
    for (double fc : {50.0, 500.0, 3000.0, std::min(0.3 * fs, 19000.0)}) {
      for (double fr : {0.25, 0.5, 1.0, 2.0, 4.0}) {
        const double f = fc * fr;
        if (f >= 0.45 * fs) continue;  // (lowpass targets >= 20 kHz are "off" by definition)
        const double lp = VoiceGain(snd::FilterTarget{static_cast<float>(fc), 0.0f}, f, fs);
        const double hp = VoiceGain(snd::FilterTarget{1.0e9f, static_cast<float>(fc)}, f, fs);
        const double el = std::fabs(dsptest::Db(lp) - dsptest::Db(ButterLp(f, fc, fs)));
        const double eh = std::fabs(dsptest::Db(hp) - dsptest::Db(ButterHp(f, fc, fs)));
        worst = std::max(worst, std::max(el, eh));
        SND_CHECK_MSG(el < 0.05 && eh < 0.05, "fs %u fc %.0f f %.0f: LP %.3f (ref %.3f) HP %.3f (ref %.3f) dB", fs, fc, f,
                      dsptest::Db(lp), dsptest::Db(ButterLp(f, fc, fs)), dsptest::Db(hp), dsptest::Db(ButterHp(f, fc, fs)));
      }
    }
  }
  snd::test::Note("voice filter vs bilinear Butterworth: worst %.4f dB", worst);
}

// Open / off thresholds: bit-exact pass-through, for every channel count and
// rate; IsOpen follows the documented thresholds.
SND_TEST(VoiceFilterIndep, OpenIsBitExactPassThrough) {
  const snd::FilterTarget opens[] = {kVoiceFilterOpen, snd::FilterTarget{kVoiceLowpassOffHz, kVoiceHighpassOffHz},
                                     snd::FilterTarget{30000.0f, 0.0f}, snd::FilterTarget{1e9f, -5.0f}};
  for (std::uint32_t fs : {22050u, 48000u, 192000u}) {
    for (const snd::FilterTarget& t : opens) {
      for (std::uint32_t ch = 1; ch <= 8; ++ch) {
        snd::FilterState st;
        VoiceFilterInit(st, t);
        SND_CHECK(VoiceFilterIsOpen(st, fs));
        Audio a(ch, 300), ref(ch, 300);
        dsptest::Noise nz(ch);
        for (std::uint32_t c = 0; c < ch; ++c)
          for (std::uint32_t i = 0; i < 300; ++i) a.ch(c)[i] = ref.ch(c)[i] = nz.Next();
        VoiceFilterProcess(st, a.p(), ch, 300, t, fs);
        for (std::uint32_t c = 0; c < ch; ++c) SND_CHECK_EQ(dsptest::MaxAbsDiff(a.ch(c), ref.ch(c), 300), 0.0f);
      }
    }
  }
  // At 22.05 kHz a 15 kHz lowpass is above 0.45 fs: off.
  snd::FilterState st;
  VoiceFilterInit(st, snd::FilterTarget{15000.0f, 0.0f});
  SND_CHECK(VoiceFilterIsOpen(st, 22050));
  SND_CHECK(!VoiceFilterIsOpen(st, 48000));
  VoiceFilterInit(st, snd::FilterTarget{1e9f, 10.5f});
  SND_CHECK(!VoiceFilterIsOpen(st, 48000));
}

// Switching a filter on / off: "switching on starts from the steady state of
// the current input (no click on DC/low frequencies); switching off ramps to
// the open limit first". Signal: 0.3 DC + 0.4 sine at 150 Hz, whose own
// |second difference| is <= 0.4 (2 pi 150 / 48k)^2 = 1.5e-3.
namespace {
float SwitchClick(const snd::FilterTarget* seq, std::uint32_t n, std::size_t* at) {
  const std::uint32_t fs = 48000;
  snd::FilterState st;
  VoiceFilterInit(st, seq[0]);
  std::vector<float> out;
  Audio a(1, 128);
  std::uint64_t t = 0;
  for (std::uint32_t k = 0; k < n; ++k) {
    for (std::uint32_t i = 0; i < 128; ++i, ++t)
      a.ch(0)[i] = 0.3f + 0.4f * static_cast<float>(std::sin(2 * dsptest::kPi * 150.0 * static_cast<double>(t) / fs));
    VoiceFilterProcess(st, a.p(), 1, 128, seq[k], fs);
    out.insert(out.end(), a.ch(0), a.ch(0) + 128);
  }
  float worst = 0.0f;
  for (std::size_t i = 2; i < out.size(); ++i) {
    const float d2 = std::fabs(out[i] - 2 * out[i - 1] + out[i - 2]);
    if (d2 > worst) {
      worst = d2;
      *at = i;
    }
  }
  return worst;
}
}  // namespace

SND_TEST(VoiceFilterIndep, LowpassSwitchOnOffWithoutClick) {
  // On (open -> 1000 Hz), up to 18 kHz, then off: no click.
  const snd::FilterTarget seq[] = {kVoiceFilterOpen,  kVoiceFilterOpen,  {1000.0f, 0.0f},  {1000.0f, 0.0f},
                                   {1300.0f, 0.0f},   {1800.0f, 0.0f},   {2500.0f, 0.0f},  {3500.0f, 0.0f},
                                   {5000.0f, 0.0f},   {7000.0f, 0.0f},   {10000.0f, 0.0f}, {14000.0f, 0.0f},
                                   {18000.0f, 0.0f},  {18000.0f, 0.0f},  kVoiceFilterOpen, kVoiceFilterOpen};
  std::size_t at = 0;
  const float w = SwitchClick(seq, 16, &at);
  snd::test::Note("lowpass on/off: max second difference %.5f at frame %zu", static_cast<double>(w), at);
  SND_CHECK_MSG(w < 0.01f, "click %.5f at frame %zu (block %zu)", static_cast<double>(w), at, at / 128);
  // Off straight from 300 Hz: the cutoff sweeps 300 Hz -> 0.45 fs inside one
  // 128-frame block (as requested by the caller); measured only.
  const snd::FilterTarget seq2[] = {{300.0f, 0.0f}, {300.0f, 0.0f}, {300.0f, 0.0f}, kVoiceFilterOpen, kVoiceFilterOpen};
  const float w2 = SwitchClick(seq2, 5, &at);
  snd::test::Note("lowpass 300 Hz -> off in one block: max second difference %.5f at frame %zu", static_cast<double>(w2), at);
}

SND_TEST(VoiceFilterIndep, HighpassSwitchOnWithoutClick) {
  const snd::FilterTarget seq[] = {kVoiceFilterOpen, kVoiceFilterOpen, {1e9f, 200.0f}, {1e9f, 200.0f}, {1e9f, 200.0f}};
  std::size_t at = 0;
  const float w = SwitchClick(seq, 5, &at);
  snd::test::Note("highpass on: max second difference %.5f at frame %zu", static_cast<double>(w), at);
  SND_CHECK_MSG(w < 0.01f, "click %.5f at frame %zu (block %zu)", static_cast<double>(w), at, at / 128);
}

// Highpass switching OFF: the ramp ends at the 10 Hz "open limit", which still
// removes DC, then the next block bypasses the filter: the DC the highpass had
// removed comes back as a step (the size of the DC offset).
SND_TEST(VoiceFilterIndep, HighpassSwitchOffStepsBackTheDc) {
  const snd::FilterTarget seq[] = {{1e9f, 200.0f}, {1e9f, 200.0f}, {1e9f, 200.0f}, {1e9f, 200.0f}, {1e9f, 200.0f},
                                   kVoiceFilterOpen, kVoiceFilterOpen, kVoiceFilterOpen};
  std::size_t at = 0;
  const float w = SwitchClick(seq, 8, &at);
  snd::test::Note("highpass off: max second difference %.5f at frame %zu (block %zu)", static_cast<double>(w), at, at / 128);
  SND_CHECK_MSG(w < 0.01f, "click %.5f at frame %zu (block %zu)", static_cast<double>(w), at, at / 128);
}
