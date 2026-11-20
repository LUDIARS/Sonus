// Helpers of the independent tests of the stage 7-D source kinds, group 1
// (indep_osc_test, indep_noise_test, indep_input_test, indep_src1_core_test,
// indep_defects_src1_test). Expected values are computed here from the
// specification (CONTRACTS.md D-190 .. D-195), never with the kinds' own code.
// The kinds are driven with synth_driver.hpp (RT scope around every call).
#ifndef SND_SOURCE_TEST_INDEP_SRC1_UTIL_HPP
#define SND_SOURCE_TEST_INDEP_SRC1_UTIL_HPP

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace isrc {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

// Complex Fourier coefficient (peak amplitude, rectangular window) of x[from, from + n)
// at `cycles` cycles per n samples. Exact for a signal periodic in n.
inline std::complex<double> Coef(const std::vector<float>& x, std::size_t from, std::size_t n, double cycles) {
  double re = 0.0, im = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double a = kTwoPi * cycles * static_cast<double>(i) / static_cast<double>(n);
    re += static_cast<double>(x[from + i]) * std::cos(a);
    im -= static_cast<double>(x[from + i]) * std::sin(a);
  }
  return std::complex<double>(2.0 * re / static_cast<double>(n), 2.0 * im / static_cast<double>(n));
}

inline double Mean(const std::vector<float>& x, std::size_t from = 0, std::size_t n = 0) {
  if (n == 0) n = x.size() - from;
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) s += x[from + i];
  return s / static_cast<double>(n);
}
inline double RmsOf(const std::vector<float>& x, std::size_t from = 0, std::size_t n = 0) {
  if (n == 0) n = x.size() - from;
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[from + i]) * x[from + i];
  return std::sqrt(s / static_cast<double>(n));
}
inline double MaxAbs(const std::vector<float>& x) {
  double m = 0.0;
  for (float v : x) m = std::fmax(m, std::fabs(static_cast<double>(v)));
  return m;
}
inline bool AllFinite(const std::vector<float>& x) {
  for (float v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

// Iterative radix-2 FFT (own copy).
inline void Fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0u; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    std::vector<std::complex<double>> tw(len / 2);
    for (std::size_t k = 0; k < len / 2; ++k) {
      const double ang = -kTwoPi * static_cast<double>(k) / static_cast<double>(len);
      tw[k] = std::complex<double>(std::cos(ang), std::sin(ang));
    }
    for (std::size_t i = 0; i < n; i += len) {
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * tw[k];
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
      }
    }
  }
}

// Welch power density (Hann, segments of `seg`, half overlap). Bins 0 .. seg/2.
inline std::vector<double> Welch(const std::vector<float>& x, std::size_t seg) {
  std::vector<double> acc(seg / 2 + 1, 0.0);
  std::vector<double> win(seg);
  for (std::size_t i = 0; i < seg; ++i) win[i] = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(seg));
  std::size_t segs = 0;
  std::vector<std::complex<double>> a(seg);
  for (std::size_t from = 0; from + seg <= x.size(); from += seg / 2) {
    for (std::size_t i = 0; i < seg; ++i) a[i] = std::complex<double>(x[from + i] * win[i], 0.0);
    Fft(a);
    for (std::size_t k = 0; k < acc.size(); ++k) acc[k] += std::norm(a[k]);
    ++segs;
  }
  for (double& v : acc) v /= static_cast<double>(segs);
  return acc;
}

// Blackman-Harris power spectrum of x[from, from + n) (n a power of two).
inline std::vector<double> PowerBh(const std::vector<float>& x, std::size_t from, std::size_t n) {
  std::vector<std::complex<double>> a(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = kTwoPi * static_cast<double>(i) / static_cast<double>(n);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    a[i] = std::complex<double>(static_cast<double>(x[from + i]) * w, 0.0);
  }
  Fft(a);
  std::vector<double> p(n / 2 + 1);
  for (std::size_t k = 0; k <= n / 2; ++k) p[k] = std::norm(a[k]);
  return p;
}

// Power (dB relative to the total above DC) of everything that is not within +-guard
// bins of a harmonic k f0 < rate / 2: the aliases folded back below Nyquist.
inline double NonHarmonicDb(const std::vector<double>& p, double f0, double rate, std::size_t n, std::size_t guard = 8) {
  std::vector<char> harm(p.size(), 0);
  for (double f = f0; f < rate / 2.0; f += f0) {
    const long c = std::lround(f * static_cast<double>(n) / rate);
    for (long k = c - static_cast<long>(guard); k <= c + static_cast<long>(guard); ++k) {
      if (k >= 0 && static_cast<std::size_t>(k) < p.size()) harm[static_cast<std::size_t>(k)] = 1;
    }
  }
  double alias = 0.0, total = 0.0;
  for (std::size_t k = 4; k < p.size(); ++k) {
    total += p[k];
    if (harm[k] == 0) alias += p[k];
  }
  return 10.0 * std::log10(alias / total + 1e-300);
}

// Naive (not band-limited) waveforms at peak 1, phase in cycles.
inline double NaiveSaw(double ph) { return 2.0 * ph - 1.0; }
inline double NaiveSquare(double ph, double duty = 0.5) { return ph < duty ? 1.0 : -1.0; }
inline double NaiveTriangle(double ph) { return ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph; }

// splitmix64 output function (the finaliser), as named in D-193.
inline std::uint64_t SplitMix64(std::uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
inline constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;

// D-193 white sequence, written from the text of the decision.
inline std::uint64_t SpecVoiceKey(std::uint32_t bankSeed, std::uint32_t startSeed) {
  return SplitMix64((static_cast<std::uint64_t>(bankSeed) << 32) | startSeed);
}
inline std::uint64_t SpecChannelKey(std::uint64_t key, std::uint32_t c) {
  return SplitMix64(key ^ ((static_cast<std::uint64_t>(c) + 1u) * kGolden));
}
inline double SpecWhite(std::uint64_t channelKey, std::uint64_t k) {
  const std::uint64_t z = SplitMix64(channelKey + (k + 1u) * kGolden);
  const double top24 = static_cast<double>(z >> 40);  // 0 .. 2^24 - 1
  return top24 / 8388608.0 - 1.0;                     // [-1, 1)
}

// Correlation coefficient of two equally long sequences.
inline double Corr(const std::vector<float>& a, const std::vector<float>& b) {
  const double ma = Mean(a), mb = Mean(b);
  double sab = 0, saa = 0, sbb = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double x = a[i] - ma, y = b[i] - mb;
    sab += x * y;
    saa += x * x;
    sbb += y * y;
  }
  return sab / std::sqrt(saa * sbb);
}

}  // namespace isrc

#endif  // SND_SOURCE_TEST_INDEP_SRC1_UTIL_HPP
