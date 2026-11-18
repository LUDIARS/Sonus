// WAVEFORMATEXTENSIBLE construction for WASAPI (D-63). Non-RT.
#ifndef SND_PLATFORM_SRC_WINDOWS_WASAPI_FORMAT_HPP
#define SND_PLATFORM_SRC_WINDOWS_WASAPI_FORMAT_HPP

#include <cstdint>

#include "common/sample_format.hpp"
#include "windows/windows_headers.hpp"

namespace snd::platform::detail::wasapi {

// Interleaved stream format. Float32 = IEEE float 32/32; Int32 = PCM 32/32;
// Int16 = PCM 16/16. `validBits` < container (e.g. 24 in 32) for Int32 only.
WAVEFORMATEXTENSIBLE MakeFormat(std::uint32_t sampleRate, std::uint32_t channels, SampleFormat format,
                                std::uint16_t validBits = 0) noexcept;

// Exclusive-mode formats tried in order (D-63): float32, int32, int24-in-32, int16.
struct ExclusiveCandidate {
  SampleFormat format;
  std::uint16_t validBits;
};
inline constexpr ExclusiveCandidate kExclusiveCandidates[] = {
    {SampleFormat::Float32, 32}, {SampleFormat::Int32, 32}, {SampleFormat::Int32, 24}, {SampleFormat::Int16, 16}};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_WASAPI_FORMAT_HPP
