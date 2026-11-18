// ALSA PCM opening and hw/sw parameter negotiation (CONTRACTS.md D-62).
// Non-RT (owner thread at Open, device thread when re-opening after a loss).
#ifndef SND_PLATFORM_SRC_LINUX_ALSA_PCM_HPP
#define SND_PLATFORM_SRC_LINUX_ALSA_PCM_HPP

#include <alsa/asoundlib.h>

#include <cstdint>

#include "common/sample_format.hpp"

namespace snd::platform::detail {

struct AlsaRequest {
  std::uint32_t sampleRate = 0;  // 0 = 48000
  std::uint32_t channels = 0;    // 0 = 2
  std::uint32_t periodFrames = 0;  // 0 = DefaultPeriodFrames(rate)
  std::uint32_t periods = 3;     // periods in the buffer
};

struct AlsaStream {
  std::uint32_t sampleRate = 0;
  std::uint32_t channels = 0;
  std::uint32_t periodFrames = 0;
  std::uint32_t bufferFrames = 0;
  SampleFormat format = SampleFormat::Float32;
};

// Opens `name` for playback, non-blocking. With `configText` the PCM is
// looked up in that configuration only (snd_pcm_open_lconf) instead of the
// system one. Returns 0 or a negative errno.
int OpenAlsaPcm(const char* name, const char* configText, snd_pcm_t** out) noexcept;

// Negotiates interleaved access, the first format of kFormatPreference the
// PCM accepts, the nearest channel count / rate (ALSA resampling allowed for
// plug devices) / period / buffer, and the software parameters
// (start when the buffer is full, wake at one period). Returns 0 or a
// negative errno; `out` holds what the device accepted.
int ConfigureAlsaPcm(snd_pcm_t* pcm, const AlsaRequest& request, AlsaStream* out) noexcept;

}  // namespace snd::platform::detail

#endif  // SND_PLATFORM_SRC_LINUX_ALSA_PCM_HPP
