#include "windows/wasapi_format.hpp"

#include <cstring>

#include "common/stream_policy.hpp"
#include "windows/wasapi_guids.hpp"

namespace snd::platform::detail::wasapi {

WAVEFORMATEXTENSIBLE MakeFormat(std::uint32_t sampleRate, std::uint32_t channels, SampleFormat format,
                                std::uint16_t validBits) noexcept {
  WAVEFORMATEXTENSIBLE f;
  std::memset(&f, 0, sizeof(f));
  const std::uint16_t bits = static_cast<std::uint16_t>(BytesPerSample(format) * 8u);
  f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  f.Format.nChannels = static_cast<WORD>(channels);
  f.Format.nSamplesPerSec = sampleRate;
  f.Format.wBitsPerSample = bits;
  f.Format.nBlockAlign = static_cast<WORD>(channels * (bits / 8u));
  f.Format.nAvgBytesPerSec = sampleRate * f.Format.nBlockAlign;
  f.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));  // 22
  f.Samples.wValidBitsPerSample = validBits != 0u && validBits <= bits ? validBits : bits;
  f.dwChannelMask = SpeakerMaskForChannels(channels);
  f.SubFormat = format == SampleFormat::Float32 ? kSubtypeIeeeFloat : kSubtypePcm;
  return f;
}

}  // namespace snd::platform::detail::wasapi
