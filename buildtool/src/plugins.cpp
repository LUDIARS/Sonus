#include "snd/buildtool/plugins.hpp"

#include <vector>

#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/ext.h"

namespace snd::buildtool {
namespace {

// Encoding through the codec module (A1-11): the bytes are exactly what the
// runtime's built-in decoders read (bank-format.md §6.5, codec/README.md).
static_assert(kAdpcmBlockBytesPerChannel <= kMaxAdpcmBlockBytesPerChannel);
static_assert(kAdpcmBlockBytesPerChannel == codec::encode::kDefaultAdpcmBlockBytesPerChannel);

bool EncodeWithCodec(const EncodeInput& in, std::uint32_t fourcc, EncodedAsset* out, std::string* error) {
  namespace ce = codec::encode;
  const CodecHeader h = fourcc == SND_CODEC_ADPCM
                            ? ce::MakeAdpcmHeader(in.channels, in.sampleRate, in.frames, kAdpcmBlockBytesPerChannel)
                            : ce::MakePcmHeader(fourcc, in.channels, in.sampleRate, in.frames);
  if (const codec::HeaderError he = codec::ValidateHeader(h); he != codec::HeaderError::None) {
    *error = std::string("codec header rejected: ") + codec::HeaderErrorName(he);
    return false;
  }
  // The codec encoders take non-interleaved channels.
  std::vector<std::vector<float>> planar(in.channels, std::vector<float>(static_cast<std::size_t>(in.frames)));
  std::vector<const float*> ptr(in.channels);
  for (std::uint32_t c = 0; c < in.channels; ++c) {
    for (std::uint64_t f = 0; f < in.frames; ++f) {
      planar[c][static_cast<std::size_t>(f)] = in.samples[f * in.channels + c];
    }
    ptr[c] = planar[c].data();
  }
  out->codec = fourcc;
  out->data.assign(static_cast<std::size_t>(ce::EncodedBytes(h)), 0);
  std::uint64_t written = 0;
  const ce::EncodeStatus st = ce::Encode(h, ptr.data(), in.frames, out->data.data(), out->data.size(), &written);
  if (st != ce::EncodeStatus::Ok) {
    *error = std::string("encoding failed: ") + ce::EncodeStatusName(st);
    return false;
  }
  out->data.resize(static_cast<std::size_t>(written));
  out->framesPerPacket = h.frames_per_packet;
  out->blockAlign = h.block_align;
  out->preSkip = h.pre_skip;
  out->preRoll = h.pre_roll;
  out->totalFrames = h.total_frames;
  out->seek.clear();
  if (in.seekIntervalFrames != 0) {  // 0: no seek points needed (resident, no seeking)
    const std::uint32_t n = ce::BuildSeekTable(h, in.seekIntervalFrames, nullptr, nullptr, 0);
    std::vector<std::uint64_t> frames(n), offsets(n);
    ce::BuildSeekTable(h, in.seekIntervalFrames, frames.data(), offsets.data(), n);
    for (std::uint32_t i = 0; i < n; ++i) out->seek.push_back(SeekPoint{frames[i], offsets[i]});
  }
  return true;
}

bool EncodePcmF32(const EncodeInput& in, EncodedAsset* out, std::string* error) {
  return EncodeWithCodec(in, SND_CODEC_PCM_F32, out, error);
}
bool EncodePcmS16(const EncodeInput& in, EncodedAsset* out, std::string* error) {
  return EncodeWithCodec(in, SND_CODEC_PCM_S16, out, error);
}
bool EncodeAdpcm(const EncodeInput& in, EncodedAsset* out, std::string* error) {
  return EncodeWithCodec(in, SND_CODEC_ADPCM, out, error);
}

// Opus: variable-size packets (bank-format.md §6.5). Always 48 kHz: the build
// converts other rates before encoding (Builder::ConvertAsset).
bool EncodeOpusAsset(const EncodeInput& in, EncodedAsset* out, std::string* error) {
  namespace ce = codec::encode;
  if (in.sampleRate != codec::kOpusSampleRate) {
    *error = "Opus assets must be " + std::to_string(codec::kOpusSampleRate) + " Hz (got " +
             std::to_string(in.sampleRate) + ")";
    return false;
  }
  ce::OpusSettings st;
  st.bitrate = in.opus.bitrate;
  st.complexity = in.opus.complexity;
  st.vbr = static_cast<ce::OpusVbr>(in.opus.vbr);
  st.signal = static_cast<ce::OpusSignal>(in.opus.signal);
  st.independentFrames = in.opus.independentFrames;
  std::vector<std::vector<float>> planar(in.channels, std::vector<float>(static_cast<std::size_t>(in.frames)));
  std::vector<const float*> ptr(in.channels);
  for (std::uint32_t c = 0; c < in.channels; ++c) {
    for (std::uint64_t f = 0; f < in.frames; ++f) {
      planar[c][static_cast<std::size_t>(f)] = in.samples[f * in.channels + c];
    }
    ptr[c] = planar[c].data();
  }
  out->codec = SND_CODEC_OPUS;
  out->data.assign(static_cast<std::size_t>(ce::OpusMaxEncodedBytes(in.channels, in.frames)), 0);
  ce::OpusEncoded e{};
  const ce::EncodeStatus st2 =
      ce::EncodeOpus(st, ptr.data(), in.channels, in.sampleRate, in.frames, out->data.data(), out->data.size(), &e);
  if (st2 != ce::EncodeStatus::Ok) {
    *error = std::string("Opus encoding failed: ") + ce::EncodeStatusName(st2);
    return false;
  }
  out->data.resize(static_cast<std::size_t>(e.bytes));
  out->extra.assign(e.setup, e.setup + e.setupBytes);
  out->framesPerPacket = e.header.frames_per_packet;
  out->blockAlign = 0;
  out->preSkip = e.header.pre_skip;
  out->preRoll = e.header.pre_roll;
  out->totalFrames = e.header.total_frames;
  out->seek.clear();
  if (in.seekIntervalFrames != 0) {
    CodecHeader h = e.header;
    h.extra = out->extra.data();
    const std::uint32_t n =
        ce::BuildOpusSeekTable(h, out->data.data(), out->data.size(), in.seekIntervalFrames, nullptr, nullptr, 0);
    std::vector<std::uint64_t> frames(n), offsets(n);
    ce::BuildOpusSeekTable(h, out->data.data(), out->data.size(), in.seekIntervalFrames, frames.data(),
                           offsets.data(), n);
    for (std::uint32_t i = 0; i < n; ++i) out->seek.push_back(SeekPoint{frames[i], offsets[i]});
  }
  return true;
}

}  // namespace

EncoderRegistry::EncoderRegistry() {
  // Versions are part of the differential-build cache key: 2 = codec module encoders.
  encoders_.push_back(Encoder{SND_CODEC_PCM_F32, "PCMF", 2, &EncodePcmF32});
  encoders_.push_back(Encoder{SND_CODEC_PCM_S16, "PC16", 2, &EncodePcmS16});
}

void RegisterCodecEncoders(EncoderRegistry& registry) {
  registry.Register(Encoder{SND_CODEC_PCM_F32, "PCMF", 2, &EncodePcmF32});
  registry.Register(Encoder{SND_CODEC_PCM_S16, "PC16", 2, &EncodePcmS16});
  registry.Register(Encoder{SND_CODEC_ADPCM, "ADPC", 2, &EncodeAdpcm});
  if (codec::encode::OpusEncoderAvailable()) {
    registry.Register(Encoder{SND_CODEC_OPUS, "OPUS", codec::encode::kOpusEncoderVersion, &EncodeOpusAsset});
  }
}

const EncoderRegistry& CodecEncoders() {
  static const EncoderRegistry registry = [] {
    EncoderRegistry r;
    RegisterCodecEncoders(r);
    return r;
  }();
  return registry;
}

void EncoderRegistry::Register(const Encoder& e) {
  for (Encoder& x : encoders_) {
    if (x.codec == e.codec) {
      x = e;
      return;
    }
  }
  encoders_.push_back(e);
}

const Encoder* EncoderRegistry::Find(std::uint32_t codec) const {
  for (const Encoder& x : encoders_) {
    if (x.codec == codec && x.encode != nullptr) return &x;
  }
  return nullptr;
}

bool ParseCodec(const std::string& name, std::uint32_t* fourcc) {
  if (name.size() != 4) return false;
  *fourcc = SND_FOURCC(name[0], name[1], name[2], name[3]);
  return true;
}

std::string CodecName(std::uint32_t f) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(i)] = static_cast<char>((f >> (8 * i)) & 0xFF);
  return s;
}

}  // namespace snd::buildtool
