// Plug-in points of the build (design.md 10.3, 3.3):
//   - EncoderRegistry: waveform encoders by codec fourcc, all from the codec
//     module (A1-11). A default-constructed registry (and BuildOptions with
//     encoders == nullptr) has PCM float32 (PCMF) and PCM s16 (PC16);
//     CodecEncoders() / RegisterCodecEncoders() add IMA ADPCM (ADPC,
//     kAdpcmBlockBytesPerChannel bytes per channel and block) and, when the
//     build has Opus (SND_CODEC_OPUS, CONTRACTS.md D-56), Opus (OPUS).
//     sonus-build uses CodecEncoders() by default. Custom codecs come through
//     Register(). A codec without an encoder is a build error naming the codec.
//   - DspSizer: per-effect instance state size for EFFX.stateBytes / DSPN.
//     The default is the dsp module's sizing (dsp::EffectInstanceBytes, the
//     same function the runtime's ComputeDspPoolNeed uses).
#ifndef SND_BUILDTOOL_PLUGINS_HPP
#define SND_BUILDTOOL_PLUGINS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace snd::buildtool {

// Opus settings of one asset (project-format.md "convert" / "platforms":
// "opus": {bitrate, complexity, vbr, signal, independentFrames}). Encoders of
// other codecs ignore them. Values map to snd::codec::encode::OpusSettings.
struct OpusOptions {
  std::uint32_t bitrate = 0;      // bit/s for all channels; 0 = 64000 x channels
  std::uint32_t complexity = 10;  // 0..10
  std::uint32_t vbr = 0;          // 0 vbr, 1 constrained vbr, 2 cbr
  std::uint32_t signal = 0;       // 0 auto, 1 music, 2 voice
  bool independentFrames = true;  // sample-exact seeks (no prediction across packets)
};

struct EncodeInput {
  const float* samples = nullptr;  // interleaved
  std::uint64_t frames = 0;
  std::uint32_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t seekIntervalFrames = 0;  // desired spacing of seek points (0 = none needed)
  std::uint64_t loopStart = 0;            // loop (0/0 = none); encoders may align packets to it
  std::uint64_t loopEnd = 0;
  OpusOptions opus;                       // used by the Opus encoder only
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

// ADPCM block size the build writes: 512 bytes per channel (1017 frames).
// bank-format.md §6.5: at most 1024 bytes per channel, so that the decoder
// state fits the default Config.memory.decoderSlotBytes (32 KiB) up to 8 ch.
inline constexpr std::uint32_t kAdpcmBlockBytesPerChannel = 512;
inline constexpr std::uint32_t kMaxAdpcmBlockBytesPerChannel = 1024;

class EncoderRegistry {
 public:
  // With the codec module's PCMF and PC16 encoders.
  EncoderRegistry();
  void Register(const Encoder& e);  // replaces an encoder of the same codec
  const Encoder* Find(std::uint32_t codec) const;

 private:
  std::vector<Encoder> encoders_;
};

// Registers the codec module's PCMF, PC16 and ADPC encoders (replacing those
// codecs), and OPUS when snd::codec::encode::OpusEncoderAvailable(). The Opus
// encoder's version is snd::codec::encode::kOpusEncoderVersion.
void RegisterCodecEncoders(EncoderRegistry& registry);
// A registry with every codec module encoder (sonus-build's default).
const EncoderRegistry& CodecEncoders();

struct DspSizer {
  void* user = nullptr;
  // Instance state bytes of one effect (EFFX.stateBytes).
  std::uint32_t (*stateBytes)(void* user, std::uint32_t effectType, const float* params,
                              std::uint32_t paramCount, std::uint32_t channels) = nullptr;
};

// Codec name <-> fourcc ("PCMF", "PC16", "ADPC", "OPUS" or any 4 characters).
bool ParseCodec(const std::string& name, std::uint32_t* fourcc);
std::string CodecName(std::uint32_t fourcc);

}  // namespace snd::buildtool

#endif  // SND_BUILDTOOL_PLUGINS_HPP
