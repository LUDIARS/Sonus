// WAV (RIFF/WAVE) reading and writing: PCM 16/24/32-bit, IEEE float 32/64,
// WAVE_FORMAT_EXTENSIBLE; loop points from the 'smpl' chunk.
#ifndef SND_BUILDTOOL_WAV_HPP
#define SND_BUILDTOOL_WAV_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool {

struct PcmData {
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint64_t frames = 0;
  std::vector<float> samples;  // interleaved, frames * channels
  bool hasLoop = false;
  std::uint64_t loopStart = 0;  // first frame of the loop
  std::uint64_t loopEnd = 0;    // one past the last frame (exclusive)
};

// false and `error` on malformed or unsupported input.
bool ReadWav(const std::vector<std::uint8_t>& bytes, PcmData* out, std::string* error);

enum class WavSampleFormat : std::uint8_t { Int16, Int24, Int32, Float32 };
// Writes a canonical WAV (tests and tools). Loop -> 'smpl' chunk.
std::vector<std::uint8_t> WriteWav(const PcmData& pcm, WavSampleFormat format);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_WAV_HPP
