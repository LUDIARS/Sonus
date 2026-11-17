// Device sample formats and float32 conversion (CONTRACTS.md D-62).
// OS independent; used by backends whose device refuses float32 (ALSA hw:).
// RT-safe: no allocation, no lock.
#ifndef SND_PLATFORM_SRC_COMMON_SAMPLE_FORMAT_HPP
#define SND_PLATFORM_SRC_COMMON_SAMPLE_FORMAT_HPP

#include <cstddef>
#include <cstdint>

namespace snd::platform::detail {

enum class SampleFormat : std::uint8_t {
  Float32 = 0,  // native endian IEEE float, [-1, 1] (no conversion)
  Int32 = 1,    // native endian, full scale 2^31 - 1
  Int16 = 2,    // native endian, full scale 32767
};

// Preference order when the device lets us choose (D-62): float first so
// the device or the OS does the final conversion, then the widest integer.
inline constexpr SampleFormat kFormatPreference[] = {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int16};

constexpr std::uint32_t BytesPerSample(SampleFormat f) noexcept {
  return f == SampleFormat::Int16 ? 2u : 4u;
}
const char* SampleFormatName(SampleFormat f) noexcept;

// Converts `samples` floats to `format` in `dst` (BytesPerSample each),
// clipping to [-1, 1] and rounding to nearest. NaN becomes 0. Float32
// copies. dst and src must not overlap.
void ConvertFromFloat(const float* src, void* dst, std::size_t samples, SampleFormat format) noexcept;

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_COMMON_SAMPLE_FORMAT_HPP
