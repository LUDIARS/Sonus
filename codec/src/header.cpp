// Header validation and packet geometry of the built-in codecs.
#include <cstdint>

#include "detail.hpp"
#include "snd/codec/codec.hpp"

namespace snd::codec {

const char* HeaderErrorName(HeaderError e) noexcept {
  switch (e) {
    case HeaderError::None: return "none";
    case HeaderError::UnknownCodec: return "unknown codec";
    case HeaderError::CodecUnavailable: return "codec not available in this build (Opus: built with SND_CODEC_OPUS=OFF)";
    case HeaderError::BadChannels: return "bad channel count";
    case HeaderError::BadSampleRate: return "bad sample rate";
    case HeaderError::BadBlockAlign: return "bad block_align";
    case HeaderError::BadFramesPerPacket: return "frames_per_packet does not match block_align";
    case HeaderError::FrameCountOverflow: return "pre_skip + total_frames overflows";
    case HeaderError::BadCodecSetup: return "Opus setup data (extra) missing or invalid";
  }
  return "?";
}

bool OpusAvailable() noexcept { return SND_CODEC_OPUS_ENABLED != 0; }

bool IsBuiltinCodec(std::uint32_t fourcc) noexcept {
  return fourcc == SND_CODEC_PCM_F32 || fourcc == SND_CODEC_PCM_S16 || fourcc == SND_CODEC_ADPCM ||
         (fourcc == SND_CODEC_OPUS && OpusAvailable());
}

HeaderError ParseOpusSetup(const CodecHeader& h, OpusSetup* out) noexcept {
  if (h.channels == 0u || h.channels > SND_MAX_CHANNELS) return HeaderError::BadChannels;
  if (h.extra == nullptr || h.extra_bytes != kOpusSetupHeaderBytes + h.channels) return HeaderError::BadCodecSetup;
  const std::uint8_t* e = h.extra;
  const std::uint32_t streams = e[2];
  const std::uint32_t coupled = e[3];
  if (e[0] != kOpusSetupVersion || e[1] != h.channels) return HeaderError::BadCodecSetup;
  if (streams == 0u || streams > kOpusMaxStreams || coupled > streams) return HeaderError::BadCodecSetup;
  OpusSetup s{};
  s.streams = streams;
  s.coupledStreams = coupled;
  for (std::uint32_t c = 0; c < h.channels; ++c) {
    const std::uint32_t m = e[kOpusSetupHeaderBytes + c];
    if (m != 255u && m >= streams + coupled) return HeaderError::BadCodecSetup;
    s.mapping[c] = static_cast<std::uint8_t>(m);
  }
  if (out != nullptr) *out = s;
  return HeaderError::None;
}

std::uint32_t OpusMaxPacketBytes(const CodecHeader& h) noexcept {
  OpusSetup s;
  if (h.codec != SND_CODEC_OPUS || ParseOpusSetup(h, &s) != HeaderError::None) return 0;
  return s.streams * kOpusMaxPacketBytesPerStream;
}

namespace {

// Opus: 48 kHz, 20 ms packets, variable size (block_align 0), setup in extra.
HeaderError ValidateOpusHeader(const CodecHeader& h) noexcept {
  if (h.channels == 0u || h.channels > SND_MAX_CHANNELS) return HeaderError::BadChannels;
  if (h.sample_rate != kOpusSampleRate) return HeaderError::BadSampleRate;
  if (h.total_frames > UINT64_MAX - h.pre_skip) return HeaderError::FrameCountOverflow;
  if (h.block_align != 0u) return HeaderError::BadBlockAlign;
  if (h.frames_per_packet != 0u && h.frames_per_packet != kOpusPacketFrames) return HeaderError::BadFramesPerPacket;
  return ParseOpusSetup(h, nullptr);
}

}  // namespace

std::uint32_t AdpcmFramesPerBlock(std::uint32_t channels, std::uint32_t blockAlign) noexcept {
  if (channels == 0u || channels > SND_MAX_CHANNELS) return 0;
  const std::uint32_t head = 4u * channels;
  if (blockAlign <= head || blockAlign > kMaxAdpcmBlockBytes) return 0;
  if ((blockAlign - head) % head != 0u) return 0;
  return 1u + (blockAlign - head) / head * 8u;
}

HeaderError ValidateHeader(const CodecHeader& h) noexcept {
  if (h.codec == SND_CODEC_OPUS) return OpusAvailable() ? ValidateOpusHeader(h) : HeaderError::CodecUnavailable;
  if (!IsBuiltinCodec(h.codec)) return HeaderError::UnknownCodec;
  if (h.channels == 0u || h.channels > SND_MAX_CHANNELS) return HeaderError::BadChannels;
  if (h.sample_rate == 0u) return HeaderError::BadSampleRate;
  if (h.total_frames > UINT64_MAX - h.pre_skip) return HeaderError::FrameCountOverflow;
  if (h.codec == SND_CODEC_ADPCM) {
    const std::uint32_t fpb = AdpcmFramesPerBlock(h.channels, h.block_align);
    if (fpb == 0u) return HeaderError::BadBlockAlign;
    if (h.frames_per_packet != 0u && h.frames_per_packet != fpb) return HeaderError::BadFramesPerPacket;
    return HeaderError::None;
  }
  // PCM: one packet is one frame. frames_per_packet is 1 (0 = not given, the
  // bank column's default); block_align is the frame size (0 = not given).
  if (h.frames_per_packet != 0u && h.frames_per_packet != 1u) return HeaderError::BadFramesPerPacket;
  const std::uint32_t frameBytes = h.channels * (h.codec == SND_CODEC_PCM_F32 ? 4u : 2u);
  if (h.block_align != 0u && h.block_align != frameBytes) return HeaderError::BadBlockAlign;
  return HeaderError::None;
}

std::uint32_t PacketBytes(const CodecHeader& h) noexcept {
  if (ValidateHeader(h) != HeaderError::None) return 0;
  if (h.codec == SND_CODEC_OPUS) return 0;  // variable size
  if (h.codec == SND_CODEC_ADPCM) return h.block_align;
  return h.channels * (h.codec == SND_CODEC_PCM_F32 ? 4u : 2u);
}

std::uint32_t PacketFrames(const CodecHeader& h) noexcept {
  if (ValidateHeader(h) != HeaderError::None) return 0;
  if (h.codec == SND_CODEC_OPUS) return kOpusPacketFrames;
  if (h.codec == SND_CODEC_ADPCM) return AdpcmFramesPerBlock(h.channels, h.block_align);
  return 1u;
}

std::uint32_t HoldCapacityBytes(const CodecHeader& h) noexcept {
  if (h.codec == SND_CODEC_OPUS) {
    if (ValidateHeader(h) != HeaderError::None) return 0;
    return kHoldInputBytes + kOpusLengthPrefixBytes + OpusMaxPacketBytes(h);
  }
  const std::uint32_t packet = PacketBytes(h);
  if (packet == 0u) return 0;
  return kHoldInputBytes + packet;
}

bool PacketForFrame(const CodecHeader& h, std::uint64_t target, PacketPosition* out) noexcept {
  if (out == nullptr) return false;
  const std::uint32_t pb = PacketBytes(h);
  const std::uint32_t pf = PacketFrames(h);
  if (pb == 0u || pf == 0u) return false;
  if (target >= h.total_frames) return false;  // past the playable range
  // ValidateHeader guarantees pre_skip + total_frames fits, so d does not overflow.
  const std::uint64_t d = target + h.pre_skip;
  const std::uint64_t packet = d / pf;
  const std::uint64_t start = packet * pf;  // <= d
  if (start <= h.pre_skip) {
    *out = PacketPosition{0, 0};
    return true;
  }
  if (packet > UINT64_MAX / pb) return false;  // byte offset not representable
  *out = PacketPosition{start - h.pre_skip, packet * pb};
  return true;
}

std::uint32_t FindSeekPoint(const std::uint64_t* frames, std::uint32_t count, std::uint64_t target,
                            std::uint32_t preRoll) noexcept {
  if (frames == nullptr || count == 0u) return kNoSeekPoint;
  const std::uint64_t t = target > preRoll ? target - preRoll : 0u;
  // Last index with frames[i] <= t (entry 0 is the stream start).
  std::uint32_t lo = 0, hi = count;  // answer in [lo, hi)
  while (hi - lo > 1u) {
    const std::uint32_t mid = lo + (hi - lo) / 2u;
    if (frames[mid] <= t) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

std::uint32_t FindSeekPoint(const std::uint64_t* frames, std::uint32_t count, std::uint64_t target,
                            std::uint32_t preRoll, std::uint64_t totalFrames) noexcept {
  if (target >= totalFrames) return kNoSeekPoint;
  return FindSeekPoint(frames, count, target, preRoll);
}

}  // namespace snd::codec
