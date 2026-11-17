#include "common/sample_format.hpp"

#include <cstring>

namespace snd::platform::detail {

const char* SampleFormatName(SampleFormat f) noexcept {
  switch (f) {
    case SampleFormat::Float32: return "float32";
    case SampleFormat::Int32: return "int32";
    case SampleFormat::Int16: return "int16";
  }
  return "?";
}

namespace {
// Clip to [-1, 1]; NaN -> 0 (comparisons with NaN are false).
inline double Clip(float x) noexcept {
  if (x >= 1.0f) return 1.0;
  if (x <= -1.0f) return -1.0;
  if (x > -1.0f) return static_cast<double>(x);
  return 0.0;
}
// Round half away from zero without libm (exact for |v| < 2^52).
inline std::int64_t RoundNearest(double v) noexcept {
  return v >= 0.0 ? static_cast<std::int64_t>(v + 0.5) : -static_cast<std::int64_t>(-v + 0.5);
}
}  // namespace

void ConvertFromFloat(const float* src, void* dst, std::size_t samples, SampleFormat format) noexcept {
  switch (format) {
    case SampleFormat::Float32:
      std::memcpy(dst, src, samples * sizeof(float));
      return;
    case SampleFormat::Int32: {
      auto* d = static_cast<std::int32_t*>(dst);
      for (std::size_t i = 0; i < samples; ++i) {
        d[i] = static_cast<std::int32_t>(RoundNearest(Clip(src[i]) * 2147483647.0));
      }
      return;
    }
    case SampleFormat::Int16: {
      auto* d = static_cast<std::int16_t*>(dst);
      for (std::size_t i = 0; i < samples; ++i) {
        d[i] = static_cast<std::int16_t>(RoundNearest(Clip(src[i]) * 32767.0));
      }
      return;
    }
  }
}

}  // namespace snd::platform::detail
