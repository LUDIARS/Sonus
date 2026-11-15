// Waveform conversion for the build: sample-rate and channel conversion,
// encoding through the EncoderRegistry and the differential build cache.
#ifndef SND_BUILDTOOL_SRC_CONVERT_HPP
#define SND_BUILDTOOL_SRC_CONVERT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "snd/buildtool/plugins.hpp"
#include "snd/buildtool/wav.hpp"

namespace snd::buildtool::detail {

struct ConvertSettings {
  std::uint32_t codec = 0;
  std::uint32_t sampleRate = 0;  // 0 = keep the source rate
  std::uint32_t channels = 0;    // 0 = keep the source channel count
  std::uint32_t seekIntervalMs = 0;
  bool loop = false;             // use loop points (source frames below)
  std::uint64_t loopStart = 0;
  std::uint64_t loopEnd = 0;
};

struct Converted {
  EncodedAsset enc;
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t sourceRate = 0;
  std::uint64_t loopStart = 0;  // output frames
  std::uint64_t loopEnd = 0;    // exclusive, 0 = no loop
};

// Linear-interpolation resampler (deterministic; quality adequate for stage 1).
std::vector<float> Resample(const std::vector<float>& in, std::uint32_t channels, std::uint64_t frames,
                            std::uint32_t from, std::uint32_t to, std::uint64_t* outFrames);
// Channel conversion: N -> N copy; 1 -> M duplicate; N -> 1 average; N -> M
// (M < N) keep the first M; N -> M (M > N) copy and zero the rest.
std::vector<float> ConvertChannels(const std::vector<float>& in, std::uint32_t from, std::uint32_t to,
                                   std::uint64_t frames);

// Cache key of one conversion (input bytes + settings + encoder version).
std::uint64_t CacheKey(const std::vector<std::uint8_t>& wavBytes, const ConvertSettings& s, std::uint32_t encoderVersion);
bool LoadCached(const std::string& cacheDir, std::uint64_t key, Converted* out);
void StoreCached(const std::string& cacheDir, std::uint64_t key, const Converted& c);

// Decode + convert + encode. `error` on failure.
bool ConvertWav(const std::vector<std::uint8_t>& wavBytes, const ConvertSettings& s, const Encoder& encoder,
                Converted* out, std::string* error);

}  // namespace snd::buildtool::detail

#endif  // SND_BUILDTOOL_SRC_CONVERT_HPP
