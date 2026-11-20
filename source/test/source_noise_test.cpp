// Noise kind (stage 7-D, D-193) driven directly (synth_driver.hpp): bit identity for
// one seed whatever the block size, spectral slope per colour (0 / -3 / -6 dB per
// octave) from averaged periodograms, level, channel independence, pitch, virtual
// advance and seek.
#include <cmath>
#include <cstdio>
#include <vector>

#include "noise_kind.hpp"
#include "snd/test/test.hpp"
#include "synth_driver.hpp"

using namespace synthtest;

namespace {

constexpr double kRate = 48000.0;

std::vector<std::vector<float>> Render(std::uint32_t color, std::uint32_t seed, std::uint32_t startSeed,
                                       std::size_t frames, std::uint32_t channels = 1, std::uint32_t block = 256,
                                       const Driver::Mod& m = {}) {
  Driver d(snd::SourceKind::Noise, Driver::Opt{});
  Driver::Voice v;
  src::NoiseParams p{color, seed};
  if (!d.Ok() || !d.Start(v, &p, channels, startSeed)) return {};
  d.Render(v, frames, m, block);
  return v.out;
}

// Welch power density (Blackman-Harris, 4096-point segments, half overlap).
std::vector<double> Density(const std::vector<float>& x) {
  const std::size_t n = 4096;
  std::vector<double> acc(n / 2 + 1, 0.0);
  std::size_t segs = 0;
  for (std::size_t from = 0; from + n <= x.size(); from += n / 2) {
    const std::vector<double> p = Power(x, from, n);
    for (std::size_t k = 0; k < acc.size(); ++k) acc[k] += p[k];
    ++segs;
  }
  for (double& a : acc) a /= double(segs);
  return acc;
}
// Mean density (dB) over [lo, hi) Hz.
double BandDb(const std::vector<double>& p, double lo, double hi) {
  const double binHz = kRate / 4096.0;
  double s = 0;
  std::size_t c = 0;
  for (std::size_t k = static_cast<std::size_t>(lo / binHz); k < static_cast<std::size_t>(hi / binHz); ++k) {
    s += p[k];
    ++c;
  }
  return 10.0 * std::log10(s / double(c));
}
// Least-squares slope (dB per octave) of the octave bands centred 250 Hz .. 8 kHz.
double SlopeDbPerOctave(const std::vector<double>& p) {
  std::vector<double> xs, ys;
  for (double fc = 250.0; fc <= 8000.0; fc *= 2.0) {
    xs.push_back(std::log2(fc));
    ys.push_back(BandDb(p, fc / std::sqrt(2.0), fc * std::sqrt(2.0)));
  }
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

// Same seed -> the same bits whatever the block size; another seed -> another noise.
SND_TEST(SourceNoise, SameSeedIsBitIdenticalForAnyBlocking) {
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto a = Render(color, 7, 99, 6000, 2, 256);
    const auto b = Render(color, 7, 99, 6000, 2, 61);
    const auto c = Render(color, 7, 99, 6000, 2, 1024);
    SND_REQUIRE(a.size() == 2u && b.size() == 2u && c.size() == 2u);
    SND_CHECK_MSG(a == b && a == c, "colour %u", color);
    // At pitch 0.7 too (fixed-point source position).
    Driver::Mod m;
    m.pitch = snd::Ramp{0.7f, 0.7f};
    const auto d = Render(color, 7, 99, 6000, 1, 256, m);
    const auto e = Render(color, 7, 99, 6000, 1, 100, m);
    SND_CHECK_MSG(d == e, "colour %u pitch 0.7", color);
    const auto f = Render(color, 8, 99, 6000, 1);
    const auto g = Render(color, 7, 98, 6000, 1);
    SND_CHECK(f[0] != a[0]);
    SND_CHECK(g[0] != a[0]);
  }
  // The white sequence is the counter-based one of the header.
  const auto w = Render(0, 7, 99, 100, 1);
  const std::uint64_t key = src::detail::NoiseKind::ChannelKey(src::detail::NoiseKind::VoiceKey(7, 99), 0);
  for (std::size_t i = 0; i < 100; ++i) SND_CHECK_EQ(w[0][i], src::detail::NoiseKind::White(key, i));
}

// Spectral slope 0 / -3 / -6 dB per octave (250 Hz .. 8 kHz) and RMS 1/sqrt(3).
SND_TEST(SourceNoise, SpectralSlopeAndLevelPerColour) {
  const double want[3] = {0.0, -3.0, -6.0};
  const double tol[3] = {0.3, 0.6, 0.8};
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto x = Render(color, 1, 2, 1u << 19, 1, 512);
    SND_REQUIRE(!x.empty());
    const double slope = SlopeDbPerOctave(Density(x[0]));
    const double rms = Rms(x[0], 48000);
    std::printf("noise colour %u: slope %.2f dB/oct, rms %.3f\n", color, slope, rms);
    SND_CHECK_MSG(std::fabs(slope - want[color]) < tol[color], "colour %u slope %f", color, slope);
    SND_CHECK_MSG(std::fabs(rms - 0.57735) < 0.06, "colour %u rms %f", color, rms);
  }
}

// Every channel has its own sequence (D-193): uncorrelated, same colour and level.
SND_TEST(SourceNoise, ChannelsAreUncorrelated) {
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto x = Render(color, 3, 4, 200000, 4);
    SND_REQUIRE(x.size() == 4u);
    for (std::uint32_t a = 0; a < 4; ++a) {
      for (std::uint32_t b = a + 1; b < 4; ++b) {
        double sab = 0, saa = 0, sbb = 0;
        for (std::size_t i = 0; i < x[a].size(); ++i) {
          sab += double(x[a][i]) * x[b][i];
          saa += double(x[a][i]) * x[a][i];
          sbb += double(x[b][i]) * x[b][i];
        }
        const double r = sab / std::sqrt(saa * sbb);
        // Brown noise has few independent samples (time constant 200): a looser bound.
        SND_CHECK_MSG(std::fabs(r) < (color == 2u ? 0.1 : 0.02), "colour %u ch %u-%u r %f", color, a, b, r);
      }
    }
  }
}

// Pitch: at ratio 0.25 the white noise is the source sequence interpolated (darker);
// the source position advances by the ratio.
SND_TEST(SourceNoise, PitchStretchesTheSequence) {
  Driver::Mod m;
  m.pitch = snd::Ramp{0.25f, 0.25f};
  const auto x = Render(0, 5, 6, 4000, 1, 256, m);
  const std::uint64_t key = src::detail::NoiseKind::ChannelKey(src::detail::NoiseKind::VoiceKey(5, 6), 0);
  for (std::size_t i = 0; i < 4000; ++i) {
    const std::size_t k = i / 4;
    const float y0 = src::detail::NoiseKind::White(key, k), y1 = src::detail::NoiseKind::White(key, k + 1);
    const float want = y0 + (y1 - y0) * (static_cast<float>((i % 4) * 0x40000000u) * (1.0f / 4294967296.0f));
    SND_CHECK_NEAR(x[0][i], want, 1e-6f);
  }
  // Darker: little power above 6 kHz compared with pitch 1.
  const auto lo = Render(0, 5, 6, 1u << 16, 1, 256, m);
  const auto hi = Render(0, 5, 6, 1u << 16, 1);
  const auto pl = Density(lo[0]), ph = Density(hi[0]);
  SND_CHECK(BandDb(pl, 8000.0, 16000.0) < BandDb(ph, 8000.0, 16000.0) - 15.0);
}

// Virtual advance: white stays exactly on its sequence; pink / brown rebuild the
// filter state from the recent samples (tiny difference). Seek moves to a sample.
SND_TEST(SourceNoise, VirtualAdvanceAndSeekFollowTheSequence) {
  for (std::uint32_t color : {0u, 1u, 2u}) {
    const auto ref = Render(color, 11, 12, 40000);
    Driver d(snd::SourceKind::Noise, Driver::Opt{});
    SND_REQUIRE(d.Ok());
    src::NoiseParams p{color, 11};
    Driver::Voice v;
    SND_REQUIRE(d.Start(v, &p, 1, 12));
    d.Render(v, 2560);
    d.Advance(v, 10240);
    const std::size_t at = v.out[0].size();
    d.Render(v, 2560);
    double err = 0;
    for (std::size_t i = 0; i < 2560; ++i) err = std::fmax(err, std::fabs(v.out[0][at + i] - ref[0][12800 + i]));
    std::printf("noise colour %u: after virtual advance max error %g\n", color, err);
    SND_CHECK_MSG(color == 0u ? err == 0.0 : err < 2e-3, "colour %u advance %g", color, err);
    d.Control(v, src::ControlOp::Seek, 30000);
    const std::size_t at2 = v.out[0].size();
    d.Render(v, 2000);
    err = 0;
    for (std::size_t i = 0; i < 2000; ++i) err = std::fmax(err, std::fabs(v.out[0][at2 + i] - ref[0][30000 + i]));
    SND_CHECK_MSG(color == 0u ? err == 0.0 : err < 2e-3, "colour %u seek %g", color, err);
    SND_CHECK_EQ(v.last.ended, 0u);
  }
  // A seek before the warm-up window is exact for every colour (rebuilt from sample 0).
  const auto ref = Render(1, 1, 1, 3000);
  Driver d(snd::SourceKind::Noise, Driver::Opt{});
  src::NoiseParams p{1, 1};
  Driver::Voice v;
  SND_REQUIRE(d.Start(v, &p, 1, 1));
  d.Render(v, 2000);
  d.Control(v, src::ControlOp::Seek, 500);
  d.Render(v, 1000);
  for (std::size_t i = 0; i < 1000; ++i) SND_CHECK_EQ(v.out[0][2000 + i], ref[0][500 + i]);
}

SND_TEST(SourceNoise, BadStartsAreRefused) {
  Driver d(snd::SourceKind::Noise, Driver::Opt{});
  SND_REQUIRE(d.Ok());
  Driver::Voice v;
  src::NoiseParams p{3, 0};
  SND_CHECK(!d.Start(v, &p, 1));
  SND_CHECK(!d.Start(v, nullptr, 1));
}
