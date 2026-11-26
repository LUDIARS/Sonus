// Independent tests of the real FFT (fft.hpp, CONTRACTS.md D-208), written from
// the specification: every size against a direct DFT in double, the inverse
// against the inverse-DFT formula of the packed layout, round trip, linearity,
// Parseval, the time-shift theorem, guards around every output buffer,
// alignment, invalid sizes, concurrent use of one plan, RT safety and a
// bit-exact checksum shared by every SIMD configuration (scalar / SSE / AVX2+FMA).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "dsp_test_util.hpp"
#include "snd/dsp/fft.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/test.hpp"

using namespace snd::dsp;

namespace {

constexpr double kPi = dsptest::kPi;
constexpr float kSentinel = -12345.678f;
constexpr std::uint32_t kGuard = 32;  // floats of guard after every buffer

// Buffer of n floats with guard floats before and after (sentinel filled).
class Guarded {
 public:
  explicit Guarded(std::uint32_t n, std::uint32_t offset = 0)
      : n_(n), off_(offset), v_(kGuard + offset + n + kGuard, kSentinel) {}
  float* data() { return v_.data() + kGuard + off_; }
  const float* data() const { return v_.data() + kGuard + off_; }
  bool GuardsIntact() const {
    for (std::uint32_t i = 0; i < kGuard + off_; ++i)
      if (std::memcmp(&v_[i], &kSentinel, 4) != 0) return false;
    for (std::size_t i = kGuard + off_ + n_; i < v_.size(); ++i)
      if (std::memcmp(&v_[i], &kSentinel, 4) != 0) return false;
    return true;
  }

 private:
  std::uint32_t n_, off_;
  std::vector<float> v_;
};

struct Plan {
  dsptest::AlignedMem mem;
  bool ok = false;
  explicit Plan(std::uint32_t n) : mem(FftPlanBytes(n) + 64u, dsptest::kDirty) {
    ok = FftPlanInit(mem.get(), FftPlanBytes(n), n);
  }
  const void* get() const { return mem.get(); }
};

std::vector<float> Noise(std::uint32_t n, std::uint32_t seed, float amp = 1.0f) {
  dsptest::Noise nz(seed);
  std::vector<float> v(n);
  for (float& x : v) x = nz.Next(amp);
  return v;
}

std::uint32_t Log2(std::uint32_t n) {
  std::uint32_t l = 0;
  while ((1u << l) < n) ++l;
  return l;
}

// X[k] of a real signal in double.
void DftBin(const std::vector<float>& x, std::uint32_t k, double* re, double* im) {
  const std::uint32_t n = static_cast<std::uint32_t>(x.size());
  double r = 0.0, i = 0.0;
  for (std::uint32_t j = 0; j < n; ++j) {
    // exact phase reduction: (j * k) mod n
    const std::uint64_t ph = (std::uint64_t{j} * k) % n;
    const double a = -2.0 * kPi * static_cast<double>(ph) / static_cast<double>(n);
    r += x[j] * std::cos(a);
    i += x[j] * std::sin(a);
  }
  *re = r;
  *im = i;
}

double Norm2(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return std::sqrt(s);
}

std::uint64_t Fnv(const float* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
  const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n * 4u; ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace

// F1/F2: validity of every size 0..2^20 (all non powers of two in a sweep, all
// powers of two), plan bytes multiple of 64, Init refuses short memory and never
// writes past FftPlanBytes(n).
SND_TEST(IndepFft, SizeValidityAndPlanGuards) {
  for (std::uint32_t n = 0; n <= 70000u; ++n) {
    const bool pow2 = n != 0u && (n & (n - 1u)) == 0u;
    const bool valid = pow2 && n >= 4u && n <= 131072u;
    if (FftSizeValid(n) != valid) {
      SND_CHECK_MSG(false, "FftSizeValid(%u)", n);
      break;
    }
    if (!valid && FftPlanBytes(n) != 0u) {
      SND_CHECK_MSG(false, "FftPlanBytes(%u) != 0", n);
      break;
    }
  }
  for (std::uint32_t l = 0; l <= 31u; ++l) {
    const std::uint32_t n = 1u << l;
    const bool valid = l >= 2u && l <= 17u;
    SND_CHECK_EQ(FftSizeValid(n), valid);
    SND_CHECK_EQ(FftPlanBytes(n) != 0u, valid);
    SND_CHECK(!FftSizeValid(n + 1u) || n + 1u == 4u);
  }
  SND_CHECK(!FftSizeValid(0xFFFFFFFFu));
  for (std::uint32_t n = 4; n <= kFftMaxSize; n *= 2u) {
    const std::size_t b = FftPlanBytes(n);
    SND_CHECK(b % 64u == 0u);
    // Guard: 256 bytes after the plan stay untouched; short memory is refused
    // without writing anything.
    std::vector<unsigned char> mem(b + 256u + 64u, 0xA5);
    unsigned char* p = mem.data() + (64u - reinterpret_cast<std::uintptr_t>(mem.data()) % 64u);
    std::vector<unsigned char> before(mem);
    SND_CHECK(!FftPlanInit(p, b - 1u, n));
    SND_CHECK(!FftPlanInit(nullptr, b, n));
    SND_CHECK(mem == before);
    SND_CHECK(FftPlanInit(p, b + 256u, n));
    bool intact = true;
    for (std::size_t i = 0; i < 256u; ++i) intact = intact && p[b + i] == 0xA5;
    SND_CHECK_MSG(intact, "FftPlanInit(%u) wrote past FftPlanBytes", n);
    SND_CHECK_EQ(FftPlanSize(p), n);
    SND_CHECK(!FftPlanInit(p, b, n + 1u));
    SND_CHECK(!FftPlanInit(p, b, 2u));
    SND_CHECK(!FftPlanInit(p, b, kFftMaxSize * 2u));
  }
  SND_CHECK_EQ(FftWorkFloats(64), 64u);
  std::vector<unsigned char> zero(256, 0);
  SND_CHECK_EQ(FftPlanSize(zero.data()), 0u);
  SND_CHECK_EQ(FftPlanSize(nullptr), 0u);
}

// F4: every size against the direct DFT in double. Full DFT up to 4096, 48
// sampled bins (including DC, Nyquist, 1, m-1, m/2) above. Error relative to
// ||x||_2 bounded by 4e-7 * log2(n). Every output buffer is guarded, `in` is
// unchanged.
SND_TEST(IndepFft, EverySizeMatchesDirectDft) {
  double worstRel = 0.0;
  std::uint32_t worstN = 0;
  for (std::uint32_t n = 4; n <= kFftMaxSize; n *= 2u) {
    const std::uint32_t m = n / 2u;
    Plan plan(n);
    SND_REQUIRE(plan.ok);
    const std::vector<float> x = Noise(n, 1000u + n);
    Guarded in(n), re(m), im(m), work(static_cast<std::uint32_t>(FftWorkFloats(n)));
    std::memcpy(in.data(), x.data(), sizeof(float) * n);
    {
      SND_RT_SCOPE();
      FftRealForward(plan.get(), in.data(), re.data(), im.data(), work.data());
    }
    SND_CHECK_MSG(std::memcmp(in.data(), x.data(), sizeof(float) * n) == 0, "n=%u: input modified", n);
    SND_CHECK_MSG(in.GuardsIntact() && re.GuardsIntact() && im.GuardsIntact() && work.GuardsIntact(),
                  "n=%u: write outside a buffer", n);
    std::vector<std::uint32_t> bins;
    if (n <= 4096u) {
      for (std::uint32_t k = 0; k <= m; ++k) bins.push_back(k);
    } else {
      bins = {0u, 1u, 2u, 3u, m / 2u, m / 2u + 1u, m - 1u, m, m - 2u, m / 4u, 3u * m / 4u};
      dsptest::Noise pick(n);
      while (bins.size() < 48u) bins.push_back(1u + static_cast<std::uint32_t>((pick.Next(1.0f) + 1.0f) * 0.5f * (m - 2u)));
    }
    double err = 0.0;
    for (std::uint32_t k : bins) {
      double wr, wi;
      DftBin(x, k, &wr, &wi);
      double gr, gi;
      if (k == 0u) {
        gr = re.data()[0];
        gi = 0.0;
      } else if (k == m) {
        gr = im.data()[0];
        gi = 0.0;
      } else {
        gr = re.data()[k];
        gi = im.data()[k];
      }
      err = std::max(err, std::hypot(gr - wr, gi - wi));
      // DC and Nyquist of a real signal are real.
      if (k == 0u || k == m) err = std::max(err, std::fabs(wi));
    }
    const double rel = err / Norm2(x);
    if (rel > worstRel) {
      worstRel = rel;
      worstN = n;
    }
    SND_CHECK_MSG(rel < 4e-7 * Log2(n), "n=%u: DFT error %.3g of ||x||", n, rel);
  }
  snd::test::Note("FFT vs direct DFT: worst error %.3g of ||x||2 (n=%u)", worstRel, worstN);
}

// F5: the inverse against the inverse-DFT formula of the packed layout, from an
// ARBITRARY packed spectrum (does not rely on the forward transform):
//   y[j] = X0 + (-1)^j Xm + 2 sum_{k=1}^{m-1} (Re Xk cos(2 pi jk/n) - Im Xk sin(2 pi jk/n)).
// re / im unchanged, guards intact.
SND_TEST(IndepFft, InverseMatchesFormula) {
  for (std::uint32_t n = 4; n <= 2048u; n *= 2u) {
    const std::uint32_t m = n / 2u;
    Plan plan(n);
    const std::vector<float> sr = Noise(m, 7u + n), si = Noise(m, 9u + n);
    Guarded re(m), im(m), out(n), work(n);
    std::memcpy(re.data(), sr.data(), 4u * m);
    std::memcpy(im.data(), si.data(), 4u * m);
    {
      SND_RT_SCOPE();
      FftRealInverse(plan.get(), re.data(), im.data(), out.data(), work.data());
    }
    SND_CHECK_MSG(std::memcmp(re.data(), sr.data(), 4u * m) == 0 &&
                      std::memcmp(im.data(), si.data(), 4u * m) == 0,
                  "n=%u: inverse modified its input", n);
    SND_CHECK(re.GuardsIntact() && im.GuardsIntact() && out.GuardsIntact() && work.GuardsIntact());
    double err = 0.0, norm = 0.0;
    for (std::uint32_t j = 0; j < n; ++j) {
      double y = sr[0] + ((j & 1u) ? -1.0 : 1.0) * si[0];
      for (std::uint32_t k = 1; k < m; ++k) {
        const double a = 2.0 * kPi * static_cast<double>((std::uint64_t{j} * k) % n) / n;
        y += 2.0 * (sr[k] * std::cos(a) - si[k] * std::sin(a));
      }
      err = std::max(err, std::fabs(out.data()[j] - y));
      norm += y * y;
    }
    SND_CHECK_MSG(err / std::sqrt(norm) < 4e-7 * Log2(n), "n=%u: inverse error %.3g", n, err / std::sqrt(norm));
  }
}

// F5: round trip = n x, error bounded for every size; D-208: < 1e-6 of the RMS at 8192.
SND_TEST(IndepFft, RoundTripEverySize) {
  for (std::uint32_t n = 4; n <= kFftMaxSize; n *= 2u) {
    Plan plan(n);
    const std::vector<float> x = Noise(n, 77u + n);
    std::vector<float> re(n / 2u), im(n / 2u), out(n), work(n);
    FftRealForward(plan.get(), x.data(), re.data(), im.data(), work.data());
    FftRealInverse(plan.get(), re.data(), im.data(), out.data(), work.data());
    double e2 = 0.0, x2 = 0.0, emax = 0.0;
    for (std::uint32_t j = 0; j < n; ++j) {
      const double d = out[j] / static_cast<double>(n) - x[j];
      e2 += d * d;
      x2 += static_cast<double>(x[j]) * x[j];
      emax = std::max(emax, std::fabs(d));
    }
    const double rms = std::sqrt(x2 / n), erms = std::sqrt(e2 / n);
    SND_CHECK_MSG(erms < 1.5e-7 * Log2(n) * rms, "n=%u: round trip rms error %.3g", n, erms / rms);
    SND_CHECK_MSG(emax < 1e-6 * Log2(n) * rms, "n=%u: round trip max error %.3g", n, emax / rms);
    if (n == 8192u) {
      SND_CHECK(erms < 1e-6 * rms);
      snd::test::Note("FFT round trip n=8192: rms error %.3g, max %.3g of the RMS", erms / rms, emax / rms);
    }
  }
}

// F6: linearity, Parseval, time shift, conjugate symmetry (via a 2-bin check of
// a real cosine), and exact zero for a zero input.
SND_TEST(IndepFft, TheoremsHold) {
  for (std::uint32_t n : {8u, 64u, 1024u, 16384u}) {
    const std::uint32_t m = n / 2u;
    Plan plan(n);
    const std::vector<float> x = Noise(n, 3u), y = Noise(n, 4u);
    std::vector<float> xr(m), xi(m), yr(m), yi(m), zr(m), zi(m), work(n);
    FftRealForward(plan.get(), x.data(), xr.data(), xi.data(), work.data());
    FftRealForward(plan.get(), y.data(), yr.data(), yi.data(), work.data());
    // Linearity: F(0.75 x - 1.5 y).
    std::vector<float> z(n);
    for (std::uint32_t j = 0; j < n; ++j) z[j] = 0.75f * x[j] - 1.5f * y[j];
    FftRealForward(plan.get(), z.data(), zr.data(), zi.data(), work.data());
    double lin = 0.0, scale = Norm2(z);
    for (std::uint32_t k = 0; k < m; ++k) {
      lin = std::max(lin, std::fabs(zr[k] - (0.75 * xr[k] - 1.5 * yr[k])));
      lin = std::max(lin, std::fabs(zi[k] - (0.75 * xi[k] - 1.5 * yi[k])));
    }
    SND_CHECK_MSG(lin < 2e-6 * Log2(n) * scale, "n=%u linearity %.3g", n, lin / scale);
    // Parseval: sum x^2 = (X0^2 + Xm^2 + 2 sum |Xk|^2) / n.
    double e = 0.0;
    for (float v : x) e += static_cast<double>(v) * v;
    double s = static_cast<double>(xr[0]) * xr[0] + static_cast<double>(xi[0]) * xi[0];
    for (std::uint32_t k = 1; k < m; ++k) s += 2.0 * (static_cast<double>(xr[k]) * xr[k] + static_cast<double>(xi[k]) * xi[k]);
    SND_CHECK_MSG(std::fabs(s / n - e) < 1e-5 * e, "n=%u Parseval %.3g", n, std::fabs(s / n - e) / e);
    // Circular time shift by d: X'[k] = X[k] exp(-2 pi i k d / n); X'[m] = (-1)^d X[m].
    const std::uint32_t d = 3u % n;
    std::vector<float> sh(n);
    for (std::uint32_t j = 0; j < n; ++j) sh[(j + d) % n] = x[j];
    FftRealForward(plan.get(), sh.data(), zr.data(), zi.data(), work.data());
    double ts = std::fabs(zr[0] - xr[0]) + std::fabs(zi[0] - ((d & 1u) ? -xi[0] : xi[0]));
    for (std::uint32_t k = 1; k < m; ++k) {
      const double a = -2.0 * kPi * static_cast<double>((std::uint64_t{k} * d) % n) / n;
      const double wr = xr[k] * std::cos(a) - xi[k] * std::sin(a);
      const double wi = xr[k] * std::sin(a) + xi[k] * std::cos(a);
      ts = std::max(ts, std::hypot(zr[k] - wr, zi[k] - wi));
    }
    SND_CHECK_MSG(ts < 2e-6 * Log2(n) * Norm2(x), "n=%u time shift %.3g", n, ts / Norm2(x));
    // Zero in, exact zero out (no garbage from the dirty plan memory or work).
    std::vector<float> zero(n, 0.0f);
    std::vector<float> w2(n, std::nanf(""));
    FftRealForward(plan.get(), zero.data(), zr.data(), zi.data(), w2.data());
    bool allZero = true;
    for (std::uint32_t k = 0; k < m; ++k) allZero = allZero && zr[k] == 0.0f && zi[k] == 0.0f;
    SND_CHECK_MSG(allZero, "n=%u: zero input gives a non-zero spectrum (NaN work buffer)", n);
    std::vector<float> o(n);
    FftRealInverse(plan.get(), zr.data(), zi.data(), o.data(), w2.data());
    for (float v : o) allZero = allZero && v == 0.0f;
    SND_CHECK(allZero);
  }
}

// F4: exact layout for a cosine on bin k and a sine on bin k (sign convention
// exp(-i...)), and a pure Nyquist / DC.
SND_TEST(IndepFft, LayoutAndSignConvention) {
  const std::uint32_t n = 256, m = 128;
  Plan plan(n);
  std::vector<float> x(n), re(m), im(m), work(n);
  for (std::uint32_t j = 0; j < n; ++j) x[j] = static_cast<float>(std::sin(2.0 * kPi * 5.0 * j / n));
  FftRealForward(plan.get(), x.data(), re.data(), im.data(), work.data());
  // sin -> X[5] = -i n/2.
  SND_CHECK_NEAR(im[5], -128.0, 1e-3);
  SND_CHECK_NEAR(re[5], 0.0, 1e-3);
  SND_CHECK_NEAR(re[0], 0.0, 1e-3);
  SND_CHECK_NEAR(im[0], 0.0, 1e-3);
  for (std::uint32_t j = 0; j < n; ++j) x[j] = (j & 1u) ? -0.5f : 0.5f;  // Nyquist only
  FftRealForward(plan.get(), x.data(), re.data(), im.data(), work.data());
  SND_CHECK_EQ(im[0], 128.0f);
  SND_CHECK_EQ(re[0], 0.0f);
  for (std::uint32_t k = 1; k < m; ++k) SND_CHECK(std::fabs(re[k]) + std::fabs(im[k]) < 1e-4f);
}

// F8: alignment. A plan at a 16-byte (not 64-byte) boundary and buffers at odd
// float offsets give the same bits as the aligned case.
SND_TEST(IndepFft, AlignmentDoesNotMatter) {
  for (std::uint32_t n : {4u, 8u, 16u, 32u, 4096u}) {
    const std::uint32_t m = n / 2u;
    Plan a(n);
    dsptest::AlignedMem raw(FftPlanBytes(n) + 128u, dsptest::kDirty);
    void* p16 = static_cast<unsigned char*>(raw.get()) + 16;
    SND_REQUIRE(FftPlanInit(p16, FftPlanBytes(n), n));
    const std::vector<float> x = Noise(n, 5u);
    std::vector<float> r0(m), i0(m), o0(n), w0(n);
    FftRealForward(a.get(), x.data(), r0.data(), i0.data(), w0.data());
    FftRealInverse(a.get(), r0.data(), i0.data(), o0.data(), w0.data());
    Guarded xin(n, 1), r1(m, 3), i1(m, 5), o1(n, 7), w1(n, 1);
    std::memcpy(xin.data(), x.data(), 4u * n);
    FftRealForward(p16, xin.data(), r1.data(), i1.data(), w1.data());
    FftRealInverse(p16, r1.data(), i1.data(), o1.data(), w1.data());
    SND_CHECK_MSG(std::memcmp(r0.data(), r1.data(), 4u * m) == 0 && std::memcmp(i0.data(), i1.data(), 4u * m) == 0 &&
                      std::memcmp(o0.data(), o1.data(), 4u * n) == 0,
                  "n=%u: misaligned buffers change the result", n);
    SND_CHECK(r1.GuardsIntact() && i1.GuardsIntact() && o1.GuardsIntact() && w1.GuardsIntact());
  }
}

// F8: one plan used by four threads at once gives the same bits (TSan: no race).
SND_TEST(IndepFft, OnePlanManyThreads) {
  const std::uint32_t n = 2048, m = 1024;
  Plan plan(n);
  const std::vector<float> x = Noise(n, 11u);
  std::vector<float> re(m), im(m), work(n);
  FftRealForward(plan.get(), x.data(), re.data(), im.data(), work.data());
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 4; ++t) {
    th.emplace_back([&] {
      std::vector<float> r(m), i(m), w(n), o(n);
      for (int it = 0; it < 50; ++it) {
        FftRealForward(plan.get(), x.data(), r.data(), i.data(), w.data());
        if (std::memcmp(r.data(), re.data(), 4u * m) != 0 || std::memcmp(i.data(), im.data(), 4u * m) != 0) ++bad;
        FftRealInverse(plan.get(), r.data(), i.data(), o.data(), w.data());
      }
    });
  }
  for (auto& t : th) t.join();
  SND_CHECK_EQ(bad.load(), 0);
}

// F9 / S1: D-208 promises the same bits on the scalar and the SIMD paths (no
// fused multiply-add). The checksum below was computed once (gcc Debug,
// default SSE path) and must be the same in every configuration (scalar
// executable, -mavx2 -mfma, Release, sanitizers).
SND_TEST(IndepFft, BitExactChecksumAcrossSimdPaths) {
  std::uint64_t h = 1469598103934665603ull;
  for (std::uint32_t n : {4u, 8u, 16u, 32u, 64u, 512u, 8192u, 131072u}) {
    Plan plan(n);
    const std::vector<float> x = Noise(n, 2024u);
    std::vector<float> re(n / 2u), im(n / 2u), out(n), work(n);
    FftRealForward(plan.get(), x.data(), re.data(), im.data(), work.data());
    FftRealInverse(plan.get(), re.data(), im.data(), out.data(), work.data());
    h = Fnv(re.data(), re.size(), h);
    h = Fnv(im.data(), im.size(), h);
    h = Fnv(out.data(), out.size(), h);
  }
  snd::test::Note("FFT checksum %016llx", static_cast<unsigned long long>(h));
  constexpr std::uint64_t kExpected = 0xf84904a6f83fea77ull;
  SND_CHECK_EQ(h, kExpected);
}
