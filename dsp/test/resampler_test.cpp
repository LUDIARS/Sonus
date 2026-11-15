// Resampler: passband flatness, aliasing, block-split invariance, golden.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/resampler.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/golden.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;
using dsptest::Audio;

namespace {

using Signal = std::function<float(std::uint32_t ch, std::uint64_t frame)>;

// Drives one resampler over `blocks` (output frame counts) with ratio ramps.
struct Driver {
  ResamplerQuality q;
  std::uint32_t channels;
  dsptest::AlignedMem state;
  std::uint64_t srcPos = 0;
  Driver(ResamplerQuality quality, std::uint32_t ch)
      : q(quality), channels(ch), state(ResamplerStateBytes(quality, ch), dsptest::kDirty) {
    ResamplerInit(state.get(), q, ch);
  }
  // Appends `frames` output frames (ratio r0 -> r1) to out[c] at `offset`.
  void Run(const Signal& sig, std::vector<std::vector<float>>& out, std::uint32_t frames,
           float r0, float r1) {
    const std::uint32_t pad = ResamplerPadFrames(q);
    const std::uint32_t need = ResamplerInputFrames(state.get(), frames, r0, r1);
    Audio in(channels, need + 1, pad);
    Audio o(channels, frames);
    for (std::uint32_t c = 0; c < channels; ++c)
      for (std::uint32_t i = 0; i < need; ++i) in.ch(c)[i] = sig(c, srcPos + i);
    float* base[8];
    for (std::uint32_t c = 0; c < channels; ++c) base[c] = in.Base(c);
    std::uint32_t used;
    {
      SND_RT_SCOPE();
      used = ResamplerProcess(state.get(), base, need, o.p(), frames, r0, r1);
    }
    SND_CHECK_EQ(used, need);
    srcPos += need;
    for (std::uint32_t c = 0; c < channels; ++c) out[c].insert(out[c].end(), o.ch(c), o.ch(c) + frames);
  }
};

Signal Sine(double freq, double rate, float amp = 0.5f) {
  return [=](std::uint32_t, std::uint64_t n) {
    return amp * static_cast<float>(std::sin(2.0 * dsptest::kPi * freq * static_cast<double>(n) / rate));
  };
}

// Amplitude (dB re. amp) of `freqOut` in the steady part of a resampled sine.
double MeasureTone(ResamplerQuality q, double ratio, double freqIn, double freqMeasure) {
  const double outRate = 48000.0, inRate = 48000.0 * ratio;
  Driver d(q, 1);
  std::vector<std::vector<float>> out(1);
  for (int b = 0; b < 40; ++b) d.Run(Sine(freqIn, inRate), out, 256, static_cast<float>(ratio), static_cast<float>(ratio));
  const std::uint32_t skip = 1024, n = 8192;
  return dsptest::Db(dsptest::ToneAmplitude(out[0].data() + skip, n, freqMeasure, outRate) / 0.5);
}

}  // namespace

SND_TEST(Resampler, SizesAndInlineFit) {
  SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Linear), 2u);
  SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Cubic), 4u);
  SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Sinc, 4.0f), 32u);
  SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Sinc, 8.0f), 96u);
  SND_CHECK_EQ(ResamplerHistoryFrames(ResamplerQuality::Sinc), 384u);
  SND_CHECK_EQ(ResamplerLookaheadFrames(ResamplerQuality::Sinc, 1.0f), 16u);
  SND_CHECK_EQ(ResamplerLookaheadFrames(ResamplerQuality::Sinc, 32.0f), 192u);
  SND_CHECK_EQ(ResamplerPadFrames(ResamplerQuality::Sinc) % 16u, 0u);
  SND_CHECK(ResamplerFitsInline(ResamplerQuality::Linear, 2));
  SND_CHECK(ResamplerFitsInline(ResamplerQuality::Cubic, 2));
  SND_CHECK(!ResamplerFitsInline(ResamplerQuality::Sinc, 1));
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    snd::test::Note("state bytes ch=%u: linear %zu cubic %zu sinc(max ratio 4) %zu sinc(max 32) %zu", ch,
                    ResamplerStateBytes(ResamplerQuality::Linear, ch),
                    ResamplerStateBytes(ResamplerQuality::Cubic, ch),
                    ResamplerStateBytes(ResamplerQuality::Sinc, ch, 4.0f),
                    ResamplerStateBytes(ResamplerQuality::Sinc, ch));
  }
}

SND_TEST(Resampler, UnitRatioIsIdentityForLinearAndCubic) {
  for (ResamplerQuality q : {ResamplerQuality::Linear, ResamplerQuality::Cubic}) {
    Driver d(q, 2);
    std::vector<std::vector<float>> out(2);
    auto sig = [](std::uint32_t c, std::uint64_t n) { return static_cast<float>((n * 7 + c * 3) % 11) - 5.0f; };
    for (int b = 0; b < 4; ++b) d.Run(sig, out, 100, 1.0f, 1.0f);
    for (std::uint32_t i = 0; i < 400; ++i) {
      SND_CHECK_EQ(out[0][i], sig(0, i));
      SND_CHECK_EQ(out[1][i], sig(1, i));
    }
    SND_CHECK_EQ(d.srcPos, 400u + ResamplerHistoryFrames(q) / 2u);
  }
}

SND_TEST(Resampler, SincPassbandIsFlat) {
  const double ratios[] = {0.5, 0.75, 1.0, 1.2, 1.5, 2.0, 3.0};
  for (double r : ratios) {
    // 32 taps: the transition band is ~0.16 of the source rate, so above ratio 2
    // the passband narrows to ~(0.45 / tableRatio - 0.085) * sourceRate.
    const double tableRatio = r <= 2.0 ? r : 3.4;
    const double limit = std::min(0.28 * std::min(48000.0 * r, 48000.0),
                                  (0.45 / std::max(1.0, tableRatio) - 0.085) * 48000.0 * r);
    for (double f : {100.0, 1000.0, 5000.0, 10000.0, 13000.0}) {
      if (f > limit) continue;
      const double db = MeasureTone(ResamplerQuality::Sinc, r, f, f);
      SND_CHECK_MSG(std::fabs(db) < 0.1, "ratio %.2f f %.0f Hz: %.3f dB", r, f, db);
    }
  }
}

SND_TEST(Resampler, SincSuppressesAliasing) {
  struct Case {
    double ratio, fin;
  };
  // Input tones above the output Nyquist; alias lands at 48000 - fin.
  const Case cases[] = {{2.0, 0.35 * 96000.0}, {1.5, 0.40 * 72000.0}, {3.0, 0.40 * 144000.0}};
  for (const Case& c : cases) {
    const double alias = 48000.0 - std::fmod(c.fin, 48000.0);
    const double sinc = MeasureTone(ResamplerQuality::Sinc, c.ratio, c.fin, alias);
    const double cubic = MeasureTone(ResamplerQuality::Cubic, c.ratio, c.fin, alias);
    snd::test::Note("ratio %.1f tone %.0f Hz -> alias %.0f Hz: sinc %.1f dB, cubic %.1f dB", c.ratio,
                    c.fin, alias, sinc, cubic);
    SND_CHECK_MSG(sinc < -70.0, "alias %.1f dB", sinc);
    SND_CHECK(cubic > sinc);
  }
  // Upsampling: no images of an input tone near the input Nyquist.
  const double img = MeasureTone(ResamplerQuality::Sinc, 0.5, 10000.0, 24000.0 - 10000.0);
  SND_CHECK_MSG(img < -70.0, "image %.1f dB", img);
}

// Ratios above 4 (ratio-scaled kernel): passband edge, aliasing and cost per ratio.
SND_TEST(Resampler, SincHighRatios) {
  for (double r : {4.0, 4.5, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0}) {
    double edge = 0.0;  // highest tone (250 Hz steps) within +-0.1 dB
    for (double f = 1000.0; f < 24000.0; f += 250.0) {
      if (std::fabs(MeasureTone(ResamplerQuality::Sinc, r, f, f)) > 0.1) break;
      edge = f;
    }
    double worst = -300.0;  // alias of source tones between 0.75 and 0.98 x the output rate
    for (double x = 0.75; x <= 0.981; x += 0.01) {
      const double fin = x * 48000.0;
      worst = std::max(worst, MeasureTone(ResamplerQuality::Sinc, r, fin, 48000.0 - fin));
    }
    double us[2];  // one voice, 256 frames per call
    for (std::uint32_t ch = 1; ch <= 2; ++ch) {
      Driver d(ResamplerQuality::Sinc, ch);
      const std::uint32_t pad = ResamplerPadFrames(ResamplerQuality::Sinc);
      Audio in(ch, static_cast<std::uint32_t>(256 * r) + 512, pad), o(ch, 256);
      float* base[2] = {in.Base(0), ch > 1 ? in.Base(1) : nullptr};
      constexpr int kIter = 200;
      const auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < kIter; ++k) {
        const std::uint32_t n = ResamplerInputFrames(d.state.get(), 256, static_cast<float>(r), static_cast<float>(r));
        ResamplerProcess(d.state.get(), base, n, o.p(), 256, static_cast<float>(r), static_cast<float>(r));
      }
      const auto t1 = std::chrono::steady_clock::now();
      us[ch - 1] = std::chrono::duration<double, std::micro>(t1 - t0).count() / kIter;
    }
    snd::test::Note("sinc ratio %4.1f: lookahead %3u, passband to %5.0f Hz, worst alias %.1f dB, "
                    "%.1f us (mono) / %.1f us (stereo) per 256 frames",
                    r, ResamplerLookaheadFrames(ResamplerQuality::Sinc, static_cast<float>(r)), edge, worst, us[0], us[1]);
    SND_CHECK(edge >= 9000.0);
    // Ratio 4 still uses the 32-tap table (stopband from ~0.76 x outRate): measured only.
    if (r > 4.0) SND_CHECK_MSG(worst < -78.0, "ratio %.1f alias %.1f dB", r, worst);
  }
}

SND_TEST(Resampler, BlockSplitInvariance) {
  auto sig = [](std::uint32_t c, std::uint64_t n) {
    return static_cast<float>(std::sin(0.013 * static_cast<double>(n) * (1.0 + c)) +
                              0.3 * std::sin(0.31 * static_cast<double>(n)));
  };
  for (ResamplerQuality q : {ResamplerQuality::Linear, ResamplerQuality::Cubic, ResamplerQuality::Sinc}) {
    // Constant ratio: bit identical whatever the split.
    Driver a(q, 2), b(q, 2);
    std::vector<std::vector<float>> oa(2), ob(2);
    a.Run(sig, oa, 1024, 1.37f, 1.37f);
    for (std::uint32_t n : {100u, 1u, 300u, 256u, 367u}) b.Run(sig, ob, n, 1.37f, 1.37f);
    SND_REQUIRE_EQ(ob[0].size(), 1024u);
    SND_CHECK_EQ(a.srcPos, b.srcPos);
    std::uint32_t diff = 0;
    for (std::uint32_t c = 0; c < 2; ++c)
      for (std::uint32_t i = 0; i < 1024; ++i) diff += oa[c][i] != ob[c][i] ? 1u : 0u;
    SND_CHECK_EQ(diff, 0u);
    // Ramped ratio split at the matching intermediate ratio: equal within rounding.
    Driver c1(q, 1), c2(q, 1);
    std::vector<std::vector<float>> o1(1), o2(1);
    c1.Run(sig, o1, 256, 0.8f, 1.6f);
    c2.Run(sig, o2, 128, 0.8f, 1.2f);
    c2.Run(sig, o2, 128, 1.2f, 1.6f);
    SND_CHECK(c1.srcPos == c2.srcPos || c1.srcPos + 1 == c2.srcPos || c2.srcPos + 1 == c1.srcPos);
    // (skip the start-up edge: the sinc tables differ between the two halves)
    const float rd = dsptest::MaxAbsDiff(o1[0].data() + 32, o2[0].data() + 32, 224);
    SND_CHECK_MSG(rd < 1e-3f, "quality %u ramp split diff %g (src %llu / %llu)", static_cast<unsigned>(q),
                  static_cast<double>(rd), static_cast<unsigned long long>(c1.srcPos),
                  static_cast<unsigned long long>(c2.srcPos));
  }
}

SND_TEST(Resampler, ConsumesSourceAtTheRequestedRate) {
  Driver d(ResamplerQuality::Sinc, 1);
  std::vector<std::vector<float>> out(1);
  for (int b = 0; b < 100; ++b) d.Run(Sine(440.0, 44100.0), out, 256, 44100.0f / 48000.0f, 44100.0f / 48000.0f);
  const double expected = 100.0 * 256.0 * 44100.0 / 48000.0;
  SND_CHECK(std::fabs(static_cast<double>(d.srcPos) - expected - 16.0) <= 2.0);
  SND_CHECK(std::fabs(ResamplerPendingOffset(d.state.get())) <= 32.0);
  // Insufficient input: silence, state unchanged.
  Audio in(1, 4, ResamplerPadFrames(ResamplerQuality::Sinc)), o(1, 64);
  float* base[1] = {in.Base(0)};
  o.ch(0)[0] = 1.0f;
  SND_CHECK_EQ(ResamplerProcess(d.state.get(), base, 4, o.p(), 64, 1.0f, 1.0f), 0u);
  SND_CHECK_EQ(o.ch(0)[0], 0.0f);
}

SND_TEST(Resampler, GoldenGlide) {
  // A two-tone signal at 44.1 kHz glided from ratio 0.7 to 1.8 (pitch ramp).
  for (ResamplerQuality q : {ResamplerQuality::Sinc, ResamplerQuality::Cubic}) {
    Driver d(q, 2);
    std::vector<std::vector<float>> out(2);
    auto sig = [](std::uint32_t c, std::uint64_t n) {
      const double t = static_cast<double>(n) / 44100.0;
      return static_cast<float>(0.4 * std::sin(2 * dsptest::kPi * (c ? 997.0 : 3001.0) * t) +
                                0.2 * std::sin(2 * dsptest::kPi * 9000.0 * t));
    };
    for (int b = 0; b < 8; ++b) {
      const float r0 = 0.7f + 1.1f * static_cast<float>(b) / 8.0f;
      const float r1 = 0.7f + 1.1f * static_cast<float>(b + 1) / 8.0f;
      d.Run(sig, out, 256, r0, r1);
    }
    snd::test::AudioBuffer buf(2, 2048);
    for (std::uint32_t c = 0; c < 2; ++c) std::memcpy(buf.Channel(c), out[c].data(), 2048 * sizeof(float));
    snd::test::GoldenTolerance tol;
    tol.maxAbs = 1e-4f;
    tol.maxRms = 1e-5f;
    SND_CHECK_GOLDEN(q == ResamplerQuality::Sinc ? "resampler_sinc_glide" : "resampler_cubic_glide", buf, tol);
  }
}
