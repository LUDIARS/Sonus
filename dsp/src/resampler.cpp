#include "snd/dsp/resampler.hpp"

#include <cmath>
#include <cstring>

#include "detail.hpp"
#include "snd/platform/simd.hpp"

namespace snd::dsp {

namespace {

using simd::F32x4;

// ---------------------------------------------------------------------------
// Windowed-sinc tables (ratios up to 4)
// ---------------------------------------------------------------------------
constexpr std::uint32_t kSincTaps = 32;      // taps i-15 .. i+16
constexpr std::uint32_t kSincHalf = 16;
constexpr std::uint32_t kSincPhaseBits = 7;  // 128 rows (+1 for interpolation)
constexpr std::uint32_t kSincPhases = 1u << kSincPhaseBits;
constexpr double kKaiserBeta = 8.0;          // ~80 dB stopband
constexpr double kCutoff = 0.45;             // fraction of the input sample rate at ratio <= 1
// One table per ratio band: the table for band t is used when the block's
// max ratio is <= kTableRatio[t]; its cutoff is kCutoff / kTableRatio[t].
constexpr float kTableRatio[] = {1.0f, 1.1f, 1.2f, 1.35f, 1.5f, 1.7f, 2.0f, 2.4f, 2.8f, 3.4f, 4.0f};
constexpr std::uint32_t kTableCount = sizeof(kTableRatio) / sizeof(kTableRatio[0]);
constexpr float kTableMaxRatio = 4.0f;  // tables up to here, ratio-scaled kernel above

alignas(64) float gSinc[kTableCount][kSincPhases + 1][kSincTaps];

bool BuildSincTables() noexcept {
  const double i0b = detail::BesselI0(kKaiserBeta);
  for (std::uint32_t t = 0; t < kTableCount; ++t) {
    const double fc = kCutoff / static_cast<double>(kTableRatio[t]);  // cycles per input sample
    for (std::uint32_t p = 0; p <= kSincPhases; ++p) {
      const double frac = static_cast<double>(p) / static_cast<double>(kSincPhases);
      double row[kSincTaps];
      double sum = 0.0;
      for (std::uint32_t j = 0; j < kSincTaps; ++j) {
        // Tap j reads input i - 15 + j; the output position is i + frac.
        const double x = (static_cast<double>(j) - 15.0) - frac;
        const double u = x / static_cast<double>(kSincHalf);
        double w = 0.0;
        if (u > -1.0 && u < 1.0) w = detail::BesselI0(kKaiserBeta * std::sqrt(1.0 - u * u)) / i0b;
        const double arg = 2.0 * fc * x;
        const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(detail::kPi * arg) / (detail::kPi * arg);
        row[j] = 2.0 * fc * sinc * w;
        sum += row[j];
      }
      for (std::uint32_t j = 0; j < kSincTaps; ++j) {
        gSinc[t][p][j] = static_cast<float>(row[j] / sum);  // unity DC gain per phase
      }
    }
  }
  return true;
}

// Built during static initialisation (before main, never on an RT thread).
[[maybe_unused]] const bool gSincReady = BuildSincTables();

std::uint32_t TableFor(float maxRatio) noexcept {
  for (std::uint32_t t = 0; t < kTableCount; ++t) {
    if (maxRatio <= kTableRatio[t]) return t;
  }
  return kTableCount - 1;
}

// ---------------------------------------------------------------------------
// Ratio-scaled kernel for ratios above 4:
//   g(v) = 2 c0 sinc(2 c0 v) kaiser(v / W0),  v = x / ratio (OUTPUT samples)
// so cutoff, transition band and stopband stay at fixed OUTPUT frequencies:
//   c0 = 0.5 (transition centred on the output Nyquist), W0 = 6, beta 8.6.
// Taps per output frame and channel: 2 * ceil(6 * ratio) (48 just above 4, 384 at 32).
// ---------------------------------------------------------------------------
constexpr double kWideHalf = 6.0;
constexpr double kWideCutoff = 0.5;
constexpr double kWideBeta = 8.6;
constexpr std::uint32_t kWideRes = 512;  // table points per output-sample unit
constexpr std::uint32_t kWidePoints = static_cast<std::uint32_t>(2.0 * kWideHalf) * kWideRes + 2u;
constexpr std::uint32_t kWideMaxTaps = 2u * 6u * 32u;  // 2 * ceil(6 * kResamplerMaxRatio)
constexpr std::uint32_t kWideTableFloats = 8192;       // per-call polyphase table (32 KiB of stack)

alignas(64) float gWide[kWidePoints];

bool BuildWideTable() noexcept {
  const double i0b = detail::BesselI0(kWideBeta);
  for (std::uint32_t k = 0; k < kWidePoints; ++k) {
    const double v = static_cast<double>(k) / kWideRes - kWideHalf;
    const double u = v / kWideHalf;
    double w = 0.0;
    if (u > -1.0 && u < 1.0) w = detail::BesselI0(kWideBeta * std::sqrt(1.0 - u * u)) / i0b;
    const double arg = 2.0 * kWideCutoff * v;
    const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(detail::kPi * arg) / (detail::kPi * arg);
    gWide[k] = static_cast<float>(2.0 * kWideCutoff * sinc * w);
  }
  return true;
}
[[maybe_unused]] const bool gWideReady = BuildWideTable();

inline float WideCoef(float v) noexcept {
  const float u = (v + static_cast<float>(kWideHalf)) * static_cast<float>(kWideRes);
  if (!(u > 0.0f) || u >= static_cast<float>(kWidePoints - 1u)) return 0.0f;
  const std::uint32_t i = static_cast<std::uint32_t>(u);
  const float f = u - static_cast<float>(i);
  return gWide[i] + (gWide[i + 1] - gWide[i]) * f;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Header {
  std::uint32_t frac;      // 2^-32 frame units
  std::uint16_t phaseInt;  // index of the next output's base sample in the history window
  std::uint8_t quality;
  std::uint8_t channels;
  std::uint16_t hist;      // history frames kept (H)
  std::uint16_t reserved;
  float maxRatio;          // ratios are clamped to [kResamplerMinRatio, maxRatio]
};
static_assert(sizeof(Header) == 16);

float ClampMaxRatio(float r) noexcept {
  if (!(r >= 1.0f)) r = 1.0f;  // also NaN
  return r > kResamplerMaxRatio ? kResamplerMaxRatio : r;
}

// Look-ahead R (frames after the base sample that are read) for a block whose
// largest ratio is r.
std::uint32_t RightFor(ResamplerQuality q, float r) noexcept {
  switch (q) {
    case ResamplerQuality::Linear: return 1;
    case ResamplerQuality::Cubic: return 2;
    case ResamplerQuality::Sinc:
      return r <= kTableMaxRatio
                 ? kSincHalf
                 : static_cast<std::uint32_t>(std::ceil(kWideHalf * static_cast<double>(r)));
  }
  return 1;
}

std::uint32_t HistFor(ResamplerQuality q, float maxRatio) noexcept {
  return 2u * RightFor(q, ClampMaxRatio(maxRatio));
}

std::uint32_t ClampChannels(std::uint32_t channels) noexcept {
  if (channels == 0) return 1;
  return channels > kMaxChannels ? kMaxChannels : channels;
}

Header* Hdr(void* s) noexcept { return static_cast<Header*>(s); }
const Header* Hdr(const void* s) noexcept { return static_cast<const Header*>(s); }
float* History(void* s) noexcept {
  return reinterpret_cast<float*>(static_cast<std::uint8_t*>(s) + sizeof(Header));
}

float ClampRatio(float r, float maxRatio) noexcept {
  if (!(r >= kResamplerMinRatio)) r = kResamplerMinRatio;  // also NaN
  return r > maxRatio ? maxRatio : r;
}

std::uint64_t RatioToFixed(float r) noexcept {
  return static_cast<std::uint64_t>(static_cast<double>(r) * 4294967296.0 + 0.5);
}

// Increment for output frame k of n (shared by InputFrames and Process).
struct IncRamp {
  std::int64_t start;
  std::int64_t delta;
  std::int64_t n;
  std::uint64_t At(std::uint32_t k) const noexcept {
    if (delta == 0) return static_cast<std::uint64_t>(start);
    return static_cast<std::uint64_t>(start + (delta * static_cast<std::int64_t>(k)) / n);
  }
};

struct Block {
  IncRamp ramp;
  float maxRatio;       // largest (clamped) ratio of the call
  std::uint32_t right;  // R for this call
};

Block MakeBlock(const Header* h, float r0, float r1, std::uint32_t n) noexcept {
  const float a = ClampRatio(r0, h->maxRatio), b = ClampRatio(r1, h->maxRatio);
  const std::int64_t fa = static_cast<std::int64_t>(RatioToFixed(a));
  const std::int64_t fb = static_cast<std::int64_t>(RatioToFixed(b));
  Block blk;
  blk.ramp = {fa, fb - fa, static_cast<std::int64_t>(n == 0 ? 1 : n)};
  blk.maxRatio = a > b ? a : b;
  blk.right = RightFor(static_cast<ResamplerQuality>(h->quality), blk.maxRatio);
  return blk;
}

std::uint64_t StartPos(const Header* h) noexcept {
  return (static_cast<std::uint64_t>(h->phaseInt) << 32) | h->frac;
}

// Position of the last output frame (before its increment) and the position after n frames.
void Advance(std::uint64_t pos, const IncRamp& ramp, std::uint32_t n, std::uint64_t* last,
             std::uint64_t* end) noexcept {
  std::uint64_t p = pos;
  std::uint64_t l = pos;
  if (ramp.delta == 0) {
    const std::uint64_t inc = static_cast<std::uint64_t>(ramp.start);
    l = pos + inc * (n - 1u);
    p = l + inc;
  } else {
    for (std::uint32_t k = 0; k < n; ++k) {
      l = p;
      p += ramp.At(k);
    }
  }
  *last = l;
  *end = p;
}

std::uint32_t NeedFrames(const Header* h, std::uint32_t n, const Block& blk,
                         std::uint64_t* endPos) noexcept {
  const std::uint32_t hist = h->hist;
  std::uint64_t last = 0, end = 0;
  Advance(StartPos(h), blk.ramp, n, &last, &end);
  if (endPos != nullptr) *endPos = end;
  const std::uint64_t iLast = last >> 32;
  const std::uint64_t reach = iLast + blk.right + 1u;  // window samples needed
  return reach > hist ? static_cast<std::uint32_t>(reach - hist) : 0u;
}

inline float FracToFloat(std::uint32_t frac) noexcept {
  return static_cast<float>(frac >> 8) * (1.0f / 16777216.0f);
}

}  // namespace

std::uint32_t ResamplerHistoryFrames(ResamplerQuality q, float maxRatio) noexcept {
  return HistFor(q, maxRatio);
}

std::uint32_t ResamplerLookaheadFrames(ResamplerQuality q, float ratio) noexcept {
  return RightFor(q, ClampMaxRatio(ratio));
}

std::uint32_t ResamplerPadFrames(ResamplerQuality q, float maxRatio) noexcept {
  return static_cast<std::uint32_t>(detail::AlignUp(ResamplerHistoryFrames(q, maxRatio), 16));
}

std::size_t ResamplerStateBytes(ResamplerQuality q, std::uint32_t channels, float maxRatio) noexcept {
  return sizeof(Header) + sizeof(float) * ResamplerHistoryFrames(q, maxRatio) * ClampChannels(channels);
}

bool ResamplerFitsInline(ResamplerQuality q, std::uint32_t channels, float maxRatio) noexcept {
  return ResamplerStateBytes(q, channels, maxRatio) <= 64u;  // the 1.0 RealVoice.resamplerState column size (removed in 1.1, D-44)
}

void ResamplerInit(void* state, ResamplerQuality q, std::uint32_t channels, float maxRatio) noexcept {
  channels = ClampChannels(channels);
  // Every byte of the state is written: the memory may hold anything.
  std::memset(state, 0, ResamplerStateBytes(q, channels, maxRatio));
  Header* h = Hdr(state);
  h->quality = static_cast<std::uint8_t>(q);
  h->channels = static_cast<std::uint8_t>(channels);
  h->hist = static_cast<std::uint16_t>(ResamplerHistoryFrames(q, maxRatio));
  h->phaseInt = h->hist;
  h->frac = 0;
  h->maxRatio = ClampMaxRatio(maxRatio);
}

void ResamplerReset(void* state) noexcept {
  const Header h = *Hdr(state);
  ResamplerInit(state, static_cast<ResamplerQuality>(h.quality), h.channels, h.maxRatio);
}

ResamplerQuality ResamplerGetQuality(const void* state) noexcept {
  return static_cast<ResamplerQuality>(Hdr(state)->quality);
}

std::uint32_t ResamplerGetChannels(const void* state) noexcept { return Hdr(state)->channels; }

double ResamplerPendingOffset(const void* state) noexcept {
  const Header* h = Hdr(state);
  return static_cast<double>(h->phaseInt) + static_cast<double>(h->frac) / 4294967296.0 -
         static_cast<double>(h->hist);
}

std::uint32_t ResamplerInputFrames(const void* state, std::uint32_t outFrames, float ratioStart,
                                   float ratioEnd) noexcept {
  if (outFrames == 0) return 0;
  const Header* h = Hdr(state);
  return NeedFrames(h, outFrames, MakeBlock(h, ratioStart, ratioEnd, outFrames), nullptr);
}

std::uint32_t ResamplerProcess(void* state, float* const* in, std::uint32_t inFrames,
                               float* const* out, std::uint32_t outFrames, float ratioStart,
                               float ratioEnd) noexcept {
  Header* h = Hdr(state);
  const std::uint32_t channels = h->channels;
  if (outFrames == 0) return 0;
  const ResamplerQuality q = static_cast<ResamplerQuality>(h->quality);
  const Block blk = MakeBlock(h, ratioStart, ratioEnd, outFrames);
  const IncRamp& ramp = blk.ramp;
  std::uint64_t endPos = 0;
  const std::uint32_t need = NeedFrames(h, outFrames, blk, &endPos);
  if (inFrames < need) {
    for (std::uint32_t c = 0; c < channels; ++c) std::memset(out[c], 0, outFrames * sizeof(float));
    return 0;
  }
  const std::uint32_t hist = h->hist;
  const std::uint32_t pad = static_cast<std::uint32_t>(detail::AlignUp(hist, 16));
  float* histStore = History(state);

  // Window: win[c][0..hist) = history, win[c][hist..hist+need) = new frames.
  float* win[kMaxChannels];
  for (std::uint32_t c = 0; c < channels; ++c) {
    win[c] = in[c] + (pad - hist);
    std::memcpy(win[c], histStore + c * hist, hist * sizeof(float));
  }

  std::uint64_t pos = StartPos(h);
  switch (q) {
    case ResamplerQuality::Linear: {
      for (std::uint32_t k = 0; k < outFrames; ++k) {
        const std::uint32_t i = static_cast<std::uint32_t>(pos >> 32);
        const float t = FracToFloat(static_cast<std::uint32_t>(pos));
        for (std::uint32_t c = 0; c < channels; ++c) {
          const float x0 = win[c][i], x1 = win[c][i + 1];
          const float d = (x1 - x0) * t;
          out[c][k] = x0 + d;
        }
        pos += ramp.At(k);
      }
      break;
    }
    case ResamplerQuality::Cubic: {
      for (std::uint32_t k = 0; k < outFrames; ++k) {
        const std::uint32_t i = static_cast<std::uint32_t>(pos >> 32);
        const float t = FracToFloat(static_cast<std::uint32_t>(pos));
        for (std::uint32_t c = 0; c < channels; ++c) {
          const float* x = win[c] + i;
          const float xm1 = x[-1], x0 = x[0], x1 = x[1], x2 = x[2];
          const float c1 = 0.5f * (x1 - xm1);
          const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
          const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
          out[c][k] = ((c3 * t + c2) * t + c1) * t + x0;
        }
        pos += ramp.At(k);
      }
      break;
    }
    case ResamplerQuality::Sinc: {
      if (blk.maxRatio <= kTableMaxRatio) {
        const float (*table)[kSincTaps] = gSinc[TableFor(blk.maxRatio)];
        constexpr std::uint32_t kShift = 32u - kSincPhaseBits;
        constexpr float kSubScale = 1.0f / static_cast<float>(1u << kShift);
        for (std::uint32_t k = 0; k < outFrames; ++k) {
          const std::uint32_t i = static_cast<std::uint32_t>(pos >> 32);
          const std::uint32_t frac = static_cast<std::uint32_t>(pos);
          const std::uint32_t p = frac >> kShift;
          const F32x4 t = F32x4::Set1(static_cast<float>(frac & ((1u << kShift) - 1u)) * kSubScale);
          const float* r0 = table[p];
          const float* r1 = table[p + 1];
          F32x4 coef[kSincTaps / 4];
          for (std::uint32_t j = 0; j < kSincTaps / 4; ++j) {
            const F32x4 a = F32x4::Load(r0 + 4 * j);
            const F32x4 b = F32x4::Load(r1 + 4 * j);
            coef[j] = a + (b - a) * t;
          }
          const std::uint32_t base = i - (kSincHalf - 1u);
          for (std::uint32_t c = 0; c < channels; ++c) {
            const float* x = win[c] + base;
            F32x4 acc0 = coef[0] * F32x4::LoadU(x);
            F32x4 acc1 = coef[1] * F32x4::LoadU(x + 4);
            acc0 = simd::MulAdd(coef[2], F32x4::LoadU(x + 8), acc0);
            acc1 = simd::MulAdd(coef[3], F32x4::LoadU(x + 12), acc1);
            acc0 = simd::MulAdd(coef[4], F32x4::LoadU(x + 16), acc0);
            acc1 = simd::MulAdd(coef[5], F32x4::LoadU(x + 20), acc1);
            acc0 = simd::MulAdd(coef[6], F32x4::LoadU(x + 24), acc0);
            acc1 = simd::MulAdd(coef[7], F32x4::LoadU(x + 28), acc1);
            out[c][k] = simd::ReduceAdd(acc0 + acc1);
          }
          pos += ramp.At(k);
        }
      } else {
        // Ratio-scaled kernel, 2R taps. A polyphase table for this call's ratio
        // is built on the stack (P + 1 rows, each normalised to unity DC; rows
        // padded to a multiple of 4 taps), then rows are interpolated per frame
        // like the fixed tables. P = 64 phases while the table fits 32 KiB,
        // fewer for very wide kernels (the kernel is then smooth in input samples).
        const std::uint32_t R = blk.right;
        const std::uint32_t taps = 2u * R;
        const std::uint32_t stride = (taps + 3u) & ~3u;
        std::uint32_t phases = kWideTableFloats / stride - 1u;
        if (phases > 64u) phases = 64u;
        const float invRatio = 1.0f / blk.maxRatio;
        alignas(64) float tab[kWideTableFloats];
        for (std::uint32_t p = 0; p <= phases; ++p) {
          float* row = tab + p * stride;
          const float f = static_cast<float>(p) / static_cast<float>(phases);
          float sum = 0.0f;
          for (std::uint32_t j = 0; j < taps; ++j) {
            // Tap j reads input i - R + 1 + j, at distance x = (j - R + 1) - f.
            const float x =
                static_cast<float>(static_cast<std::int32_t>(j) - static_cast<std::int32_t>(R) + 1) - f;
            row[j] = WideCoef(x * invRatio);
            sum += row[j];
          }
          const float norm = sum != 0.0f ? 1.0f / sum : 0.0f;
          for (std::uint32_t j = 0; j < taps; ++j) row[j] *= norm;
          for (std::uint32_t j = taps; j < stride; ++j) row[j] = 0.0f;
        }
        const float phaseScale = static_cast<float>(phases) / 4294967296.0f;
        alignas(64) float coef[kWideMaxTaps];
        for (std::uint32_t k = 0; k < outFrames; ++k) {
          const std::uint32_t i = static_cast<std::uint32_t>(pos >> 32);
          const float u = static_cast<float>(static_cast<std::uint32_t>(pos)) * phaseScale;
          std::uint32_t p = static_cast<std::uint32_t>(u);
          if (p >= phases) p = phases - 1u;
          const F32x4 t = F32x4::Set1(u - static_cast<float>(p));
          const float* r0 = tab + p * stride;
          const float* r1 = r0 + stride;
          for (std::uint32_t j = 0; j < stride; j += 4) {
            const F32x4 a = F32x4::Load(r0 + j);
            const F32x4 b = F32x4::Load(r1 + j);
            (a + (b - a) * t).Store(coef + j);
          }
          const std::uint32_t base = i - (R - 1u);
          for (std::uint32_t c = 0; c < channels; ++c) {
            const float* x = win[c] + base;
            F32x4 acc0 = F32x4::Zero(), acc1 = F32x4::Zero();
            std::uint32_t j = 0;
            for (; j + 8 <= taps; j += 8) {
              acc0 = simd::MulAdd(F32x4::Load(coef + j), F32x4::LoadU(x + j), acc0);
              acc1 = simd::MulAdd(F32x4::Load(coef + j + 4), F32x4::LoadU(x + j + 4), acc1);
            }
            if (j + 4 <= taps) {
              acc0 = simd::MulAdd(F32x4::Load(coef + j), F32x4::LoadU(x + j), acc0);
              j += 4;
            }
            float tail = 0.0f;  // taps is even: 0 or 2 left (never read past the window)
            for (; j < taps; ++j) tail += coef[j] * x[j];
            out[c][k] = simd::ReduceAdd(acc0 + acc1) + tail;
          }
          pos += ramp.At(k);
        }
      }
      break;
    }
  }

  // Keep the last `hist` window samples and rebase the phase.
  for (std::uint32_t c = 0; c < channels; ++c) {
    std::memmove(histStore + c * hist, win[c] + need, hist * sizeof(float));
  }
  const std::uint64_t iNext = endPos >> 32;
  h->phaseInt = static_cast<std::uint16_t>(iNext - need);
  h->frac = static_cast<std::uint32_t>(endPos);
  return need;
}

}  // namespace snd::dsp
