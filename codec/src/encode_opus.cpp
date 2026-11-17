// Opus encoder and seek table (build time only; snd/codec/encode.hpp).
// Byte layout: bank-format.md §6.5 (u16 LE length + multistream packet, 20 ms
// each; setup data in extra). Decoder: opus_decoder.cpp.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "snd/base/endian.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"

#if SND_CODEC_OPUS_ENABLED
#include <cstdlib>
#include <vector>

#include <opus_multistream.h>
#endif

namespace snd::codec::encode {

bool OpusEncoderAvailable() noexcept { return SND_CODEC_OPUS_ENABLED != 0; }

std::uint64_t OpusMaxEncodedBytes(std::uint32_t channels, std::uint64_t frames) noexcept {
  if (channels == 0u || channels > SND_MAX_CHANNELS) return 0;
  // Streams of the surround layouts are at most `channels`; the encoder
  // lookahead (pre_skip, 312) is at most one packet (checked by EncodeOpus).
  if (frames > UINT64_MAX / 2u) return 0;
  const std::uint64_t decoded = frames + kOpusPacketFrames;
  const std::uint64_t packets = decoded / kOpusPacketFrames + 1u;
  return packets * (kOpusLengthPrefixBytes + static_cast<std::uint64_t>(channels) * kOpusMaxPacketBytesPerStream);
}

std::uint32_t BuildOpusSeekTable(const CodecHeader& h, const std::uint8_t* data, std::uint64_t dataBytes,
                                 std::uint64_t intervalFrames, std::uint64_t* frames, std::uint64_t* byteOffsets,
                                 std::uint32_t capacity) noexcept {
  if (h.codec != SND_CODEC_OPUS || ValidateHeader(h) != HeaderError::None) return 0;
  if (data == nullptr && dataBytes != 0u) return 0;
  const std::uint32_t maxPacket = OpusMaxPacketBytes(h);
  const bool write = frames != nullptr && byteOffsets != nullptr;
  std::uint32_t n = 0;
  auto put = [&](std::uint64_t f, std::uint64_t o) {
    if (write && n < capacity) {
      frames[n] = f;
      byteOffsets[n] = o;
    }
    ++n;
  };
  put(0, 0);
  std::uint64_t last = 0;
  std::uint64_t decoded = 0;  // first decoded frame of the packet at `off`
  std::uint64_t off = 0;
  while (off < dataBytes) {
    if (dataBytes - off < kOpusLengthPrefixBytes) return 0;
    const std::uint32_t len = base::LoadLE16(data + off);
    if (len == 0u || len > maxPacket || dataBytes - off - kOpusLengthPrefixBytes < len) return 0;
    if (decoded > h.pre_skip) {
      const std::uint64_t f = decoded - h.pre_skip;
      if (f < h.total_frames && f - last >= intervalFrames) {
        put(f, off);
        last = f;
      }
    }
    decoded += kOpusPacketFrames;
    off += kOpusLengthPrefixBytes + len;
  }
  return n;
}

#if SND_CODEC_OPUS_ENABLED

namespace {

// WAV channel index -> Vorbis channel index (RFC 7845 5.1.1.2), per channel count.
// WAV: L R C LFE BL BR SL SR (BC in place of BL for 7 ch: L R C LFE BC SL SR).
// Vorbis: 3 L C R; 4 L R BL BR; 5 L C R BL BR; 6 L C R BL BR LFE;
//         7 L C R SL SR BC LFE; 8 L C R SL SR BL BR LFE.
constexpr std::uint8_t kWavToVorbis[SND_MAX_CHANNELS + 1][SND_MAX_CHANNELS] = {
    {},
    {0},
    {0, 1},
    {0, 2, 1},
    {0, 1, 2, 3},
    {0, 2, 1, 3, 4},
    {0, 2, 1, 5, 3, 4},
    {0, 2, 1, 6, 5, 3, 4},
    {0, 2, 1, 7, 5, 6, 3, 4},
};

struct EncoderDeleter {
  OpusMSEncoder* p;
  ~EncoderDeleter() {
    if (p != nullptr) opus_multistream_encoder_destroy(p);
  }
};

}  // namespace

EncodeStatus EncodeOpus(const OpusSettings& st, const float* const* src, std::uint32_t channels,
                        std::uint32_t sampleRate, std::uint64_t frames, std::uint8_t* dst, std::uint64_t dstBytes,
                        OpusEncoded* out) noexcept {
  if (out == nullptr || channels == 0u || channels > SND_MAX_CHANNELS || sampleRate != kOpusSampleRate) {
    return EncodeStatus::BadArguments;
  }
  std::memset(out, 0, sizeof(*out));
  if (frames > 0u) {
    if (src == nullptr) return EncodeStatus::BadArguments;
    for (std::uint32_t c = 0; c < channels; ++c) {
      if (src[c] == nullptr) return EncodeStatus::BadArguments;
      for (std::uint64_t i = 0; i < frames; ++i) {
        if (!std::isfinite(src[c][i])) return EncodeStatus::NonFiniteInput;
      }
    }
  }
  const std::uint64_t bitrate =
      st.bitrate == 0u ? std::uint64_t{kOpusDefaultBitratePerChannel} * channels : std::uint64_t{st.bitrate};
  if (bitrate < kOpusMinBitrate || bitrate > std::uint64_t{kOpusMaxBitratePerChannel} * channels ||
      st.complexity > 10u || static_cast<std::uint32_t>(st.vbr) > 2u || static_cast<std::uint32_t>(st.signal) > 2u) {
    return EncodeStatus::BadSettings;
  }
  const std::uint64_t need = OpusMaxEncodedBytes(channels, frames);
  if (need == 0u) return EncodeStatus::BadArguments;
  if (dst == nullptr || dstBytes < need) return EncodeStatus::BufferTooSmall;

  int streams = 0, coupled = 0, err = OPUS_OK;
  unsigned char vorbisMapping[SND_MAX_CHANNELS] = {};
  EncoderDeleter enc{opus_multistream_surround_encoder_create(
      static_cast<opus_int32>(kOpusSampleRate), static_cast<int>(channels), channels <= 2u ? 0 : 1, &streams,
      &coupled, vorbisMapping, OPUS_APPLICATION_AUDIO, &err)};
  if (enc.p == nullptr || err != OPUS_OK) return EncodeStatus::EncoderFailed;
  const int signal = st.signal == OpusSignal::Music ? OPUS_SIGNAL_MUSIC
                     : st.signal == OpusSignal::Voice ? OPUS_SIGNAL_VOICE
                                                      : OPUS_AUTO;
  opus_int32 lookahead = 0;
  if (opus_multistream_encoder_ctl(enc.p, OPUS_SET_BITRATE(static_cast<opus_int32>(bitrate))) != OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_SET_COMPLEXITY(static_cast<opus_int32>(st.complexity))) != OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_SET_VBR(st.vbr == OpusVbr::Cbr ? 0 : 1)) != OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_SET_VBR_CONSTRAINT(st.vbr == OpusVbr::ConstrainedVbr ? 1 : 0)) !=
          OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_SET_SIGNAL(signal)) != OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_SET_PREDICTION_DISABLED(st.independentFrames ? 1 : 0)) != OPUS_OK ||
      opus_multistream_encoder_ctl(enc.p, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0 ||
      lookahead > static_cast<opus_int32>(kOpusPacketFrames)) {
    return EncodeStatus::EncoderFailed;
  }
  if (streams <= 0 || static_cast<std::uint32_t>(streams) > kOpusMaxStreams || coupled < 0 || coupled > streams) {
    return EncodeStatus::EncoderFailed;
  }

  // Setup data: mapping in WAV channel order (the decoder outputs WAV order).
  out->setup[0] = static_cast<std::uint8_t>(kOpusSetupVersion);
  out->setup[1] = static_cast<std::uint8_t>(channels);
  out->setup[2] = static_cast<std::uint8_t>(streams);
  out->setup[3] = static_cast<std::uint8_t>(coupled);
  for (std::uint32_t c = 0; c < channels; ++c) {
    out->setup[kOpusSetupHeaderBytes + c] = vorbisMapping[kWavToVorbis[channels][c]];
  }
  out->setupBytes = kOpusSetupHeaderBytes + channels;

  CodecHeader& h = out->header;
  h.codec = SND_CODEC_OPUS;
  h.channels = channels;
  h.sample_rate = kOpusSampleRate;
  h.frames_per_packet = kOpusPacketFrames;
  h.total_frames = frames;
  h.pre_skip = static_cast<std::uint32_t>(lookahead);
  h.pre_roll = kOpusPreRollFrames;
  h.block_align = 0;
  h.extra_bytes = out->setupBytes;
  h.extra = out->setup;

  const std::uint64_t decoded = frames + h.pre_skip;
  const std::uint64_t packets = (decoded + kOpusPacketFrames - 1u) / kOpusPacketFrames;
  const std::uint32_t maxPacket = static_cast<std::uint32_t>(streams) * kOpusMaxPacketBytesPerStream;
  std::vector<float> block(static_cast<std::size_t>(kOpusPacketFrames) * channels);
  std::uint64_t off = 0;
  for (std::uint64_t p = 0; p < packets; ++p) {
    const std::uint64_t first = p * kOpusPacketFrames;
    for (std::uint32_t f = 0; f < kOpusPacketFrames; ++f) {
      const std::uint64_t i = first + f;
      for (std::uint32_t c = 0; c < channels; ++c) {
        block[static_cast<std::size_t>(f) * channels + kWavToVorbis[channels][c]] = i < frames ? src[c][i] : 0.0f;
      }
    }
    const opus_int32 n = opus_multistream_encode_float(enc.p, block.data(), static_cast<int>(kOpusPacketFrames),
                                                       dst + off + kOpusLengthPrefixBytes,
                                                       static_cast<opus_int32>(maxPacket));
    if (n <= 0 || static_cast<std::uint32_t>(n) > maxPacket) return EncodeStatus::EncoderFailed;
    base::StoreLE16(dst + off, static_cast<std::uint16_t>(n));
    off += kOpusLengthPrefixBytes + static_cast<std::uint64_t>(n);
  }
  out->packets = packets;
  out->bytes = off;
  return EncodeStatus::Ok;
}

#else  // !SND_CODEC_OPUS_ENABLED

EncodeStatus EncodeOpus(const OpusSettings&, const float* const*, std::uint32_t, std::uint32_t, std::uint64_t,
                        std::uint8_t*, std::uint64_t, OpusEncoded* out) noexcept {
  if (out != nullptr) std::memset(out, 0, sizeof(*out));
  return EncodeStatus::CodecUnavailable;
}

#endif

}  // namespace snd::codec::encode
