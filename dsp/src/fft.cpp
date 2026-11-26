// Real FFT (fft.hpp): radix-2 Stockham complex FFT on split arrays + real packing.
#include "snd/dsp/fft.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"
#include "snd/platform/simd.hpp"

namespace snd::dsp {

namespace {

namespace simd = snd::simd;
using simd::F32x8;

constexpr std::uint32_t kPlanMagic = 0x4E414C50u;  // 'PLAN'

struct PlanHeader {
  std::uint32_t magic;
  std::uint32_t n;       // real size
  std::uint32_t m;       // complex size n / 2
  std::uint32_t stages;  // log2(m)
  std::uint32_t twOff;   // complex twiddles W_m^j, j < m / 2: re[m/2], im[m/2]
  std::uint32_t rtOff;   // real split twiddles W_n^k, k <= m / 2: re[m/2+1], im[m/2+1]
  std::uint32_t pad[10];
};
static_assert(sizeof(PlanHeader) == 64);

std::size_t Layout(std::uint32_t n, std::uint32_t* twOff, std::uint32_t* rtOff) noexcept {
  const std::uint32_t m = n / 2u;
  const std::uint32_t half = m / 2u;
  detail::Carver c(nullptr, sizeof(PlanHeader));
  *twOff = c.Take<float>(2u * half, 64);
  *rtOff = c.Take<float>(2u * (half + 1u), 64);
  return c.Size();
}

const PlanHeader& Header(const void* plan) noexcept { return *static_cast<const PlanHeader*>(plan); }

// One radix-2 Stockham stage: n = current length, s = stride (n * s = m).
void Stage(std::uint32_t half, std::uint32_t s, const float* twr, const float* twi,
           const float* xr, const float* xi, float* yr, float* yi) noexcept {
  if (s >= 8u) {
    for (std::uint32_t p = 0; p < half; ++p) {
      const F32x8 wr = F32x8::Set1(twr[p * s]);
      const F32x8 wi = F32x8::Set1(twi[p * s]);
      const float* ar = xr + s * p;
      const float* ai = xi + s * p;
      const float* br = xr + s * (p + half);
      const float* bi = xi + s * (p + half);
      float* y0r = yr + s * (2u * p);
      float* y0i = yi + s * (2u * p);
      float* y1r = yr + s * (2u * p + 1u);
      float* y1i = yi + s * (2u * p + 1u);
      for (std::uint32_t q = 0; q < s; q += 8u) {
        const F32x8 a0 = F32x8::LoadU(ar + q), a1 = F32x8::LoadU(ai + q);
        const F32x8 b0 = F32x8::LoadU(br + q), b1 = F32x8::LoadU(bi + q);
        (a0 + b0).StoreU(y0r + q);
        (a1 + b1).StoreU(y0i + q);
        const F32x8 dr = a0 - b0, di = a1 - b1;
        (dr * wr - di * wi).StoreU(y1r + q);
        (dr * wi + di * wr).StoreU(y1i + q);
      }
    }
    return;
  }
  for (std::uint32_t p = 0; p < half; ++p) {
    const float wr = twr[p * s];
    const float wi = twi[p * s];
    for (std::uint32_t q = 0; q < s; ++q) {
      const float a0 = xr[q + s * p], a1 = xi[q + s * p];
      const float b0 = xr[q + s * (p + half)], b1 = xi[q + s * (p + half)];
      yr[q + s * (2u * p)] = a0 + b0;
      yi[q + s * (2u * p)] = a1 + b1;
      const float dr = a0 - b0, di = a1 - b1;
      yr[q + s * (2u * p + 1u)] = dr * wr - di * wi;
      yi[q + s * (2u * p + 1u)] = dr * wi + di * wr;
    }
  }
}

// Complex forward FFT of size m. Data starts in buffer A (ar, ai); the result is
// in A when the stage count is even and in B otherwise.
void Complex(const PlanHeader& h, const float* twr, const float* twi, float* ar, float* ai,
             float* br, float* bi) noexcept {
  float* xr = ar;
  float* xi = ai;
  float* yr = br;
  float* yi = bi;
  std::uint32_t n = h.m, s = 1u;
  while (n > 1u) {
    Stage(n / 2u, s, twr, twi, xr, xi, yr, yi);
    float* t = xr;
    xr = yr;
    yr = t;
    t = xi;
    xi = yi;
    yi = t;
    n /= 2u;
    s *= 2u;
  }
}

}  // namespace

bool FftSizeValid(std::uint32_t n) noexcept {
  return n >= kFftMinSize && n <= kFftMaxSize && (n & (n - 1u)) == 0u;
}

std::size_t FftPlanBytes(std::uint32_t n) noexcept {
  if (!FftSizeValid(n)) return 0;
  std::uint32_t a, b;
  return Layout(n, &a, &b);
}

bool FftPlanInit(void* plan, std::size_t bytes, std::uint32_t n) noexcept {
  if (plan == nullptr || !FftSizeValid(n)) return false;
  std::uint32_t twOff, rtOff;
  const std::size_t need = Layout(n, &twOff, &rtOff);
  if (bytes < need) return false;
  std::memset(plan, 0, need);
  PlanHeader& h = *static_cast<PlanHeader*>(plan);
  h.n = n;
  h.m = n / 2u;
  h.stages = 0;
  for (std::uint32_t v = h.m; v > 1u; v /= 2u) ++h.stages;
  h.twOff = twOff;
  h.rtOff = rtOff;
  const std::uint32_t half = h.m / 2u;
  float* twr = detail::At<float>(plan, twOff);
  float* twi = twr + half;
  for (std::uint32_t j = 0; j < half; ++j) {
    const double a = -2.0 * detail::kPi * static_cast<double>(j) / static_cast<double>(h.m);
    twr[j] = static_cast<float>(std::cos(a));
    twi[j] = static_cast<float>(std::sin(a));
  }
  float* rtr = detail::At<float>(plan, rtOff);
  float* rti = rtr + half + 1u;
  for (std::uint32_t k = 0; k <= half; ++k) {
    const double a = -2.0 * detail::kPi * static_cast<double>(k) / static_cast<double>(n);
    rtr[k] = static_cast<float>(std::cos(a));
    rti[k] = static_cast<float>(std::sin(a));
  }
  h.magic = kPlanMagic;
  return true;
}

std::uint32_t FftPlanSize(const void* plan) noexcept {
  if (plan == nullptr) return 0;
  const PlanHeader& h = Header(plan);
  return h.magic == kPlanMagic ? h.n : 0u;
}

void FftRealForward(const void* plan, const float* in, float* re, float* im, float* work) noexcept {
  const PlanHeader& h = Header(plan);
  const std::uint32_t m = h.m, half = m / 2u;
  const float* twr = detail::At<float>(plan, h.twOff);
  const float* twi = twr + half;
  const float* rtr = detail::At<float>(plan, h.rtOff);
  const float* rti = rtr + half + 1u;
  float* wr = work;
  float* wi = work + m;
  // Pack z[j] = x[2j] + i x[2j+1] into the buffer the result must NOT end in.
  const bool even = (h.stages % 2u) == 0u;
  float* pr = even ? re : wr;
  float* pi = even ? im : wi;
  float* qr = even ? wr : re;
  float* qi = even ? wi : im;
  for (std::uint32_t j = 0; j < m; ++j) {
    pr[j] = in[2u * j];
    pi[j] = in[2u * j + 1u];
  }
  Complex(h, twr, twi, pr, pi, qr, qi);  // Z in (re, im)
  // Split: X[k] = E[k] + W_n^k O[k], X[m-k] = conj(E[k] - W_n^k O[k]).
  const float z0r = re[0], z0i = im[0];
  re[0] = z0r + z0i;  // DC
  im[0] = z0r - z0i;  // Nyquist
  for (std::uint32_t k = 1; k <= half; ++k) {
    const std::uint32_t j = m - k;
    const float ar = re[k], ai = im[k], br = re[j], bi = im[j];
    const float er = (ar + br) * 0.5f, ei = (ai - bi) * 0.5f;
    const float orr = (ai + bi) * 0.5f, oi = (br - ar) * 0.5f;
    const float w0 = rtr[k], w1 = rti[k];
    const float tr = w0 * orr - w1 * oi;
    const float ti = w0 * oi + w1 * orr;
    re[k] = er + tr;
    im[k] = ei + ti;
    if (j != k) {
      re[j] = er - tr;
      im[j] = ti - ei;
    }
  }
}

void FftRealInverse(const void* plan, const float* re, const float* im, float* out,
                    float* work) noexcept {
  const PlanHeader& h = Header(plan);
  const std::uint32_t m = h.m, half = m / 2u;
  const float* twr = detail::At<float>(plan, h.twOff);
  const float* twi = twr + half;
  const float* rtr = detail::At<float>(plan, h.rtOff);
  const float* rti = rtr + half + 1u;
  float* wr = work;
  float* wi = work + m;
  float* or_ = out;
  float* oi = out + m;
  // The complex FFT runs on (imag, real) swapped arrays (inverse by swapping);
  // the result must end in the work buffer, then it is interleaved into out.
  const bool even = (h.stages % 2u) == 0u;
  float* zr = even ? wr : or_;  // start buffer (swapped: "re" slot holds imag)
  float* zi = even ? wi : oi;
  float* yr = even ? or_ : wr;
  float* yi = even ? oi : wi;
  // Z'[k] = 2 Z[k]: from X[k] and X[m-k].
  {
    const float x0 = re[0], xm = im[0];
    zi[0] = x0 + xm;  // real part of Z'[0]
    zr[0] = x0 - xm;  // imaginary part of Z'[0]
  }
  for (std::uint32_t k = 1; k <= half; ++k) {
    const std::uint32_t j = m - k;
    const float xr = re[k], xi = im[k], vr = re[j], vi = im[j];
    const float sr = xr + vr, si = xi - vi;  // X[k] + conj X[m-k]
    const float dr = xr - vr, di = xi + vi;  // X[k] - conj X[m-k]
    const float w0 = rtr[k], w1 = rti[k];
    const float o2r = w0 * dr + w1 * di;     // conj(W) * D
    const float o2i = w0 * di - w1 * dr;
    // Z'[k] = S + i O2 ; Z'[m-k] = conj(S) + i conj(O2). Stored swapped.
    zi[k] = sr - o2i;
    zr[k] = si + o2r;
    if (j != k) {
      zi[j] = sr + o2i;
      zr[j] = o2r - si;
    }
  }
  Complex(h, twr, twi, zr, zi, yr, yi);  // result in (wr, wi), swapped back
  for (std::uint32_t j = 0; j < m; ++j) {
    out[2u * j] = wi[j];
    out[2u * j + 1u] = wr[j];
  }
}

}  // namespace snd::dsp
