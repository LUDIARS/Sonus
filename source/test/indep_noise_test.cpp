// Independent tests of the Noise kind (stage 7-D, CONTRACTS.md D-193), written from the
// specification: the white sequence is recomputed from the text of D-193 (splitmix64
// counter, top 24 bits), the colours from their filters (Kellett's economy pink, the leaky
// brown integrator) in double precision, the spectra against the analytic responses of
// those filters and against the nominal slopes, the pitch against the 32.32 reading rule.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "indep_src1_util.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using synthtest::Driver;
namespace src = snd::source;
using isrc::kPi;
using isrc::kTwoPi;

namespace {

constexpr double kRate = 48000.0;

// Reference colours in double precision (D-193).
struct RefNoise {
  std::uint32_t color;
  double b[3] = {0, 0, 0};
  double brown = 0;
  double pinkScale = 1.0;
  explicit RefNoise(std::uint32_t c) : color(c) {
    const double p[4] = {0.99765, 0.963, 0.57, 0.0};
    const double g[4] = {0.0990460, 0.2965164, 1.0526913, 0.1848};
    double v = 0.0;  // variance gain of the parallel one-pole sum (white input)
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) v += g[i] * g[j] / (1.0 - p[i] * p[j]);
    }
    pinkScale = 1.0 / std::sqrt(v);
  }
  double Step(double w) {
    if (color == 1u) {
      b[0] = 0.99765 * b[0] + 0.0990460 * w;
      b[1] = 0.96300 * b[1] + 0.2965164 * w;
      b[2] = 0.57000 * b[2] + 1.0526913 * w;
      return (b[0] + b[1] + b[2] + 0.1848 * w) * pinkScale;
    }
    if (color == 2u) {
      brown = 0.995 * brown + std::sqrt(1.0 - 0.995 * 0.995) * w;
      return brown;
    }
    return w;
  }
};

// Y(c, 0 .. n) of a voice from the spec.
std::vector<double> RefY(std::uint32_t color, std::uint32_t bankSeed, std::uint32_t startSeed, std::uint32_t c,
                         std::size_t n) {
  const std::uint64_t ck = isrc::SpecChannelKey(isrc::SpecVoiceKey(bankSeed, startSeed), c);
  RefNoise f(color);
  std::vector<double> y(n);
  for (std::size_t k = 0; k < n; ++k) y[k] = f.Step(isrc::SpecWhite(ck, k));
  return y;
}

std::vector<std::vector<float>> Render(std::uint32_t color, std::uint32_t bankSeed, std::uint32_t startSeed,
                                       std::size_t frames, std::uint32_t channels = 1, std::uint32_t block = 256,
                                       const Driver::Mod& m = {}, std::uint64_t startFrame = 0) {
  Driver d(snd::SourceKind::Noise, Driver::Opt{});
  Driver::Voice v;
  src::NoiseParams p{color, bankSeed};
  if (!d.Ok() || !d.Start(v, &p, channels, startSeed, startFrame)) return {};
  d.Render(v, frames, m, block);
  return v.out;
}

// |H(e^jw)|^2 of the colour filters (normalised as above).
double Response(std::uint32_t color, double hz) {
  const double w = kTwoPi * hz / kRate;
  const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -w));
  if (color == 1u) {
    RefNoise r(1);
    const std::complex<double> h = 0.0990460 / (1.0 - 0.99765 * z1) + 0.2965164 / (1.0 - 0.963 * z1) +
                                   1.0526913 / (1.0 - 0.57 * z1) + 0.1848;
    return std::norm(h) * r.pinkScale * r.pinkScale;
  }
  if (color == 2u) return (1.0 - 0.995 * 0.995) / std::norm(1.0 - 0.995 * z1);
  return 1.0;
}

double BandDb(const std::vector<double>& p, double lo, double hi, std::size_t seg) {
  const double bin = kRate / static_cast<double>(seg);
  double s = 0;
  std::size_t c = 0;
  for (std::size_t k = static_cast<std::size_t>(std::ceil(lo / bin)); k < static_cast<std::size_t>(hi / bin); ++k) {
    s += p[k];
    ++c;
  }
  return 10.0 * std::log10(s / double(c));
}
double AnalyticBandDb(std::uint32_t color, double lo, double hi, std::size_t seg) {
  const double bin = kRate / static_cast<double>(seg);
  double s = 0;
  std::size_t c = 0;
  for (std::size_t k = static_cast<std::size_t>(std::ceil(lo / bin)); k < static_cast<std::size_t>(hi / bin); ++k) {
    s += Response(color, double(k) * bin);
    ++c;
  }
  return 10.0 * std::log10(s / double(c));
}

double Slope(const std::vector<double>& xs, const std::vector<double>& ys) {
  double mx = 0, my = 0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    mx += xs[i];
    my += ys[i];
  }
  mx /= double(xs.size());
  my /= double(xs.size());
  double num = 0, den = 0;
  for (std::size_t i = 0; i < xs.size(); ++i) {
    num += (xs[i] - mx) * (ys[i] - my);
    den += (xs[i] - mx) * (xs[i] - mx);
  }
  return num / den;
}

}  // namespace

// N1 / N4 / C6: the white sequence is exactly the D-193 counter construction, on every
// channel (own key per channel), for several seeds incl. 0 and all ones; values in [-1, 1).
SND_TEST(IndepNoise, WhiteIsTheSpecifiedCounterSequence) {
  const std::uint32_t seeds[][2] = {{0, 0}, {1, 0}, {0, 1}, {0xFFFFFFFFu, 0xFFFFFFFFu}, {12345, 678}};
  for (const auto& s : seeds) {
    const auto x = Render(0, s[0], s[1], 3000, 8);
    SND_REQUIRE_EQ(x.size(), std::size_t{8});
    const std::uint64_t key = isrc::SpecVoiceKey(s[0], s[1]);
    std::size_t bad = 0;
    for (std::uint32_t c = 0; c < 8; ++c) {
      const std::uint64_t ck = isrc::SpecChannelKey(key, c);
      for (std::size_t k = 0; k < 3000; ++k) {
        const float want = static_cast<float>(isrc::SpecWhite(ck, k));
        if (x[c][k] != want) ++bad;
        SND_CHECK(x[c][k] >= -1.0f && x[c][k] < 1.0f);
      }
    }
    SND_CHECK_MSG(bad == 0u, "seeds %u/%u: %u samples differ from D-193", s[0], s[1], unsigned(bad));
  }
}

// N2 / N3: pink and brown follow their filters (double precision reference, float in the
// kind): the colour is the specified one, normalised to the white variance.
SND_TEST(IndepNoise, ColoursFollowTheirFilters) {
  for (std::uint32_t color : {1u, 2u}) {
    const auto x = Render(color, 77, 88, 20000, 2);
    SND_REQUIRE_EQ(x.size(), std::size_t{2});
    for (std::uint32_t c = 0; c < 2; ++c) {
      const std::vector<double> y = RefY(color, 77, 88, c, 20000);
      double err = 0;
      for (std::size_t k = 0; k < 20000; ++k) err = std::fmax(err, std::fabs(x[c][k] - y[k]));
      SND_CHECK_MSG(err < 2e-4, "colour %u ch %u: max deviation from the filter %g", color, c, err);
    }
  }
}

// N2 / N3 / N10: spectra (Welch, 2^20 samples) per octave band 63 Hz .. 16 kHz against the
// analytic response of the filter (+-0.5 dB), the fitted slope 250 Hz .. 8 kHz against the
// nominal 0 / -3 / -6 dB per octave, the RMS (0.577, 3 %), no DC drift, peaks (reported).
SND_TEST(IndepNoise, SpectraLevelsAndDc) {
  const std::size_t n = 1u << 20, seg = 8192;
  const double nominal[3] = {0.0, -3.0103, -6.0206};
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto x = Render(color, 3, 9, n, 2, 1024);
    SND_REQUIRE_EQ(x.size(), std::size_t{2});
    const std::vector<double> p = isrc::Welch(x[0], seg);
    // Offset between measured and analytic (window gain etc.): from the 1 kHz octave.
    const double off = BandDb(p, 1000 / std::sqrt(2.0), 1000 * std::sqrt(2.0), seg) -
                       AnalyticBandDb(color, 1000 / std::sqrt(2.0), 1000 * std::sqrt(2.0), seg);
    std::vector<double> xs, ys;
    for (double fc = 62.5; fc <= 16000.0; fc *= 2.0) {
      const double lo = fc / std::sqrt(2.0), hi = fc * std::sqrt(2.0);
      const double m = BandDb(p, lo, hi, seg) - off;
      const double a = AnalyticBandDb(color, lo, hi, seg);
      SND_CHECK_MSG(std::fabs(m - a) < 0.5, "colour %u %g Hz: %.2f dB vs analytic %.2f", color, fc, m, a);
      if (fc >= 250.0 && fc <= 8000.0) {
        xs.push_back(std::log2(fc));
        ys.push_back(m);
      }
    }
    const double s = Slope(xs, ys);
    std::printf("noise colour %u: slope %.2f dB/oct (250 Hz .. 8 kHz)\n", color, s);
    SND_CHECK_MSG(std::fabs(s - nominal[color]) < 0.25, "colour %u: slope %.2f", color, s);
    const double rms = 0.5 * (isrc::RmsOf(x[0]) + isrc::RmsOf(x[1]));
    const double mean = 0.5 * (isrc::Mean(x[0]) + isrc::Mean(x[1]));
    std::printf("noise colour %u: rms %.4f mean %.5f peak %.3f\n", color, rms, mean,
                std::fmax(isrc::MaxAbs(x[0]), isrc::MaxAbs(x[1])));
    SND_CHECK_NEAR(rms, 1.0 / std::sqrt(3.0), 0.03 / std::sqrt(3.0));
    SND_CHECK_MSG(std::fabs(mean) < 0.02, "colour %u: mean %g", color, mean);
    SND_CHECK(isrc::AllFinite(x[0]) && isrc::AllFinite(x[1]));
    SND_CHECK(isrc::MaxAbs(x[0]) < 4.0);
  }
  // Brown over 2^22 samples: the mean of each quarter stays near 0 (leaky, no drift).
  const auto b = Render(2, 1, 2, 1u << 22, 1, 4096);
  for (int q = 0; q < 4; ++q) {
    const double m = isrc::Mean(b[0], std::size_t(q) << 20, 1u << 20);
    SND_CHECK_MSG(std::fabs(m) < 0.05, "brown quarter %d: mean %g", q, m);
  }
}

// N4 / N5: the same seeds give the same bits for any block layout, any order inside the
// batch, alone or with other voices; another bank seed, another start seed, another
// colour each give another noise.
SND_TEST(IndepNoise, BitIdentityAcrossBlocksBatchesAndSeeds) {
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto ref = Render(color, 5, 6, 6000, 2, 6000);
    Driver d(snd::SourceKind::Noise, Driver::Opt{});
    SND_REQUIRE(d.Ok());
    src::NoiseParams pa{color, 5}, pb{color, 7};
    Driver::Voice a, b, c;
    SND_REQUIRE(d.Start(a, &pa, 2, 6));
    SND_REQUIRE(d.Start(b, &pb, 2, 6));
    SND_REQUIRE(d.Start(c, &pa, 2, 99));
    const std::uint32_t sizes[] = {1, 255, 3, 1024, 17, 700};
    std::size_t done = 0;
    for (int i = 0; done < 6000; ++i) {
      std::uint32_t n = sizes[i % 6];
      if (done + n > 6000) n = static_cast<std::uint32_t>(6000 - done);
      if (i % 3 == 0) {
        d.Block({&a, &b, &c}, Driver::Mod{}, n, 0, n);
      } else if (i % 3 == 1) {
        d.Block({&c, &a, &b}, Driver::Mod{}, n, 0, n);
      } else {
        d.Block({&b}, Driver::Mod{}, n, 0, n);
        d.Block({&a}, Driver::Mod{}, n, 0, n);
        d.Block({&c}, Driver::Mod{}, n, 0, n);
      }
      done += n;
    }
    SND_CHECK_MSG(a.out == ref, "colour %u: not bit identical", color);
    SND_CHECK(b.out[0] != ref[0]);
    SND_CHECK(c.out[0] != ref[0]);
    SND_CHECK(b.out[0] != c.out[0]);
  }
  SND_CHECK(Render(0, 5, 6, 512)[0] != Render(1, 5, 6, 512)[0]);
}

// N6: channels of one voice are uncorrelated (first differences remove the long memory of
// pink / brown so the estimate is tight), each with the same level.
SND_TEST(IndepNoise, ChannelsAreUncorrelated) {
  const std::size_t n = 1u << 18;
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto x = Render(color, 21, 22, n, 4, 512);
    SND_REQUIRE_EQ(x.size(), std::size_t{4});
    std::vector<std::vector<float>> dx(4, std::vector<float>(n - 1));
    for (int c = 0; c < 4; ++c) {
      for (std::size_t i = 1; i < n; ++i) dx[std::size_t(c)][i - 1] = x[std::size_t(c)][i] - x[std::size_t(c)][i - 1];
    }
    for (int i = 0; i < 4; ++i) {
      for (int j = i + 1; j < 4; ++j) {
        const double r = isrc::Corr(dx[std::size_t(i)], dx[std::size_t(j)]);
        SND_CHECK_MSG(std::fabs(r) < 0.015, "colour %u ch %d/%d: correlation %g", color, i, j, r);
      }
    }
    if (color == 0u) SND_CHECK(std::fabs(isrc::Corr(x[0], x[1])) < 0.015);
  }
}

// N7: the pitch reads the source samples at the rate ratio with linear interpolation, the
// position in 32.32 fixed point (increment = round(ratio x 2^32)).
SND_TEST(IndepNoise, PitchReadsTheSequenceAtTheRatio) {
  const auto y = Render(1, 8, 9, 12000);  // Y at ratio 1 (= the source samples)
  SND_REQUIRE_EQ(y.size(), std::size_t{1});
  Driver::Mod m2;
  m2.pitch = snd::Ramp{2.0f, 2.0f};
  const auto x2 = Render(1, 8, 9, 5000, 1, 256, m2);
  for (std::size_t i = 0; i < 5000; ++i) SND_CHECK_EQ(x2[0][i], y[0][2 * i]);
  Driver::Mod mh;
  mh.pitch = snd::Ramp{0.5f, 0.5f};
  const auto xh = Render(1, 8, 9, 5000, 1, 256, mh);
  for (std::size_t i = 0; i < 2500; ++i) {
    SND_CHECK_EQ(xh[0][2 * i], y[0][i]);
    SND_CHECK_EQ(xh[0][2 * i + 1], y[0][i] + (y[0][i + 1] - y[0][i]) * 0.5f);
  }
  // 1/3 and a ramp 0.7 -> 1.9 over each block: the position from the rule.
  for (int mode = 0; mode < 2; ++mode) {
    Driver::Mod m;
    m.pitch = mode == 0 ? snd::Ramp{1.0f / 3.0f, 1.0f / 3.0f} : snd::Ramp{0.7f, 1.9f};
    const auto x = Render(1, 8, 9, 2048, 1, 256, m);
    std::uint64_t pos = 0;  // 32.32
    double err = 0;
    for (std::size_t i = 0; i < 2048; ++i) {
      const std::size_t k = static_cast<std::size_t>(pos >> 32);
      const double fr = static_cast<double>(pos & 0xFFFFFFFFu) / 4294967296.0;
      const double want = y[0][k] + (double(y[0][k + 1]) - y[0][k]) * fr;
      err = std::fmax(err, std::fabs(x[0][i] - want));
      const double ratio = double(m.pitch.start) + (double(m.pitch.end) - m.pitch.start) * double(i % 256) / 256.0;
      pos += static_cast<std::uint64_t>(std::llround(ratio * 4294967296.0));
    }
    SND_CHECK_MSG(err < 1e-6, "mode %d: max error %g", mode, err);
  }
}

// N8: AdvanceVirtual moves the position by outputFrames x pitch (32.32) without
// rendering; white stays exactly on the sequence, pink / brown are within the rebuild
// tolerance of the uninterrupted colour; Seek(frame) goes to source sample `frame`
// (exact for every colour before 4096); startFrame likewise.
SND_TEST(IndepNoise, VirtualAdvanceSeekAndStartFrame) {
  const std::uint64_t ck = isrc::SpecChannelKey(isrc::SpecVoiceKey(31, 32), 0);
  {
    Driver d(snd::SourceKind::Noise, Driver::Opt{});
    src::NoiseParams p{0, 31};
    Driver::Voice v;
    SND_REQUIRE(d.Start(v, &p, 1, 32));
    d.Render(v, 1000);
    d.Advance(v, 1000, 0.3f);  // 1000 x round(0.3f x 2^32) in 32.32
    SND_CHECK_EQ(v.ph.frame, std::uint64_t{2000});
    const std::uint64_t total = static_cast<std::uint64_t>(std::llround(double(0.3f) * 4294967296.0)) * 1000u;
    const std::uint64_t k0 = 1000u + (total >> 32);
    const double fr = static_cast<double>(total & 0xFFFFFFFFu) / 4294967296.0;
    d.Render(v, 500);
    double err = 0;
    for (std::size_t i = 0; i < 500; ++i) {
      const double a = isrc::SpecWhite(ck, k0 + i), b = isrc::SpecWhite(ck, k0 + 1 + i);
      err = std::fmax(err, std::fabs(v.out[0][1000 + i] - (a + (b - a) * fr)));
    }
    SND_CHECK_MSG(err < 1e-6, "white after virtual: %g", err);
    d.Control(v, src::ControlOp::Seek, 1000000);
    d.Render(v, 300);
    for (std::size_t i = 0; i < 300; ++i) SND_CHECK_EQ(v.out[0][1500 + i], static_cast<float>(isrc::SpecWhite(ck, 1000000 + i)));
    for (src::ControlOp op : {src::ControlOp::Pause, src::ControlOp::Resume, src::ControlOp::Virtualize,
                              src::ControlOp::Devirtualize, src::ControlOp::SetLoop}) {
      d.Control(v, op, 3);
    }
    d.Render(v, 300);
    for (std::size_t i = 0; i < 300; ++i) SND_CHECK_EQ(v.out[0][1800 + i], static_cast<float>(isrc::SpecWhite(ck, 1000300 + i)));
    SND_CHECK_EQ(v.last.ended, 0u);
  }
  for (std::uint32_t color : {1u, 2u}) {
    const std::vector<double> y = RefY(color, 31, 32, 0, 60000);
    Driver d(snd::SourceKind::Noise, Driver::Opt{});
    src::NoiseParams p{color, 31};
    Driver::Voice v;
    SND_REQUIRE(d.Start(v, &p, 1, 32));
    d.Render(v, 1000);
    d.Advance(v, 20000, 1.0f);
    d.Render(v, 2000);
    double err = 0;
    for (std::size_t i = 0; i < 2000; ++i) err = std::fmax(err, std::fabs(v.out[0][1000 + i] - y[21000 + i]));
    std::printf("noise colour %u: after a virtual advance max deviation %g\n", color, err);
    SND_CHECK_MSG(err < 1e-3, "colour %u after virtual: %g", color, err);
    d.Control(v, src::ControlOp::Seek, 50000);
    d.Render(v, 2000);
    err = 0;
    for (std::size_t i = 0; i < 2000; ++i) err = std::fmax(err, std::fabs(v.out[0][3000 + i] - y[50000 + i]));
    SND_CHECK_MSG(err < 1e-3, "colour %u after seek: %g", color, err);
    d.Control(v, src::ControlOp::Seek, 3000);  // inside the warm-up window: exact filter state
    d.Render(v, 1000);
    err = 0;
    for (std::size_t i = 0; i < 1000; ++i) err = std::fmax(err, std::fabs(v.out[0][5000 + i] - y[3000 + i]));
    SND_CHECK_MSG(err < 2e-4, "colour %u after an early seek: %g", color, err);
  }
  // startFrame: the voice starts at source sample startFrame.
  const auto s = Render(0, 31, 32, 200, 1, 256, {}, 777);
  for (std::size_t i = 0; i < 200; ++i) SND_CHECK_EQ(s[0][i], static_cast<float>(isrc::SpecWhite(ck, 777 + i)));
}

// N9 / C2 / O6: unknown colours are refused; a released slot restarted with the same seeds
// is bit identical to a fresh voice (no filter state left); frames before frameOffset are
// not written; more output channels than the voice's stay silent.
SND_TEST(IndepNoise, RefusalsSlotReuseAndFrameOffset) {
  Driver::Opt o;
  o.maxVoices = 1;
  Driver d(snd::SourceKind::Noise, o);
  SND_REQUIRE(d.Ok());
  for (std::uint32_t bad : {3u, 4u, 0xFFFFFFFFu}) {
    src::NoiseParams p{bad, 1};
    Driver::Voice v;
    SND_CHECK(!d.Start(v, &p, 1));
  }
  SND_CHECK_EQ(d.runtime.Stats().noiseInstances, 0u);
  src::NoiseParams pk{1, 41};
  const auto fresh = Render(1, 41, 42, 3000);
  Driver::Voice a;
  SND_REQUIRE(d.Start(a, &pk, 1, 42));
  d.Render(a, 9000);
  d.Advance(a, 5000, 2.0f);
  Driver::Voice x;
  SND_CHECK(!d.Start(x, &pk, 1, 42));
  SND_CHECK_EQ(x.endDetail, std::uint32_t{SND_END_DETAIL_VOICE_LIMIT});
  d.Release(a);
  Driver::Voice b;
  SND_REQUIRE(d.Start(b, &pk, 1, 42));
  d.Render(b, 3000);
  SND_CHECK(b.out[0] == fresh[0]);
  // frameOffset: a 2-channel voice rendered into a 3-channel item from frame 100.
  Driver::Voice c;
  d.Release(b);
  src::NoiseParams pw{0, 41};
  SND_REQUIRE(d.Start(c, &pw, 2, 42));
  std::vector<float> buf[3];
  float* ptrs[3];
  for (int i = 0; i < 3; ++i) {
    buf[i].assign(256, 0.0f);
    ptrs[i] = buf[i].data();
  }
  src::RenderItem it{};
  it.sourceSlot = c.slot;
  it.frameOffset = 100;
  it.framesRequested = 156;
  it.channels = 3;
  it.pitch = snd::Ramp{1.0f, 1.0f};
  it.playhead = &c.ph;
  it.out = ptrs;
  src::RenderBatch bt{};
  bt.kind = snd::SourceKind::Noise;
  bt.count = 1;
  bt.items = &it;
  bt.block.frames = 256;
  bt.block.sampleRate = 48000;
  snd::VoiceRenderStatus st{};
  {
    snd::rt::RtScope rt;
    d.Kind().RenderBatch(bt, &st);
  }
  const std::uint64_t key = isrc::SpecVoiceKey(41, 42);
  for (std::uint32_t ch = 0; ch < 2; ++ch) {
    for (std::size_t i = 0; i < 100; ++i) SND_CHECK_EQ(buf[ch][i], 0.0f);
    for (std::size_t i = 0; i < 156; ++i) {
      SND_CHECK_EQ(buf[ch][100 + i], static_cast<float>(isrc::SpecWhite(isrc::SpecChannelKey(key, ch), i)));
    }
  }
  for (float v : buf[2]) SND_CHECK_EQ(v, 0.0f);
  SND_CHECK_EQ(st.framesRendered, 156u);
  SND_CHECK_EQ(st.ended, 0u);
  SND_CHECK_EQ(c.ph.frame, std::uint64_t{156});
}

// Gap (recorded, not asserted as right): the colours share the RMS of the uniform white
// noise (D-193), so pink and brown, close to Gaussian, peak far above full scale (about
// +9 dB over white's peak of 1) while white never reaches 1. The headroom of a noise
// synth then depends on its colour.
SND_TEST(IndepNoise, SpecGap_ColouredPeaksExceedFullScale) {
  const auto w = Render(0, 1, 1, 1u << 18);
  const auto p = Render(1, 1, 1, 1u << 18);
  const auto b = Render(2, 1, 1, 1u << 18);
  SND_CHECK(isrc::MaxAbs(w[0]) <= 1.0);
  SND_CHECK(isrc::MaxAbs(p[0]) > 1.5);  // current behaviour
  SND_CHECK(isrc::MaxAbs(b[0]) > 1.5);  // current behaviour
}
