// Independent resampler tests (resampler.hpp). Expected values are analytic:
// a sine of f Hz at the source rate S = ratio * 48000 must come out as the
// same sine at the output rate, with no delay (the first output frame is the
// first input frame), unit DC gain, and a passband / stopband as documented.
// Every call goes through a guarded driver: the input buffer has NaN guards
// after the `n` frames InputFrames asked for (reading them would poison the
// output) and finite guards before the pad; the state has guards after
// ResamplerStateBytes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/kernels.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr double kOut = 48000.0;
constexpr float kStateGuard = -7777.0f;
constexpr float kPreGuard = 4242.0f;
constexpr std::uint32_t kPost = 64;

const ResamplerQuality kQualities[] = {ResamplerQuality::Linear, ResamplerQuality::Cubic, ResamplerQuality::Sinc};
const char* QName(ResamplerQuality q) {
  return q == ResamplerQuality::Linear ? "linear" : (q == ResamplerQuality::Cubic ? "cubic" : "sinc");
}

using Signal = std::function<double(std::uint32_t ch, std::uint64_t frame)>;

struct Guarded {
  ResamplerQuality q;
  std::uint32_t channels;
  std::vector<float> state;  // floats: state bytes + 16 guard floats
  std::size_t stateFloats;
  std::uint64_t consumed = 0;
  std::uint32_t violations = 0;
  float maxRatio;
  Guarded(ResamplerQuality quality, std::uint32_t ch, float maxR = kResamplerMaxRatio)
      : q(quality), channels(ch), maxRatio(maxR) {
    stateFloats = (ResamplerStateBytes(q, ch, maxR) + 3) / 4;
    state.assign(stateFloats + 16 + 4, kStateGuard);
    ResamplerInit(st(), q, ch, maxR);
  }
  // 16-byte aligned start inside the vector (the documented alignment).
  float* st() {
    auto p = reinterpret_cast<std::uintptr_t>(state.data());
    return state.data() + ((16 - (p & 15)) & 15) / 4;
  }
  bool StateGuardsOk() {
    float* s = st();
    for (std::size_t i = stateFloats; i < stateFloats + 16; ++i)
      if (s[i] != kStateGuard) return false;
    return true;
  }
  // Produces `frames` output frames into out[c] (appended).
  std::uint32_t Run(const Signal& sig, std::vector<std::vector<float>>& out, std::uint32_t frames, float r0, float r1) {
    const std::uint32_t pad = ResamplerPadFrames(q, maxRatio);
    const std::uint32_t need = ResamplerInputFrames(st(), frames, r0, r1);
    const std::uint32_t stride = 16 + pad + need + kPost;
    dsptest::AlignedMem mem(sizeof(float) * stride * channels);
    float* inBase[8];
    for (std::uint32_t c = 0; c < channels; ++c) {
      float* b = static_cast<float*>(mem.get()) + c * stride;
      for (std::uint32_t i = 0; i < 16; ++i) b[i] = kPreGuard;
      for (std::uint32_t i = 0; i < pad; ++i) b[16 + i] = std::numeric_limits<float>::quiet_NaN();  // scratch
      for (std::uint32_t i = 0; i < need; ++i) b[16 + pad + i] = static_cast<float>(sig(c, consumed + i));
      for (std::uint32_t i = 0; i < kPost; ++i) b[16 + pad + need + i] = std::numeric_limits<float>::quiet_NaN();
      inBase[c] = b + 16;
    }
    std::vector<std::vector<float>> o(channels, std::vector<float>(frames + 2, kPreGuard));
    float* op[8];
    for (std::uint32_t c = 0; c < channels; ++c) op[c] = o[c].data() + 1;
    std::uint32_t used;
    {
      SND_RT_SCOPE();
      used = ResamplerProcess(st(), inBase, need, op, frames, r0, r1);
    }
    if (used != need) ++violations;
    for (std::uint32_t c = 0; c < channels; ++c) {
      float* b = static_cast<float*>(mem.get()) + c * stride;
      for (std::uint32_t i = 0; i < 16; ++i) violations += b[i] != kPreGuard;
      for (std::uint32_t i = 0; i < need; ++i)  // new input is not modified
        violations += b[16 + pad + i] != static_cast<float>(sig(c, consumed + i));
      violations += o[c][0] != kPreGuard || o[c][frames + 1] != kPreGuard;
      violations += HasNonFinite(op[c], frames) ? 1u : 0u;
      out[c].insert(out[c].end(), op[c], op[c] + frames);
    }
    violations += StateGuardsOk() ? 0u : 1u;
    consumed += need;
    return need;
  }
};

Signal SineSig(double f, double rate, double amp = 0.5, double phase = 0.0) {
  return [=](std::uint32_t, std::uint64_t n) { return amp * std::sin(2.0 * dsptest::kPi * f * static_cast<double>(n) / rate + phase); };
}

// Least-squares fit of a*sin + b*cos + dc at frequency f (cycles / output frame);
// returns amplitude, phase (rad, of sin) and the residual RMS.
struct Fit {
  double amp, phase, residual;
};
Fit FitSine(const float* y, std::uint32_t n, double fcyc, std::uint64_t k0) {
  double sss = 0, scc = 0, ssc = 0, sys = 0, syc = 0, s1 = 0, ss = 0, sc = 0, sy = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const double w = 2.0 * dsptest::kPi * fcyc * static_cast<double>(k0 + i);
    const double s = std::sin(w), c = std::cos(w);
    sss += s * s; scc += c * c; ssc += s * c; sys += y[i] * s; syc += y[i] * c;
    s1 += 1; ss += s; sc += c; sy += y[i];
  }
  // Solve the 3x3 normal equations [sss ssc ss; ssc scc sc; ss sc s1] [a b d] = [sys syc sy].
  double m[3][4] = {{sss, ssc, ss, sys}, {ssc, scc, sc, syc}, {ss, sc, s1, sy}};
  for (int i = 0; i < 3; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      const double f = m[j][i] / m[i][i];
      for (int k = i; k < 4; ++k) m[j][k] -= f * m[i][k];
    }
  }
  double x[3];
  for (int i = 2; i >= 0; --i) {
    double s = m[i][3];
    for (int k = i + 1; k < 3; ++k) s -= m[i][k] * x[k];
    x[i] = s / m[i][i];
  }
  double r2 = 0.0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const double w = 2.0 * dsptest::kPi * fcyc * static_cast<double>(k0 + i);
    const double e = y[i] - (x[0] * std::sin(w) + x[1] * std::cos(w) + x[2]);
    r2 += e * e;
  }
  return {std::hypot(x[0], x[1]), std::atan2(x[1], x[0]), std::sqrt(r2 / n)};
}

// Runs a sine of fIn (Hz at the source rate ratio*48k) through q at a constant ratio.
std::vector<float> RunSine(ResamplerQuality q, double ratio, double fIn, std::uint32_t outFrames,
                           std::uint32_t block = 480, std::uint32_t* violations = nullptr) {
  Guarded g(q, 1);
  std::vector<std::vector<float>> out(1);
  const auto sig = SineSig(fIn, ratio * kOut);
  for (std::uint32_t done = 0; done < outFrames; done += block)
    g.Run(sig, out, std::min(block, outFrames - done), static_cast<float>(ratio), static_cast<float>(ratio));
  if (violations != nullptr) *violations = g.violations;
  return out[0];
}

}  // namespace

// The output is the same sine with the right frequency, amplitude AND phase
// (no delay: output frame k is source position k * ratio), ratios 0.25..4.
SND_TEST(ResamplerIndep, SineFrequencyAmplitudePhase) {
  const double ratios[] = {0.25, 0.5, 44100.0 / 48000.0, 1.0, 48000.0 / 44100.0, 1.5, 2.0, 3.0, 4.0};
  for (ResamplerQuality q : kQualities) {
    for (double r : ratios) {
      const double S = r * kOut;
      // A tone well inside every quality's useful band: 0.02 of the lower rate
      // (linear interpolation error ~ (pi f/S)^2 / 8), and inside the sinc
      // passband documented for ratios above 2.
      double f = 0.02 * std::min(S, kOut);
      if (q == ResamplerQuality::Sinc) f = std::min(0.2 * std::min(S, kOut), 0.8 * (0.45 / std::max(1.0, r) - 0.085) * S);
      std::uint32_t viol = 0;
      const auto y = RunSine(q, r, f, 9600, 480, &viol);
      SND_CHECK_EQ(viol, 0u);
      const std::uint32_t skip = 64;  // sinc start-up: history before the first frame is silence
      const Fit fit = FitSine(y.data() + skip, 8192, f / kOut, skip);
      const double ampDb = dsptest::Db(fit.amp / 0.5);
      const double tolDb = q == ResamplerQuality::Sinc ? 0.1 : 0.02;
      SND_CHECK_MSG(std::fabs(ampDb) < tolDb, "%s ratio %.4f f %.1f Hz: amplitude %.4f dB", QName(q), r, f, ampDb);
      // Phase of the sine term (0 = no delay). 1e-3 rad at 0.02 * rate ~ 0.008 frames.
      SND_CHECK_MSG(std::fabs(fit.phase) < 2e-3, "%s ratio %.4f f %.1f Hz: phase %.5f rad (delay %.3f frames)", QName(q),
                    r, f, fit.phase, -fit.phase / (2 * dsptest::kPi * f / kOut));
      if (q == ResamplerQuality::Sinc) {
        snd::test::Note("sinc ratio %.4f f %7.1f Hz: %+.4f dB, residual %.1f dB", r, f, ampDb,
                        dsptest::Db(fit.residual / (0.5 / std::sqrt(2.0))));
      }
    }
  }
}

// The first output frame after Init / Reset equals the first input frame for
// linear and cubic at any ratio (position 0), and the sinc's output at
// position 0 of a DC step is the windowed-sinc half sum (no extra delay).
SND_TEST(ResamplerIndep, FirstOutputFrameIsFirstInputFrame) {
  for (ResamplerQuality q : {ResamplerQuality::Linear, ResamplerQuality::Cubic}) {
    for (float r : {0.3f, 1.0f, 2.7f}) {
      Guarded g(q, 1);
      std::vector<std::vector<float>> out(1);
      g.Run([](std::uint32_t, std::uint64_t n) { return 0.25 + 0.01 * static_cast<double>(n); }, out, 5, r, r);
      SND_CHECK_EQ(out[0][0], 0.25f);
      ResamplerReset(g.st());
      g.consumed = 0;
      out[0].clear();
      g.Run([](std::uint32_t, std::uint64_t n) { return n == 0 ? -0.5 : 0.0; }, out, 1, r, r);
      SND_CHECK_EQ(out[0][0], -0.5f);
    }
  }
}

// DC in -> DC out (unity gain) at any ratio and phase, including ramps.
SND_TEST(ResamplerIndep, DcGainIsUnity) {
  for (ResamplerQuality q : kQualities) {
    for (float r0 : {0.1f, 0.7f, 1.0f, 1.37f, 2.5f, 7.9f, 20.0f}) {
      Guarded g(q, 2);
      std::vector<std::vector<float>> out(2);
      const auto dc = [](std::uint32_t c, std::uint64_t) { return c == 0 ? 0.75 : -1.0; };
      for (int b = 0; b < 40; ++b) g.Run(dc, out, 97, r0, b % 2 ? r0 : r0 * 1.3f);
      // Skip the start-up (history before the first frame is silence): 17 input frames.
      const std::size_t skip = static_cast<std::size_t>(20.0f / r0) + 40;
      float worst = 0.0f;
      for (std::size_t i = skip; i < out[0].size(); ++i) {
        worst = std::max(worst, std::fabs(out[0][i] - 0.75f));
        worst = std::max(worst, std::fabs(out[1][i] + 1.0f));
      }
      SND_CHECK_MSG(worst < 2e-6f, "%s ratio %.2f: DC error %g", QName(q), static_cast<double>(r0), static_cast<double>(worst));
      SND_CHECK_EQ(g.violations, 0u);
    }
  }
}

// Sum of the response to a unit impulse = 1 / ratio (each output sample sees
// the kernel at spacing `ratio` in input frames). Linear / cubic have no
// anti-aliasing filter, so only ratios <= 1 apply to them.
SND_TEST(ResamplerIndep, ImpulseResponseSum) {
  for (ResamplerQuality q : kQualities) {
    for (float r : {0.25f, 0.5f, 1.0f, 2.0f}) {
      if (r > 1.0f && q != ResamplerQuality::Sinc) continue;
      Guarded g(q, 1);
      std::vector<std::vector<float>> out(1);
      const auto imp = [](std::uint32_t, std::uint64_t n) { return n == 40 ? 1.0 : 0.0; };
      for (int b = 0; b < 8; ++b) g.Run(imp, out, 64, r, r);
      double sum = 0.0;
      for (float v : out[0]) sum += v;
      SND_CHECK_MSG(std::fabs(sum - 1.0 / r) < 2e-3 / r, "%s ratio %.2f: impulse sum %.6f, expected %.6f", QName(q),
                    static_cast<double>(r), sum, 1.0 / r);
    }
  }
}

// Measured sinc passband edge (+-0.1 dB) and the alias / image rejection,
// compared with the figures in resampler.hpp.
SND_TEST(ResamplerIndep, SincPassbandEdgeAndStopbandMeasured) {
  const double ratios[] = {0.5, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
  for (double r : ratios) {
    const double S = r * kOut, lower = std::min(S, kOut);
    // Passband: walk up in 0.5 % steps of the lower rate until |gain| > 0.1 dB.
    double edge = 0.0;
    for (double x = 0.05; x < 0.5; x += 0.005) {
      const double f = x * lower;
      const auto y = RunSine(ResamplerQuality::Sinc, r, f, 6000, 1000);
      const Fit fit = FitSine(y.data() + 64, 4096, f / kOut, 64);
      if (std::fabs(dsptest::Db(fit.amp / 0.5)) > 0.1) break;
      edge = x;
    }
    const double documented = r <= 2.0 ? 0.28 : (0.45 / r - 0.085) * S / lower;
    // Alias / image rejection: worst level of everything that is not the tone,
    // for source tones whose alias or image falls inside the documented passband.
    double worstAliasDb = -300.0, worstAt = 0.0;
    if (r > 1.0) {
      // Tones between 48k - passband and the source Nyquist alias into [0, passband].
      const double pb = documented * lower;
      for (double f = kOut - pb; f < 0.5 * S; f += 0.0123 * S) {
        if (f <= 0.5 * kOut) continue;
        const auto y = RunSine(ResamplerQuality::Sinc, r, f, 6000, 1000);
        const double lvl = dsptest::Db(dsptest::Rms(y.data() + 1024, 4096) / (0.5 / std::sqrt(2.0)));
        if (lvl > worstAliasDb) {
          worstAliasDb = lvl;
          worstAt = f;
        }
      }
    } else {
      // Upsampling: passband tones; residual after removing the tone = images + error.
      for (double x = 0.02; x <= documented; x += 0.02) {
        const double f = x * S;
        const auto y = RunSine(ResamplerQuality::Sinc, r, f, 6000, 1000);
        const Fit fit = FitSine(y.data() + 64, 4096, f / kOut, 64);
        const double lvl = dsptest::Db(fit.residual / (0.5 / std::sqrt(2.0)));
        if (lvl > worstAliasDb) {
          worstAliasDb = lvl;
          worstAt = f;
        }
      }
    }
    snd::test::Note("sinc ratio %.2f: passband edge (+-0.1 dB) %.3f x min rate (documented %.3f); worst alias/image %.1f dB (tone %.0f Hz)",
                    r, edge, documented, worstAliasDb, worstAt);
    SND_CHECK_MSG(edge + 0.006 >= documented * 0.97, "ratio %.2f passband edge %.3f < documented %.3f", r, edge, documented);
    SND_CHECK_MSG(worstAliasDb < -78.0, "ratio %.2f: alias/image into the passband %.1f dB (documented stopband >= ~80 dB)", r,
                  worstAliasDb);
  }
}

// Constant ratio: bit-identical output and identical consumption for any
// split, including one frame per call, for every quality and odd ratios.
SND_TEST(ResamplerIndep, BlockSplitInvarianceRandomAndSingleFrames) {
  const auto sig = [](std::uint32_t c, std::uint64_t n) {
    return std::sin(0.021 * static_cast<double>(n) * (1.0 + c)) * 0.6 + 0.1 * std::sin(1.9 * static_cast<double>(n));
  };
  for (ResamplerQuality q : kQualities) {
    for (float r : {1.0f / 3.0f, 0.999f, 1.0f, 44100.0f / 48000.0f, 2.3456f, 31.0f}) {
      Guarded a(q, 3), b(q, 3), c(q, 3);
      std::vector<std::vector<float>> oa(3), ob(3), oc(3);
      a.Run(sig, oa, 1500, r, r);
      std::uint32_t seed = 77, done = 0;
      while (done < 1500) {
        seed = seed * 1103515245u + 12345u;
        const std::uint32_t n = std::min<std::uint32_t>(1u + (seed >> 16) % 200u, 1500 - done);
        b.Run(sig, ob, n, r, r);
        done += n;
      }
      for (std::uint32_t i = 0; i < 1500; ++i) c.Run(sig, oc, 1, r, r);
      std::uint32_t diff = 0;
      for (std::uint32_t ch = 0; ch < 3; ++ch)
        for (std::uint32_t i = 0; i < 1500; ++i) diff += (oa[ch][i] != ob[ch][i]) + (oa[ch][i] != oc[ch][i]);
      SND_CHECK_MSG(diff == 0, "%s ratio %.4f: %u samples differ", QName(q), static_cast<double>(r), diff);
      SND_CHECK_EQ(a.consumed, b.consumed);
      SND_CHECK_EQ(a.consumed, c.consumed);
      SND_CHECK(ResamplerPendingOffset(a.st()) == ResamplerPendingOffset(c.st()));
      SND_CHECK_EQ(a.violations + b.violations + c.violations, 0u);
    }
  }
}

// Ratio ramps: chunked ramps (each chunk from the previous end value) against
// one long ramp, and the click check (second difference) through ramps that
// cross the sinc table boundaries.
SND_TEST(ResamplerIndep, RampSplitAndNoClicks) {
  const auto sig = SineSig(1000.0, 48000.0, 0.5);
  for (ResamplerQuality q : kQualities) {
    // Many small blocks ramping 0.5 -> 2.0 -> 0.9 -> 1.05 -> 1.0 (crosses tables).
    const float pts[] = {0.5f, 2.0f, 0.9f, 1.05f, 1.0f, 1.12f, 0.97f};
    Guarded g(q, 1);
    std::vector<std::vector<float>> out(1);
    for (std::uint32_t seg = 0; seg + 1 < sizeof(pts) / sizeof(pts[0]); ++seg) {
      for (std::uint32_t b = 0; b < 20; ++b) {
        const float r0 = pts[seg] + (pts[seg + 1] - pts[seg]) * static_cast<float>(b) / 20.0f;
        const float r1 = pts[seg] + (pts[seg + 1] - pts[seg]) * static_cast<float>(b + 1) / 20.0f;
        g.Run(sig, out, 128, r0, r1);
      }
    }
    // A 1 kHz sine at ratio <= 2 has |second difference| <= A (2 pi f r / fs)^2 ~ 0.034.
    float worst = 0.0f;
    std::size_t at = 0;
    for (std::size_t i = 40; i < out[0].size(); ++i) {
      const float d2 = std::fabs(out[0][i] - 2.0f * out[0][i - 1] + out[0][i - 2]);
      if (d2 > worst) {
        worst = d2;
        at = i;
      }
    }
    snd::test::Note("%s: max second difference through ramps %.4f at %zu", QName(q), static_cast<double>(worst), at);
    SND_CHECK_MSG(worst < 0.05f, "%s: click (second difference %.4f) at output frame %zu", QName(q),
                  static_cast<double>(worst), at);
    SND_CHECK_EQ(g.violations, 0u);
    // One ramp vs. the same ramp in 8 chunks (no tolerance documented for ramps).
    Guarded one(q, 1), many(q, 1);
    std::vector<std::vector<float>> o1(1), o2(1);
    one.Run(sig, o1, 1024, 0.8f, 1.15f);
    for (std::uint32_t k = 0; k < 8; ++k) {
      const float r0 = 0.8f + 0.35f * static_cast<float>(k) / 8.0f, r1 = 0.8f + 0.35f * static_cast<float>(k + 1) / 8.0f;
      many.Run(sig, o2, 128, r0, r1);
    }
    const float d = dsptest::MaxAbsDiff(o1[0].data() + 32, o2[0].data() + 32, 1024 - 32);
    const long long dc = static_cast<long long>(one.consumed) - static_cast<long long>(many.consumed);
    snd::test::Note("%s: one ramp vs 8 chunks: max diff %.3g, consumed %+lld frames", QName(q), static_cast<double>(d), dc);
    SND_CHECK_MSG(d < 2e-3f, "%s: ramp split difference %g", QName(q), static_cast<double>(d));
    SND_CHECK(dc >= -1 && dc <= 1);
  }
}

// Out-of-range ratios are clamped to [1/256, 32]; NaN / Inf / 0 / negative do
// not make the resampler read outside the frames it asked for (NaN guards) or
// write outside its state / the pad.
SND_TEST(ResamplerIndep, ExtremeRatiosStayInBounds) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float vals[] = {0.0f, -1.0f, -0.0f, nan, INFINITY, -INFINITY, 1e-9f, 1.0f / 256.0f, 32.0f, 1e9f, 33.0f};
  const auto sig = SineSig(300.0, 48000.0);
  for (ResamplerQuality q : kQualities) {
    for (float r0 : vals) {
      for (float r1 : {r0, 1.0f, 40.0f}) {
        Guarded g(q, 2);
        std::vector<std::vector<float>> out(2);
        for (std::uint32_t n : {1u, 7u, 256u, 33u}) g.Run(sig, out, n, r0, r1);
        SND_CHECK_MSG(g.violations == 0, "%s ratio %g -> %g: %u guard/consumption violations", QName(q),
                      static_cast<double>(r0), static_cast<double>(r1), g.violations);
        // Clamped consumption: at most 32 frames per output frame plus the look-ahead.
        SND_CHECK(g.consumed <= 32u * 297u + ResamplerHistoryFrames(q) + 2u);
      }
    }
  }
  // Equivalence of clamped values.
  for (ResamplerQuality q : kQualities) {
    dsptest::AlignedMem s(ResamplerStateBytes(q, 1));
    ResamplerInit(s.get(), q, 1);
    SND_CHECK_EQ(ResamplerInputFrames(s.get(), 100, 0.0f, 0.0f), ResamplerInputFrames(s.get(), 100, 1.0f / 256.0f, 1.0f / 256.0f));
    SND_CHECK_EQ(ResamplerInputFrames(s.get(), 100, nan, nan), ResamplerInputFrames(s.get(), 100, 1.0f / 256.0f, 1.0f / 256.0f));
    SND_CHECK_EQ(ResamplerInputFrames(s.get(), 100, 1e9f, 1e9f), ResamplerInputFrames(s.get(), 100, 32.0f, 32.0f));
    SND_CHECK_EQ(ResamplerInputFrames(s.get(), 0, 1.0f, 1.0f), 0u);
  }
}

// 1..8 channels: every channel is processed identically.
SND_TEST(ResamplerIndep, MultiChannelIdenticalPerChannel) {
  for (ResamplerQuality q : kQualities) {
    for (std::uint32_t ch = 1; ch <= 8; ++ch) {
      Guarded g(q, ch), mono(q, 1);
      SND_CHECK_EQ(ResamplerGetChannels(g.st()), ch);
      SND_CHECK(ResamplerGetQuality(g.st()) == q);
      std::vector<std::vector<float>> out(ch), ref(1);
      const auto sig = [](std::uint32_t, std::uint64_t n) { return std::sin(0.05 * static_cast<double>(n)); };
      for (int b = 0; b < 5; ++b) {
        g.Run(sig, out, 100, 1.3f, 0.7f + 0.2f * b);
        mono.Run(sig, ref, 100, 1.3f, 0.7f + 0.2f * b);
      }
      std::uint32_t diff = 0;
      for (std::uint32_t c = 0; c < ch; ++c)
        for (std::size_t i = 0; i < ref[0].size(); ++i) diff += out[c][i] != ref[0][i];
      SND_CHECK_MSG(diff == 0, "%s %u ch: %u samples differ from mono", QName(q), ch, diff);
      SND_CHECK_EQ(g.violations, 0u);
    }
  }
}

// InputFrames is pure and equals what Process consumes; short input gives
// silence, returns 0 and leaves the state unchanged; extra input is ignored.
SND_TEST(ResamplerIndep, InputFramesContract) {
  std::uint32_t seed = 5;
  for (ResamplerQuality q : kQualities) {
    Guarded g(q, 1);
    std::vector<std::vector<float>> out(1);
    const auto sig = SineSig(700.0, 48000.0);
    for (int it = 0; it < 300; ++it) {
      seed = seed * 1664525u + 1013904223u;
      const float r0 = 0.05f + static_cast<float>(seed >> 8) / 16777216.0f * 6.0f;
      seed = seed * 1664525u + 1013904223u;
      const float r1 = 0.05f + static_cast<float>(seed >> 8) / 16777216.0f * 6.0f;
      const std::uint32_t n = 1 + (seed >> 3) % 300;
      const std::uint32_t n1 = ResamplerInputFrames(g.st(), n, r0, r1);
      const std::uint32_t n2 = ResamplerInputFrames(g.st(), n, r0, r1);
      SND_CHECK_EQ(n1, n2);
      if (it % 50 == 7) {
        // Too little input.
        std::vector<float> before(g.st(), g.st() + g.stateFloats);
        std::vector<float> in(ResamplerPadFrames(q) + n1 + 1, 0.3f), o(n, 9.0f);
        float* ip[1] = {in.data()};
        float* opp[1] = {o.data()};
        if (n1 > 0) {
          SND_CHECK_EQ(ResamplerProcess(g.st(), ip, n1 - 1, opp, n, r0, r1), 0u);
          for (float v : o) SND_CHECK_EQ(v, 0.0f);
          SND_CHECK(std::memcmp(before.data(), g.st(), g.stateFloats * sizeof(float)) == 0);
        }
      }
      SND_CHECK_EQ(g.Run(sig, out, n, r0, r1), n1);
    }
    SND_CHECK_EQ(g.violations, 0u);
  }
}

// Long run: consumed + PendingOffset follows sum(ratio) in 32.32 fixed point
// with no drift over millions of frames; the offset stays bounded.
SND_TEST(ResamplerIndep, LongRunPositionAccumulation) {
  const double r = 44100.0 / 48000.0 * 1.0001;
  const std::uint64_t fixed = static_cast<std::uint64_t>(r * 4294967296.0 + 0.5);
  for (ResamplerQuality q : kQualities) {
    dsptest::AlignedMem st(ResamplerStateBytes(q, 1));
    ResamplerInit(st.get(), q, 1);
    const std::uint32_t pad = ResamplerPadFrames(q);
    std::vector<float> in(pad + 2048, 0.0f), out(512);
    float* ip[1] = {in.data()};
    float* op[1] = {out.data()};
    std::uint64_t consumed = 0, produced = 0;
    double minOff = 1e9, maxOff = -1e9;
    for (int b = 0; b < 10000; ++b) {  // 5.12 M output frames
      const std::uint32_t n = ResamplerInputFrames(st.get(), 512, static_cast<float>(r), static_cast<float>(r));
      consumed += ResamplerProcess(st.get(), ip, n, op, 512, static_cast<float>(r), static_cast<float>(r));
      produced += 512;
      const double off = ResamplerPendingOffset(st.get());
      minOff = std::min(minOff, off);
      maxOff = std::max(maxOff, off);
    }
    // Ratio as passed (float) in 32.32: the expected position of the next output frame.
    const std::uint64_t ff = static_cast<std::uint64_t>(static_cast<double>(static_cast<float>(r)) * 4294967296.0 + 0.5);
    const double expectPos = static_cast<double>(produced) * static_cast<double>(ff) / 4294967296.0;
    const double pos = static_cast<double>(consumed) + ResamplerPendingOffset(st.get());
    snd::test::Note("%s: after %llu frames position %.6f expected %.6f (float ratio), %.6f (double ratio); offset range [%.3f, %.3f]",
                    QName(q), static_cast<unsigned long long>(produced), pos, expectPos,
                    static_cast<double>(produced) * static_cast<double>(fixed) / 4294967296.0, minOff, maxOff);
    SND_CHECK_MSG(std::fabs(pos - expectPos) < 1e-6, "%s: position drift %.9f frames", QName(q), pos - expectPos);
    SND_CHECK(maxOff - minOff <= 2.0);
  }
}

// resampler.hpp (decision of the dsp fix round): after a call whose last output
// frame used ratio r and look-ahead R = ResamplerLookaheadFrames(q, ...),
// PendingOffset is in [r - R - 1, r - R) (unless the call consumed no input).
// Ramps from 0.3 to 4.8 cross the sinc kernel switch at ratio 4; R is taken at the
// call's largest ratio (the ratio the call chooses its kernel from).
SND_TEST(ResamplerIndep, PendingOffsetRangeAsDocumented) {
  for (ResamplerQuality q : kQualities) {
    Guarded g(q, 1);
    std::vector<std::vector<float>> out(1);
    std::uint32_t bad = 0, checked = 0;
    double worstBelow = 0.0, worstAbove = 0.0;
    for (int b = 0; b < 120; ++b) {
      const float r0 = 0.3f + 0.075f * static_cast<float>(b % 60 + (b >= 60 ? 60 - 2 * (b % 60) : 0));
      const float r1 = (b % 7 == 3) ? r0 : r0 + 0.075f;
      const std::uint32_t n = 1 + (static_cast<std::uint32_t>(b) * 37u) % 200u;
      const std::uint32_t used = g.Run(SineSig(100.0, 48000.0), out, n, r0, r1);
      if (used == 0) continue;
      const double rLast = r0 + (static_cast<double>(r1) - r0) * (n - 1) / n;
      const double R = ResamplerLookaheadFrames(q, std::max(r0, r1));
      const double off = ResamplerPendingOffset(g.st());
      const double lo = rLast - R - 1.0, hi = rLast - R;
      ++checked;
      if (off < lo - 1e-6 || off >= hi + 1e-6) {
        worstBelow = std::min(worstBelow, off - lo);
        worstAbove = std::max(worstAbove, off - hi);
        if (bad++ < 5)
          SND_CHECK_MSG(false, "%s ratio %.3f -> %.3f (%u frames): offset %.4f outside [%.4f, %.4f) (R = %.0f)", QName(q),
                        static_cast<double>(r0), static_cast<double>(r1), n, off, lo, hi, R);
      }
    }
    snd::test::Note("%s: %u calls checked, %u outside the documented range", QName(q), checked, bad);
    SND_CHECK_EQ(bad, 0u);
    SND_CHECK_EQ(g.violations, 0u);
  }
}

// resampler.hpp: "The cutoff follows the ratio (anti-aliasing when the ratio is
// > 1) ... Stopband >= ~80 dB", ratios clamped to [1/256, 32]. The 32-tap
// kernel has a transition band of ~0.165 x the SOURCE rate, so from ratio ~5 on
// (where the documented passband formula (0.45 / ratio - 0.085) is <= 0) the
// stopband starts above the output rate; and the tables stop at ratio 8, so
// from ~9 on the cutoff itself is above the output Nyquist. Source tones in
// [0.75, 0.98] x 48 kHz alias into [1, 12] kHz with little attenuation.
SND_TEST(ResamplerIndep, SincStopbandFailsAboveRatio5) {
  for (double r : {6.0, 8.0, 16.0, 32.0}) {
    double worst = -300.0, at = 0.0;
    // Source tones from 0.75 to 0.98 of the output rate alias to 0.02..0.25 of it.
    for (double x = 0.75; x <= 0.98; x += 0.0115) {
      const double f = x * kOut;
      const auto y = RunSine(ResamplerQuality::Sinc, r, f, 6000, 1000);
      const double lvl = dsptest::Db(dsptest::Rms(y.data() + 1024, 4096) / (0.5 / std::sqrt(2.0)));
      if (lvl > worst) {
        worst = lvl;
        at = f;
      }
    }
    snd::test::Note("sinc ratio %.0f: worst alias of a source tone in [0.75, 0.98] x 48 kHz: %.1f dB (tone %.0f Hz)", r,
                    worst, at);
    SND_CHECK_MSG(worst < -78.0, "ratio %.0f: tone %.0f Hz aliases at %.1f dB (documented stopband >= ~80 dB)", r, at, worst);
  }
}

// Fix-round regression checks for the ratio > 4 sinc path (per-call polyphase
// table of a ratio-scaled kernel): guards and state size for every maxRatio,
// and no level jump / click where a ramp crosses ratio 4 (the two kernels have
// different passbands: the table at 4 is flat to ~0.21 x out, the scaled kernel
// to ~0.33 x out).
SND_TEST(ResamplerIndep, SincAbove4GuardsForEveryMaxRatio) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (float maxR : {1.0f, 4.0f, 4.01f, 5.5f, 8.0f, 16.0f, 31.9f, 32.0f}) {
    for (std::uint32_t ch : {1u, 3u, 8u}) {
      Guarded g(ResamplerQuality::Sinc, ch, maxR);
      SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Sinc, maxR),
                   2u * ResamplerLookaheadFrames(ResamplerQuality::Sinc, maxR));
      SND_CHECK_EQ(ResamplerPadFrames(ResamplerQuality::Sinc, maxR) % 16u, 0u);
      std::vector<std::vector<float>> out(ch);
      const auto sig = SineSig(700.0, 48000.0);
      const float rs[][2] = {{0.5f, 3.9f}, {3.9f, 4.2f}, {40.0f, 40.0f}, {maxR, 1.0f}, {nan, 33.0f}, {7.0f, 7.0f}, {4.0f, 4.0f}};
      for (const auto& r : rs)
        for (std::uint32_t n : {1u, 64u, 255u}) g.Run(sig, out, n, r[0], r[1]);
      SND_CHECK_MSG(g.violations == 0, "maxRatio %.2f, %u ch: %u guard / consumption violations", static_cast<double>(maxR), ch,
                    g.violations);
    }
  }
}

SND_TEST(ResamplerIndep, SincNoJumpAcrossRatio4) {
  // Level of a tone just below and just above ratio 4 (same source frequency).
  // Inside both passbands (<= 0.21 x out = 10 kHz) the level must not jump;
  // above, the documented passbands differ (measured only).
  for (double fOut : {2000.0, 6000.0, 9000.0, 11000.0, 14000.0}) {
    double lvl[2];
    for (int k = 0; k < 2; ++k) {
      const double r = k == 0 ? 3.999 : 4.001;
      const auto y = RunSine(ResamplerQuality::Sinc, r, fOut, 9600, 480);  // f Hz in = f Hz out
      lvl[k] = dsptest::Db(FitSine(y.data() + 512, 8192, fOut / kOut, 512).amp / 0.5);
    }
    snd::test::Note("tone at %.0f Hz out: ratio 3.999 %.3f dB, 4.001 %.3f dB", fOut, lvl[0], lvl[1]);
    if (fOut <= 10000.0)
      SND_CHECK_MSG(std::fabs(lvl[0] - lvl[1]) < 0.1, "tone at %.0f Hz: level jumps %.3f dB across ratio 4", fOut, lvl[1] - lvl[0]);
  }
  // Ramp 3.6 -> 4.4 -> 3.6 in 128-frame blocks under a 1 kHz output tone: no click.
  Guarded g(ResamplerQuality::Sinc, 1);
  std::vector<std::vector<float>> out(1);
  const auto sig = SineSig(1000.0, 4.0 * 48000.0, 0.5);  // output 1000 * r / 4 Hz: 0.9..1.1 kHz
  for (int b = 0; b < 80; ++b) {
    const int k = b < 40 ? b : 80 - b;
    g.Run(sig, out, 128, 3.6f + 0.02f * k, 3.6f + 0.02f * (b < 40 ? k + 1 : k - 1));
  }
  float worst = 0.0f;
  std::size_t at = 0;
  for (std::size_t i = 64; i < out[0].size(); ++i) {
    const float d2 = std::fabs(out[0][i] - 2.0f * out[0][i - 1] + out[0][i - 2]);
    if (d2 > 0.015f) snd::test::Note("  d2 %.4f at %zu (block %zu + %zu)", static_cast<double>(d2), i, i / 128, i % 128);
    if (d2 > worst) {
      worst = d2;
      at = i;
    }
  }
  // A 1.1 kHz sine of 0.5 has |second difference| <= 0.5 (2 pi 1100 / 48k)^2 = 0.0104.
  snd::test::Note("ramp across ratio 4: max second difference %.4f at %zu", static_cast<double>(worst), at);
  SND_CHECK_MSG(worst < 0.015f, "click %.4f at output frame %zu (block %zu)", static_cast<double>(worst), at, at / 128);
  SND_CHECK_EQ(g.violations, 0u);
}
