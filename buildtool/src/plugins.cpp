#include "snd/buildtool/plugins.hpp"

#include <cmath>
#include <cstring>

#include "snd/ext.h"

namespace snd::buildtool {
namespace {

void PcmSeek(const EncodeInput& in, std::uint32_t frameBytes, EncodedAsset* out) {
  if (in.seekIntervalFrames == 0) return;
  for (std::uint64_t f = 0; f < in.frames; f += in.seekIntervalFrames) {
    out->seek.push_back(SeekPoint{f, f * frameBytes});
  }
}

// PCMF: interleaved little-endian float32, one frame per packet.
bool EncodePcmF32(const EncodeInput& in, EncodedAsset* out, std::string*) {
  out->codec = SND_CODEC_PCM_F32;
  out->data.resize(static_cast<std::size_t>(in.frames * in.channels * 4));
  if (!out->data.empty()) std::memcpy(out->data.data(), in.samples, out->data.size());
  out->framesPerPacket = 1;
  out->blockAlign = in.channels * 4;
  out->totalFrames = in.frames;
  PcmSeek(in, out->blockAlign, out);
  return true;
}

// PC16: interleaved little-endian int16 (round to nearest, clamped).
bool EncodePcmS16(const EncodeInput& in, EncodedAsset* out, std::string*) {
  out->codec = SND_CODEC_PCM_S16;
  const std::uint64_t n = in.frames * in.channels;
  out->data.resize(static_cast<std::size_t>(n * 2));
  for (std::uint64_t i = 0; i < n; ++i) {
    float v = in.samples[i];
    v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    const long s = std::lround(static_cast<double>(v) * 32767.0);
    const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(s));
    out->data[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>(u);
    out->data[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::uint8_t>(u >> 8);
  }
  out->framesPerPacket = 1;
  out->blockAlign = in.channels * 2;
  out->totalFrames = in.frames;
  PcmSeek(in, out->blockAlign, out);
  return true;
}

}  // namespace

EncoderRegistry::EncoderRegistry() {
  encoders_.push_back(Encoder{SND_CODEC_PCM_F32, "PCMF", 1, &EncodePcmF32});
  encoders_.push_back(Encoder{SND_CODEC_PCM_S16, "PC16", 1, &EncodePcmS16});
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
