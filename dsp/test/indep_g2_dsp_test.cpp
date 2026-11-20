// Independent tests of stage 3 group 2 in dsp (no core): the ParamEq band type 8 (allpass,
// D-135) and non-finite / out-of-range parameters through ParamBlock ramps for every
// standard kind on 1, 2 and 6 channels. Also built against the scalar SIMD path.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "snd/dsp/effects.hpp"
#include "snd/test/test.hpp"

namespace {

// 64-byte aligned memory (std::aligned_alloc is not in the MinGW C library).
inline void* AlignedAlloc64(std::size_t bytes) {
  void* raw = std::malloc(bytes + 64u + sizeof(void*));
  if (raw == nullptr) return nullptr;
  const std::uintptr_t a = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*) + 63u) & ~std::uintptr_t{63u};
  reinterpret_cast<void**>(a)[-1] = raw;
  return reinterpret_cast<void*>(a);
}
inline void AlignedFree64(void* p) {
  if (p != nullptr) std::free(static_cast<void**>(p)[-1]);
}

namespace dsp = snd::dsp;
constexpr std::uint32_t kRate = 48000;

struct Inst {
  explicit Inst(snd::Id type, std::uint32_t channels, std::vector<float> params) : p(std::move(params)) {
    d = snd::EffectDesc{type, channels, 256, static_cast<std::uint32_t>(p.size()), p.empty() ? nullptr : p.data(),
                        nullptr, 0, kRate, 0};
    bytes = dsp::EffectStateBytes(d, kRate);
    mem = bytes != 0u ? AlignedAlloc64(bytes) : nullptr;
    ok = mem != nullptr && dsp::EffectInit(mem, bytes, d, kRate);
  }
  ~Inst() { AlignedFree64(mem); }
  Inst(const Inst&) = delete;
  Inst& operator=(const Inst&) = delete;
  std::vector<float> p;
  snd::EffectDesc d{};
  std::uint32_t bytes = 0;
  void* mem = nullptr;
  bool ok = false;
};

// Runs a mono sine of `hz` through band 0 = allpass at `f0` and returns (gain, phase in deg)
// of the steady state.
void AllpassResponse(float f0, float q, double hz, double* gain, double* phaseDeg) {
  Inst e(dsp::kEffectParamEq, 1, {8.0f, f0, q, 0.0f, 0.0f, 500.0f, 0.707f, 0.0f, 0.0f, 2000.0f, 0.707f, 0.0f, 0.0f,
                                   8000.0f, 0.707f, 0.0f});
  if (!e.ok) {
    *gain = -1;
    return;
  }
  const std::size_t n = 48128;  // 188 blocks
  std::vector<float> x(n), y(n);
  for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(std::sin(6.283185307179586 * hz * double(i) / kRate));
  for (std::size_t at = 0; at < n; at += 256) {
    const float* in[1] = {x.data() + at};
    float* out[1] = {y.data() + at};
    dsp::EffectProcess(e.mem, in, out, 1, 256, nullptr);
  }
  // Steady state (last 0.5 s): quadrature correlation.
  double xi = 0, xq = 0, yi = 0, yq = 0;
  const double w = 6.283185307179586 * hz / kRate;
  for (std::size_t i = n / 2; i < n; ++i) {
    const double c = std::cos(w * double(i)), s = std::sin(w * double(i));
    xi += x[i] * s;
    xq += x[i] * c;
    yi += y[i] * s;
    yq += y[i] * c;
  }
  *gain = std::sqrt(yi * yi + yq * yq) / std::sqrt(xi * xi + xq * xq);
  double ph = (std::atan2(yq, yi) - std::atan2(xq, xi)) * 180.0 / 3.141592653589793;
  while (ph > 0.0) ph -= 360.0;
  while (ph <= -360.0) ph += 360.0;
  *phaseDeg = ph;
}

}  // namespace

// D-135: band type 8 is an allpass: gain 1 everywhere, -180 deg at its frequency
// (second-order allpass: 0 deg at DC, -360 deg at Nyquist).
SND_TEST(IndepG2Dsp, ParamEqAllpassBand) {
  for (const double hz : {60.0, 300.0, 1500.0, 6000.0, 15000.0}) {
    double g = 0, ph = 0;
    AllpassResponse(1500.0f, 0.9f, hz, &g, &ph);
    SND_CHECK_NEAR(g, 1.0, 2e-3);
  }
  double g = 0, ph = 0;
  AllpassResponse(1500.0f, 0.9f, 1500.0, &g, &ph);
  SND_CHECK_NEAR(ph, -180.0, 3.0);
  AllpassResponse(1500.0f, 0.9f, 60.0, &g, &ph);
  SND_CHECK(ph > -40.0);  // near 0 deg well below the frequency
  AllpassResponse(1500.0f, 0.9f, 15000.0, &g, &ph);
  SND_CHECK(ph < -320.0);  // near -360 deg well above
  // The band gain does not apply to an allpass.
  Inst e(dsp::kEffectParamEq, 1, {8.0f, 1000.0f, 1.0f, 24.0f});
  SND_REQUIRE(e.ok);
  std::vector<float> x(4096), y(4096);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(6.283185307179586 * 1000.0 * double(i) / kRate));
  for (std::size_t at = 0; at < x.size(); at += 256) {
    const float* in[1] = {x.data() + at};
    float* out[1] = {y.data() + at};
    dsp::EffectProcess(e.mem, in, out, 1, 256, nullptr);
  }
  float peak = 0;
  for (std::size_t i = 2048; i < y.size(); ++i) peak = std::fmax(peak, std::fabs(y[i]));
  SND_CHECK_NEAR(peak, 1.0, 0.01);
}

// Every standard kind, 1 / 2 / 6 channels, ParamBlock ramps whose start / end are NaN, +-Inf,
// +-1e30 and the range limits: the output stays finite and bounded, and the next normal call
// is normal (effects.hpp: NaN -> default, Inf -> the bound).
SND_TEST(IndepG2Dsp, NonFiniteAndOutOfRangeRampsStayFinite) {
  const dsp::EffectKind kinds[] = {
      dsp::EffectKind::Reverb,     dsp::EffectKind::Echo,       dsp::EffectKind::Delay,      dsp::EffectKind::Chorus,
      dsp::EffectKind::Flanger,    dsp::EffectKind::Distortion, dsp::EffectKind::Compressor, dsp::EffectKind::Limiter,
      dsp::EffectKind::ParamEq,    dsp::EffectKind::Lowpass,    dsp::EffectKind::Highpass,   dsp::EffectKind::PitchShifter,
      dsp::EffectKind::Normalize,  dsp::EffectKind::Surround};
  const float nan = std::nanf("");
  const float values[] = {nan, INFINITY, -INFINITY, 1e30f, -1e30f, 0.0f};
  for (const std::uint32_t ch : {1u, 2u, 6u}) {
    for (const dsp::EffectKind k : kinds) {
      const dsp::EffectInfo& info = dsp::GetEffectInfo(k);
      Inst e(info.typeId, ch, {});
      SND_REQUIRE(e.ok);
      std::vector<std::vector<float>> buf(ch, std::vector<float>(256));
      std::vector<float*> ptr(ch);
      for (std::uint32_t c = 0; c < ch; ++c) ptr[c] = buf[c].data();
      std::vector<float> start(info.paramCount), end(info.paramCount);
      std::uint64_t n = 0;
      bool finite = true;
      float peak = 0;
      for (const float a : values) {
        for (const float b : values) {
          for (std::uint32_t i = 0; i < info.paramCount; ++i) {
            start[i] = a;
            end[i] = b;
          }
          snd::ParamBlock pb{start.data(), end.data(), info.paramCount, 0xFFFFFFFFu, nullptr};
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (std::uint32_t i = 0; i < 256; ++i) {
              buf[c][i] = 0.8f * static_cast<float>(std::sin(0.05 * double(n + i) * (1.0 + c)));
            }
          }
          n += 256;
          dsp::EffectProcess(e.mem, ptr.data(), ptr.data(), ch, 256, &pb);
          for (std::uint32_t c = 0; c < ch; ++c) {
            for (float v : buf[c]) {
              finite = finite && std::isfinite(v);
              peak = std::fmax(peak, std::fabs(v));
            }
          }
          for (std::uint32_t i = 0; i < info.paramCount; ++i) {
            SND_CHECK(std::isfinite(dsp::EffectParamValue(e.mem, i)));
          }
        }
      }
      SND_CHECK_MSG(finite && peak < 1000.0f, "%s, %u channels: finite %d, peak %g", info.name, ch, finite ? 1 : 0, peak);
    }
  }
}
