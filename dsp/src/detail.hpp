// Private helpers of the dsp module. Generic parts that belong in base once it
// exists (listed in the stage-1 report): Carver (bump layout over caller
// memory), math helpers, denormal flushing.
#ifndef SND_DSP_SRC_DETAIL_HPP
#define SND_DSP_SRC_DETAIL_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace snd::dsp::detail {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kPiF = 3.14159265358979323846f;
inline constexpr float kTailSilence = 1.0e-5f;  // -100 dBFS
inline constexpr float kFlushThreshold = 1.0e-20f;

template <class T>
constexpr T Clamp(T x, T lo, T hi) noexcept {
  return x < lo ? lo : (x > hi ? hi : x);
}
template <class T>
constexpr T Max(T a, T b) noexcept {
  return a > b ? a : b;
}
template <class T>
constexpr T Min(T a, T b) noexcept {
  return a < b ? a : b;
}

constexpr std::size_t AlignUp(std::size_t n, std::size_t a) noexcept {
  return (n + (a - 1)) & ~(a - 1);
}

inline float Flush(float x) noexcept { return std::fabs(x) < kFlushThreshold ? 0.0f : x; }
// Per-sample flush for recursive state fed by silence: keeps values far from
// the denormal range without FTZ/DAZ (branchless on SSE: andps/cmpps).
inline constexpr float kStateFloor = 1.0e-15f;
inline float FlushState(float x) noexcept { return std::fabs(x) < kStateFloor ? 0.0f : x; }

inline float DbToLin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float MbToLin(float mb) noexcept { return std::pow(10.0f, mb * 0.0005f); }

// One-pole smoothing coefficient for a time constant (seconds): y += (1 - c) (x - y).
inline float TimeCoef(float seconds, float sampleRate) noexcept {
  if (!(seconds > 0.0f)) return 0.0f;
  return std::exp(-1.0f / (seconds * sampleRate));
}

// Bump layout over caller memory. With base == nullptr it only measures.
// Offsets are relative to the state start so the state stays position
// independent (offsets, not pointers, are stored).
class Carver {
 public:
  explicit Carver(void* base, std::size_t start) noexcept
      : base_(static_cast<std::uint8_t*>(base)), off_(start) {}
  template <class T>
  std::uint32_t Take(std::size_t count, std::size_t align = 64) noexcept {
    off_ = AlignUp(off_, align);
    const std::size_t at = off_;
    off_ += count * sizeof(T);
    if (base_ != nullptr && count > 0) std::memset(base_ + at, 0, count * sizeof(T));
    return static_cast<std::uint32_t>(at);
  }
  std::size_t Size() const noexcept { return AlignUp(off_, 64); }

 private:
  std::uint8_t* base_;
  std::size_t off_;
};

template <class T>
inline T* At(void* base, std::uint32_t offset) noexcept {
  return reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
}
template <class T>
inline const T* At(const void* base, std::uint32_t offset) noexcept {
  return reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + offset);
}

// Max |x| over channels and frames.
inline float BlockPeak(const float* const* ch, std::uint32_t channels, std::uint32_t frames) noexcept {
  float m = 0.0f;
  for (std::uint32_t c = 0; c < channels; ++c) {
    const float* p = ch[c];
    for (std::uint32_t i = 0; i < frames; ++i) {
      const float a = std::fabs(p[i]);
      m = a > m ? a : m;
    }
  }
  return m;
}

// Zeroth-order modified Bessel function (Kaiser window).
inline double BesselI0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  const double q = x * x * 0.25;
  for (int k = 1; k < 64; ++k) {
    term *= q / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

// Fractional delay read with linear interpolation from a ring of `size`
// samples (size a power of two not required). `delay` in samples >= 0,
// relative to the sample written last (`write` = index of the next write).
inline float ReadFrac(const float* ring, std::uint32_t size, std::uint32_t write,
                      float delay) noexcept {
  const float d = delay + 1.0f;  // delay 0 = the sample written last
  const std::uint32_t di = static_cast<std::uint32_t>(d);
  const float f = d - static_cast<float>(di);
  std::uint32_t i0 = write + size - (di % size);
  if (i0 >= size) i0 -= size;
  std::uint32_t i1 = i0 == 0 ? size - 1 : i0 - 1;  // one sample older
  return ring[i0] + (ring[i1] - ring[i0]) * f;
}

}  // namespace snd::dsp::detail

#endif  // SND_DSP_SRC_DETAIL_HPP
