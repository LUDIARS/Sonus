// Real FFT (fft.hpp, D-208): analytic spectra of known signals, round trip,
// Parseval, a direct DFT in double, and RT safety.
#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/fft.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

struct Fft {
  std::uint32_t n;
  dsptest::AlignedMem plan;
  std::vector<float> work, re, im, out;
  explicit Fft(std::uint32_t size)
      : n(size), plan(FftPlanBytes(size), dsptest::kDirty), work(FftWorkFloats(size)), re(size / 2),
        im(size / 2), out(size) {
    ok = FftPlanInit(plan.get(), plan.size(), size);
  }
  bool ok = false;
  void Forward(const float* x) {
    SND_RT_SCOPE();
    FftRealForward(plan.get(), x, re.data(), im.data(), work.data());
  }
  void Inverse() {
    SND_RT_SCOPE();
    FftRealInverse(plan.get(), re.data(), im.data(), out.data(), work.data());
  }
};

std::vector<float> NoiseVec(std::uint32_t n, std::uint32_t seed) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (float& x : v) x = nz.Next(1.0f);
  return v;
}

}  // namespace

SND_TEST(Fft, PlanSizes) {
  for (std::uint32_t n : {0u, 1u, 2u, 3u, 6u, 12u, 1000u, kFftMaxSize * 2u}) {
    SND_CHECK_MSG(FftPlanBytes(n) == 0u, "size %u accepted", n);
    SND_CHECK(!FftSizeValid(n));
  }
  for (std::uint32_t n = kFftMinSize; n <= kFftMaxSize; n *= 2u) {
    SND_CHECK(FftSizeValid(n));
    const std::size_t b = FftPlanBytes(n);
    SND_CHECK(b > 0u && b % 64u == 0u);
    dsptest::AlignedMem m(b, dsptest::kDirty);
    SND_CHECK(!FftPlanInit(m.get(), b - 1u, n));
    SND_CHECK(FftPlanInit(m.get(), b, n));
    SND_CHECK_EQ(FftPlanSize(m.get()), n);
  }
}

// Impulse, DC, cosine / sine at bin k, Nyquist: exact analytic spectra.
SND_TEST(Fft, KnownSignals) {
  for (std::uint32_t n = kFftMinSize; n <= 65536u; n *= 2u) {
    Fft f(n);
    SND_REQUIRE(f.ok);
    const std::uint32_t m = n / 2u;
    const double tol = 2e-6 * n * std::log2(static_cast<double>(n));
    std::vector<float> x(n, 0.0f);
    // Impulse at 0: X[k] = 1.
    x[0] = 1.0f;
    f.Forward(x.data());
    double err = 0.0;
    for (std::uint32_t k = 0; k < m; ++k) {
      err = std::max(err, std::fabs(f.re[k] - 1.0) + std::fabs(f.im[k] - (k == 0u ? 1.0 : 0.0)));
    }
    SND_CHECK_MSG(err < 1e-6, "n %u impulse err %g", n, err);
    // DC: X[0] = n, rest 0.
    for (float& v : x) v = 1.0f;
    f.Forward(x.data());
    err = std::fabs(f.re[0] - n) + std::fabs(f.im[0]);
    for (std::uint32_t k = 1; k < m; ++k) err = std::max(err, static_cast<double>(std::fabs(f.re[k]) + std::fabs(f.im[k])));
    SND_CHECK_MSG(err < tol, "n %u DC err %g", n, err);
    // Nyquist (+1, -1, ...): X[m] = n.
    for (std::uint32_t j = 0; j < n; ++j) x[j] = (j & 1u) ? -1.0f : 1.0f;
    f.Forward(x.data());
    err = std::fabs(f.re[0]) + std::fabs(f.im[0] - n);
    for (std::uint32_t k = 1; k < m; ++k) err = std::max(err, static_cast<double>(std::fabs(f.re[k]) + std::fabs(f.im[k])));
    SND_CHECK_MSG(err < tol, "n %u Nyquist err %g", n, err);
    // cos(2 pi k j / n) -> re[k] = n / 2; sin -> im[k] = -n / 2.
    for (std::uint32_t k : {1u, m / 2u, m - 1u}) {
      if (k == 0u || k >= m) continue;
      for (std::uint32_t j = 0; j < n; ++j) {
        const double a = 2.0 * dsptest::kPi * static_cast<double>((std::uint64_t{k} * j) % n) / n;
        x[j] = static_cast<float>(std::cos(a) + 0.5 * std::sin(a));
      }
      f.Forward(x.data());
      double e2 = 0.0;
      for (std::uint32_t q = 0; q < m; ++q) {
        const double wr = q == k ? n / 2.0 : 0.0;
        const double wi = q == k ? -static_cast<double>(n) / 4.0 : 0.0;
        e2 = std::max(e2, std::fabs(f.re[q] - wr) + std::fabs(f.im[q] - wi));
      }
      SND_CHECK_MSG(e2 < tol, "n %u bin %u err %g (tol %g)", n, k, e2, tol);
    }
  }
}

// Inverse(Forward(x)) = n x; Parseval: sum x^2 = (X0^2 + Xm^2 + 2 sum |Xk|^2) / n.
SND_TEST(Fft, RoundTripAndParseval) {
  for (std::uint32_t n = kFftMinSize; n <= kFftMaxSize; n *= 2u) {
    Fft f(n);
    SND_REQUIRE(f.ok);
    const std::vector<float> x = NoiseVec(n, n + 3u);
    f.Forward(x.data());
    double ex = 0.0, ef = 0.0;
    for (float v : x) ex += static_cast<double>(v) * v;
    ef = static_cast<double>(f.re[0]) * f.re[0] + static_cast<double>(f.im[0]) * f.im[0];
    for (std::uint32_t k = 1; k < n / 2u; ++k)
      ef += 2.0 * (static_cast<double>(f.re[k]) * f.re[k] + static_cast<double>(f.im[k]) * f.im[k]);
    ef /= n;
    SND_CHECK_MSG(std::fabs(ef - ex) <= 1e-5 * ex, "n %u Parseval %g vs %g", n, ef, ex);
    f.Inverse();
    double maxErr = 0.0, rms = std::sqrt(ex / n);
    for (std::uint32_t j = 0; j < n; ++j)
      maxErr = std::max(maxErr, std::fabs(static_cast<double>(f.out[j]) / n - x[j]));
    const double tol = 2e-7 * std::log2(static_cast<double>(n)) * 4.0 * rms;
    SND_CHECK_MSG(maxErr < tol, "n %u round trip err %g (tol %g)", n, maxErr, tol);
    if (n == 512u || n == 8192u) snd::test::Note("n %u: round trip max err %.3g (signal rms %.3g)", n, maxErr, rms);
  }
}

// Against a direct DFT in double.
SND_TEST(Fft, MatchesDirectDft) {
  for (std::uint32_t n : {8u, 64u, 512u, 2048u}) {
    Fft f(n);
    SND_REQUIRE(f.ok);
    const std::vector<float> x = NoiseVec(n, 99u);
    f.Forward(x.data());
    double maxErr = 0.0;
    for (std::uint32_t k = 0; k <= n / 2u; ++k) {
      double r = 0.0, i = 0.0;
      for (std::uint32_t j = 0; j < n; ++j) {
        const double a = -2.0 * dsptest::kPi * static_cast<double>((std::uint64_t{k} * j) % n) / n;
        r += x[j] * std::cos(a);
        i += x[j] * std::sin(a);
      }
      double gr, gi;
      if (k == 0u) {
        gr = f.re[0];
        gi = 0.0;
      } else if (k == n / 2u) {
        gr = f.im[0];
        gi = 0.0;
      } else {
        gr = f.re[k];
        gi = f.im[k];
      }
      maxErr = std::max(maxErr, std::fabs(gr - r) + std::fabs(gi - i));
    }
    SND_CHECK_MSG(maxErr < 3e-6 * n, "n %u vs DFT %g", n, maxErr);
  }
}
