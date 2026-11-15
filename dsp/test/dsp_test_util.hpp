// Helpers shared by the dsp unit tests (test-only; allocation happens outside RT scopes).
#ifndef SND_DSP_TEST_UTIL_HPP
#define SND_DSP_TEST_UTIL_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "snd/test/golden.hpp"

namespace dsptest {

inline constexpr double kPi = 3.14159265358979323846;

// 64-byte aligned memory block.
class AlignedMem {
 public:
  AlignedMem() = default;
  // fill: initial byte value. DSP states are created on dirty memory (0xFF =
  // NaN floats) to prove that every Init writes all of its state.
  explicit AlignedMem(std::size_t bytes, int fill = 0) { Reset(bytes, fill); }
  ~AlignedMem() { std::free(p_); }
  AlignedMem(const AlignedMem&) = delete;
  AlignedMem& operator=(const AlignedMem&) = delete;
  void Reset(std::size_t bytes, int fill = 0) {
    std::free(p_);
    bytes_ = (bytes + 63u) & ~std::size_t{63};
    if (bytes_ == 0) bytes_ = 64;
    p_ = std::aligned_alloc(64, bytes_);
    std::memset(p_, fill, bytes_);
  }
  void* get() const { return p_; }
  std::size_t size() const { return bytes_; }

 private:
  void* p_ = nullptr;
  std::size_t bytes_ = 0;
};

// Memory for DSP state: deliberately dirty.
inline constexpr int kDirty = 0xFF;

// Non-interleaved multi-channel audio, each channel 64-byte aligned, with an
// optional writable pad in front (resampler input).
class Audio {
 public:
  Audio(std::uint32_t channels, std::uint32_t frames, std::uint32_t pad = 0)
      : channels_(channels), frames_(frames), pad_(pad),
        stride_(((pad + frames + 15u) / 16u) * 16u),
        mem_(sizeof(float) * stride_ * (channels == 0 ? 1 : channels)) {
    for (std::uint32_t c = 0; c < 8; ++c) ptr_[c] = nullptr;
    for (std::uint32_t c = 0; c < channels; ++c) ptr_[c] = Base(c) + pad_;
  }
  float* Base(std::uint32_t c) const { return static_cast<float*>(mem_.get()) + c * stride_; }
  float* const* p() const { return ptr_; }
  const float* const* cp() const { return ptr_; }
  float* ch(std::uint32_t c) const { return ptr_[c]; }
  std::uint32_t channels() const { return channels_; }
  std::uint32_t frames() const { return frames_; }
  void Zero() { std::memset(mem_.get(), 0, mem_.size()); }

 private:
  std::uint32_t channels_, frames_, pad_, stride_;
  AlignedMem mem_;
  float* ptr_[8];
};

// Hann-windowed single-bin DFT amplitude of `x` at `freq` (rate sr).
inline double ToneAmplitude(const float* x, std::uint32_t n, double freq, double sr) {
  double re = 0.0, im = 0.0, wsum = 0.0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    const double a = 2.0 * kPi * freq * i / sr;
    re += w * x[i] * std::cos(a);
    im -= w * x[i] * std::sin(a);
    wsum += w;
  }
  return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

inline double Db(double x) { return 20.0 * std::log10(x > 1e-30 ? x : 1e-30); }

inline double Rms(const float* x, std::uint32_t n) {
  double s = 0.0;
  for (std::uint32_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / (n == 0 ? 1 : n));
}

// Deterministic white noise in [-amp, amp].
class Noise {
 public:
  explicit Noise(std::uint32_t seed = 1u) : s_(seed) {}
  float Next(float amp = 1.0f) {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return amp * (static_cast<float>(s_ >> 8) / 8388608.0f - 1.0f);
  }

 private:
  std::uint32_t s_;
};

inline snd::test::AudioBuffer ToBuffer(const Audio& a, std::uint32_t frames, std::uint32_t rate = 48000) {
  snd::test::AudioBuffer b(a.channels(), frames, rate);
  for (std::uint32_t c = 0; c < a.channels(); ++c) std::memcpy(b.Channel(c), a.ch(c), frames * sizeof(float));
  return b;
}

inline float MaxAbsDiff(const float* a, const float* b, std::uint32_t n) {
  float m = 0.0f;
  for (std::uint32_t i = 0; i < n; ++i) {
    const float d = std::fabs(a[i] - b[i]);
    m = d > m ? d : m;
  }
  return m;
}

}  // namespace dsptest

#endif  // SND_DSP_TEST_UTIL_HPP
