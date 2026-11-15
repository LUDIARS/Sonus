// Plug-in points of the build (design.md 10.3, 3.3):
//   - EncoderRegistry: waveform encoders by codec fourcc. PCM float32 (PCMF)
//     and PCM s16 (PC16) are built in; ADPCM / Opus come from the codec
//     module's encoders (stage 1, other team) through Register(). A codec
//     without an encoder is a build error naming the codec.
//   - DspSizer: per-effect instance state size for EFFX.stateBytes / DSPN
//     (the dsp module will provide it). Without one, a provisional fixed size
//     is used and the build warns once.
#ifndef SND_BUILDTOOL_PLUGINS_HPP
#define SND_BUILDTOOL_PLUGINS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool {

struct EncodeInput {
  const float* samples = nullptr;  // interleaved
  std::uint64_t frames = 0;
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t seekIntervalFrames = 0;  // desired spacing of seek points (0 = none needed)
  std::uint64_t loopStart = 0;            // loop (0/0 = none); encoders may align packets to it
  std::uint64_t loopEnd = 0;
};

struct SeekPoint {
  std::uint64_t frame = 0;
  std::uint64_t byteOffset = 0;  // from the start of the encoded data
};

struct EncodedAsset {
  std::uint32_t codec = 0;
  std::vector<std::uint8_t> data;
  std::vector<std::uint8_t> extra;  // codec setup data (ASST.extra*)
  std::uint32_t framesPerPacket = 0;  // 0 = variable
  std::uint32_t blockAlign = 0;       // bytes per packet for fixed-size codecs
  std::uint32_t preSkip = 0;
  std::uint32_t preRoll = 0;
  std::uint64_t totalFrames = 0;      // playable frames
  std::vector<SeekPoint> seek;        // packet boundaries usable for seeking / preload cuts
};

struct Encoder {
  std::uint32_t codec = 0;    // SND_CODEC_* fourcc
  const char* name = "";
  std::uint32_t version = 1;  // part of the differential-build cache key
  // Returns false and sets `error` on failure. Must be deterministic.
  bool (*encode)(const EncodeInput& in, EncodedAsset* out, std::string* error) = nullptr;
};

class EncoderRegistry {
 public:
  // With the built-in PCMF and PC16 encoders.
  EncoderRegistry();
  void Register(const Encoder& e);  // replaces an encoder of the same codec
  const Encoder* Find(std::uint32_t codec) const;

 private:
  std::vector<Encoder> encoders_;
};

struct DspSizer {
  void* user = nullptr;
  // Instance state bytes of one effect (EFFX.stateBytes).
  std::uint32_t (*stateBytes)(void* user, std::uint32_t effectType, const float* params,
                              std::uint32_t paramCount, std::uint32_t channels) = nullptr;
};
inline constexpr std::uint32_t kProvisionalEffectStateBytes = 16384;

// Codec name <-> fourcc ("PCMF", "PC16", "ADPC", "OPUS" or any 4 characters).
bool ParseCodec(const std::string& name, std::uint32_t* fourcc);
std::string CodecName(std::uint32_t fourcc);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_PLUGINS_HPP
