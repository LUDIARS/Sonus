#include "snd/dsp/kernels.hpp"

#include <cmath>
#include <cstring>

#include "snd/platform/simd.hpp"

namespace snd::dsp {

namespace simd = snd::simd;
using simd::F32x4;
using simd::F32xN;
inline constexpr std::uint32_t W = simd::kWidth;

namespace {
// Lane offsets 0..W-1 as a vector.
inline F32xN LaneIndex() noexcept {
  alignas(64) static constexpr float kIdx[8] = {0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f};
  return F32xN::Load(kIdx);
}
}  // namespace

void Clear(float* dst, std::uint32_t frames) noexcept {
  if (frames > 0) std::memset(dst, 0, frames * sizeof(float));
}

void Clear(float* const* channels, std::uint32_t channelCount, std::uint32_t frames) noexcept {
  for (std::uint32_t c = 0; c < channelCount; ++c) Clear(channels[c], frames);
}

void Copy(float* dst, const float* src, std::uint32_t frames) noexcept {
  if (dst != src && frames > 0) std::memcpy(dst, src, frames * sizeof(float));
}

void Mix(float* dst, const float* src, std::uint32_t frames) noexcept {
  std::uint32_t i = 0;
  for (; i + W <= frames; i += W) (F32xN::LoadU(dst + i) + F32xN::LoadU(src + i)).StoreU(dst + i);
  for (; i < frames; ++i) dst[i] += src[i];
}

void Scale(float* buf, std::uint32_t frames, float gain) noexcept {
  const F32xN g = F32xN::Set1(gain);
  std::uint32_t i = 0;
  for (; i + W <= frames; i += W) (F32xN::LoadU(buf + i) * g).StoreU(buf + i);
  for (; i < frames; ++i) buf[i] *= gain;
}

// g(i) = start + step * i, computed identically in the vector and scalar tails
// (start + step * float(i), one multiply and one add).
void CopyGainRamp(float* dst, const float* src, std::uint32_t frames, float gainStart,
                  float gainEnd) noexcept {
  if (frames == 0) return;
  if (gainStart == gainEnd) {
    const F32xN g = F32xN::Set1(gainStart);
    std::uint32_t i = 0;
    for (; i + W <= frames; i += W) (F32xN::LoadU(src + i) * g).StoreU(dst + i);
    for (; i < frames; ++i) dst[i] = src[i] * gainStart;
    return;
  }
  const float step = (gainEnd - gainStart) / static_cast<float>(frames);
  const F32xN vs = F32xN::Set1(step), v0 = F32xN::Set1(gainStart);
  F32xN idx = LaneIndex();
  const F32xN inc = F32xN::Set1(static_cast<float>(W));
  std::uint32_t i = 0;
  for (; i + W <= frames; i += W) {
    const F32xN g = (vs * idx) + v0;
    (F32xN::LoadU(src + i) * g).StoreU(dst + i);
    idx = idx + inc;
  }
  for (; i < frames; ++i) {
    const float p = step * static_cast<float>(i);
    dst[i] = src[i] * (p + gainStart);
  }
}

void MixGainRamp(float* dst, const float* src, std::uint32_t frames, float gainStart,
                 float gainEnd) noexcept {
  if (frames == 0) return;
  if (gainStart == gainEnd) {
    if (gainStart == 0.0f) return;
    const F32xN g = F32xN::Set1(gainStart);
    std::uint32_t i = 0;
    for (; i + W <= frames; i += W) {
      ((F32xN::LoadU(src + i) * g) + F32xN::LoadU(dst + i)).StoreU(dst + i);
    }
    for (; i < frames; ++i) {
      const float p = src[i] * gainStart;
      dst[i] = p + dst[i];
    }
    return;
  }
  const float step = (gainEnd - gainStart) / static_cast<float>(frames);
  const F32xN vs = F32xN::Set1(step), v0 = F32xN::Set1(gainStart);
  F32xN idx = LaneIndex();
  const F32xN inc = F32xN::Set1(static_cast<float>(W));
  std::uint32_t i = 0;
  for (; i + W <= frames; i += W) {
    const F32xN g = (vs * idx) + v0;
    ((F32xN::LoadU(src + i) * g) + F32xN::LoadU(dst + i)).StoreU(dst + i);
    idx = idx + inc;
  }
  for (; i < frames; ++i) {
    const float p = step * static_cast<float>(i);
    const float g = p + gainStart;
    const float s = src[i] * g;
    dst[i] = s + dst[i];
  }
}

void CopyGainRamp(float* const* dst, const float* const* src, std::uint32_t channels,
                  std::uint32_t frames, float gainStart, float gainEnd) noexcept {
  for (std::uint32_t c = 0; c < channels; ++c) CopyGainRamp(dst[c], src[c], frames, gainStart, gainEnd);
}

void MixGainRamp(float* const* dst, const float* const* src, std::uint32_t channels,
                 std::uint32_t frames, float gainStart, float gainEnd) noexcept {
  for (std::uint32_t c = 0; c < channels; ++c) MixGainRamp(dst[c], src[c], frames, gainStart, gainEnd);
}

float Peak(const float* src, std::uint32_t frames) noexcept {
  F32xN m = F32xN::Zero();
  std::uint32_t i = 0;
  // Max(abs, m) returns m when abs is NaN (second operand), so NaN is ignored.
  for (; i + W <= frames; i += W) m = simd::Max(simd::Abs(F32xN::LoadU(src + i)), m);
  float r = simd::ReduceMax(m);
  for (; i < frames; ++i) {
    const float a = std::fabs(src[i]);
    r = a > r ? a : r;
  }
  return r;
}

double SumOfSquares(const float* src, std::uint32_t frames) noexcept {
  // Float partial sums over chunks of 64 frames, accumulated in double.
  double total = 0.0;
  std::uint32_t i = 0;
  while (i + W <= frames) {
    F32xN acc = F32xN::Zero();
    const std::uint32_t end = (frames - i) >= 64 ? i + 64 : i + ((frames - i) / W) * W;
    for (; i < end; i += W) {
      const F32xN x = F32xN::LoadU(src + i);
      acc = (x * x) + acc;
    }
    total += static_cast<double>(simd::ReduceAdd(acc));
  }
  for (; i < frames; ++i) total += static_cast<double>(src[i]) * static_cast<double>(src[i]);
  return total;
}

bool HasNonFinite(const float* src, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (!std::isfinite(src[i])) return true;
  }
  return false;
}

bool HasDenormal(const float* src, std::uint32_t frames) noexcept {
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (std::fpclassify(src[i]) == FP_SUBNORMAL) return true;
  }
  return false;
}

void Interleave(const float* const* in, std::uint32_t channels, std::uint32_t frames,
                float* out) noexcept {
  if (channels == 2) {
    const float* l = in[0];
    const float* r = in[1];
    for (std::uint32_t i = 0; i < frames; ++i) {
      out[2 * i] = l[i];
      out[2 * i + 1] = r[i];
    }
    return;
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    const float* s = in[c];
    float* d = out + c;
    for (std::uint32_t i = 0; i < frames; ++i) d[i * channels] = s[i];
  }
}

void Deinterleave(const float* in, std::uint32_t channels, std::uint32_t frames,
                  float* const* out) noexcept {
  if (channels == 2) {
    float* l = out[0];
    float* r = out[1];
    for (std::uint32_t i = 0; i < frames; ++i) {
      l[i] = in[2 * i];
      r[i] = in[2 * i + 1];
    }
    return;
  }
  for (std::uint32_t c = 0; c < channels; ++c) {
    const float* s = in + c;
    float* d = out[c];
    for (std::uint32_t i = 0; i < frames; ++i) d[i] = s[i * channels];
  }
}

float DbToGain(float db) noexcept {
  if (!(db > kMinDb)) return 0.0f;
  return std::pow(10.0f, db * 0.05f);
}

float GainToDb(float gain) noexcept {
  const float a = std::fabs(gain);
  if (!(a > 0.0f)) return kMinDb;
  const float db = 20.0f * std::log10(a);
  return db < kMinDb ? kMinDb : db;
}

}  // namespace snd::dsp
