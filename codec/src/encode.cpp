// Build-time encoders and helpers (snd/codec/encode.hpp). Not referenced by the
// runtime decoders, so a runtime binary does not link this object.
#include "snd/codec/encode.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "detail.hpp"
#include "snd/codec/codec.hpp"

namespace snd::codec::encode {
namespace {

std::int32_t ToS16(float x) noexcept {
  const float v = x * 32768.0f;
  if (v >= 32767.0f) return 32767;
  if (v <= -32768.0f) return -32768;
  return static_cast<std::int32_t>(std::lrint(v));
}

std::uint32_t ImaNibble(std::int32_t predictor, std::int32_t index, std::int32_t sample) noexcept {
  std::int32_t step = detail::kImaStep[index];
  std::int32_t diff = sample - predictor;
  std::uint32_t n = 0;
  if (diff < 0) {
    n = 8u;
    diff = -diff;
  }
  if (diff >= step) {
    n |= 4u;
    diff -= step;
  }
  step >>= 1;
  if (diff >= step) {
    n |= 2u;
    diff -= step;
  }
  step >>= 1;
  if (diff >= step) n |= 1u;
  return n;
}

// One channel of one block, read straight from the float input. Frames after
// the end of the input repeat the last sample (padding of the final block).
struct BlockInput {
  const float* src;
  std::uint64_t first;
  std::uint64_t frames;  // > 0
  std::int32_t At(std::uint32_t i) const noexcept {
    const std::uint64_t f = first + i;
    return ToS16(src[f < frames ? f : frames - 1u]);
  }
};

// Encodes samples 1..count-1 of one channel of one block starting from
// (sample 0, index). Writes nibbles if `block` is non-null. Returns the sum of
// squared errors; *finalIndex receives the step index after the block.
std::uint64_t EncodeChannel(const BlockInput& in, std::uint32_t count, std::int32_t index,
                            std::uint32_t channels, std::uint32_t c, std::uint8_t* block,
                            std::int32_t* finalIndex) noexcept {
  std::int32_t predictor = in.At(0);
  std::uint64_t err = 0;
  for (std::uint32_t k = 0; k + 1u < count; ++k) {
    const std::int32_t s = in.At(k + 1u);
    const std::uint32_t n = ImaNibble(predictor, index, s);
    detail::ImaStep(predictor, index, n);
    const std::int64_t e = static_cast<std::int64_t>(s) - predictor;
    err += static_cast<std::uint64_t>(e * e);
    if (block != nullptr) {
      std::uint8_t& b = block[detail::AdpcmByteOffset(channels, c, k)];
      b = static_cast<std::uint8_t>(b | ((k & 1u) ? (n << 4) : n));
    }
  }
  *finalIndex = index;
  return err;
}

void EncodeAdpcm(const CodecHeader& h, const float* const* src, std::uint64_t frames, std::uint8_t* dst) noexcept {
  const std::uint32_t ch = h.channels;
  const std::uint32_t fpb = AdpcmFramesPerBlock(ch, h.block_align);
  const std::uint64_t blocks = (frames + fpb - 1u) / fpb;
  std::int32_t prevIndex[SND_MAX_CHANNELS] = {};
  for (std::uint64_t b = 0; b < blocks; ++b) {
    std::uint8_t* block = dst + b * h.block_align;
    std::memset(block, 0, h.block_align);
    for (std::uint32_t c = 0; c < ch; ++c) {
      const BlockInput in{src[c], b * fpb, frames};
      // Initial step index: the previous block's final index or the best of a coarse grid.
      std::int32_t bestIndex = prevIndex[c];
      std::int32_t ignored = 0;
      std::uint64_t bestErr = EncodeChannel(in, fpb, bestIndex, ch, c, nullptr, &ignored);
      for (std::int32_t cand = 0; cand <= 88 && bestErr > 0u; cand += 4) {
        const std::uint64_t e = EncodeChannel(in, fpb, cand, ch, c, nullptr, &ignored);
        if (e < bestErr) {
          bestErr = e;
          bestIndex = cand;
        }
      }
      std::uint8_t* head = block + 4u * c;
      base::StoreLE16(head, static_cast<std::uint16_t>(static_cast<std::int16_t>(in.At(0))));
      head[2] = static_cast<std::uint8_t>(bestIndex);
      head[3] = 0;
      EncodeChannel(in, fpb, bestIndex, ch, c, block, &prevIndex[c]);
    }
  }
}

}  // namespace

const char* EncodeStatusName(EncodeStatus s) noexcept {
  switch (s) {
    case EncodeStatus::Ok: return "ok";
    case EncodeStatus::BadHeader: return "bad header";
    case EncodeStatus::BadArguments: return "bad arguments";
    case EncodeStatus::NonFiniteInput: return "non-finite input sample";
    case EncodeStatus::BufferTooSmall: return "destination buffer too small";
  }
  return "?";
}

CodecHeader MakePcmHeader(std::uint32_t codec, std::uint32_t channels, std::uint32_t sampleRate,
                          std::uint64_t frames) noexcept {
  CodecHeader h{};
  h.codec = codec;
  h.channels = channels;
  h.sample_rate = sampleRate;
  h.frames_per_packet = 1;
  h.total_frames = frames;
  h.block_align = channels * (codec == SND_CODEC_PCM_F32 ? 4u : 2u);
  return h;
}

CodecHeader MakeAdpcmHeader(std::uint32_t channels, std::uint32_t sampleRate, std::uint64_t frames,
                            std::uint32_t blockBytesPerChannel) noexcept {
  CodecHeader h{};
  h.codec = SND_CODEC_ADPCM;
  h.channels = channels;
  h.sample_rate = sampleRate;
  h.total_frames = frames;
  h.block_align = channels * blockBytesPerChannel;
  h.frames_per_packet = AdpcmFramesPerBlock(channels, h.block_align);
  return h;
}

std::uint64_t EncodedBytes(const CodecHeader& h) noexcept {
  const std::uint32_t pb = PacketBytes(h);
  const std::uint32_t pf = PacketFrames(h);
  if (pb == 0u || pf == 0u) return 0;
  const std::uint64_t frames = static_cast<std::uint64_t>(h.pre_skip) + h.total_frames;
  const std::uint64_t packets = frames / pf + (frames % pf != 0u ? 1u : 0u);
  if (packets != 0u && packets > UINT64_MAX / pb) return 0;
  return packets * pb;
}

EncodeStatus Encode(const CodecHeader& h, const float* const* src, std::uint64_t frames, std::uint8_t* dst,
                    std::uint64_t dstBytes, std::uint64_t* written) noexcept {
  if (written != nullptr) *written = 0;
  if (ValidateHeader(h) != HeaderError::None) return EncodeStatus::BadHeader;
  if (frames != static_cast<std::uint64_t>(h.pre_skip) + h.total_frames) return EncodeStatus::BadArguments;
  const std::uint64_t bytes = EncodedBytes(h);
  if (frames > 0u && bytes == 0u) return EncodeStatus::BadArguments;  // size overflow
  if (frames > 0u) {
    if (src == nullptr || dst == nullptr) return EncodeStatus::BadArguments;
    for (std::uint32_t c = 0; c < h.channels; ++c) {
      if (src[c] == nullptr) return EncodeStatus::BadArguments;
      for (std::uint64_t i = 0; i < frames; ++i) {
        if (!std::isfinite(src[c][i])) return EncodeStatus::NonFiniteInput;
      }
    }
  }
  if (dstBytes < bytes) return EncodeStatus::BufferTooSmall;
  const std::uint32_t ch = h.channels;
  if (h.codec == SND_CODEC_PCM_F32) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        base::StoreLE32(dst + (i * ch + c) * 4u, std::bit_cast<std::uint32_t>(src[c][i]));
      }
    }
  } else if (h.codec == SND_CODEC_PCM_S16) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        base::StoreLE16(dst + (i * ch + c) * 2u,
                          static_cast<std::uint16_t>(static_cast<std::int16_t>(ToS16(src[c][i]))));
      }
    }
  } else if (frames > 0u) {
    EncodeAdpcm(h, src, frames, dst);
  }
  if (written != nullptr) *written = bytes;
  return EncodeStatus::Ok;
}

std::uint32_t BuildSeekTable(const CodecHeader& h, std::uint64_t intervalFrames, std::uint64_t* frames,
                             std::uint64_t* byteOffsets, std::uint32_t capacity) noexcept {
  const std::uint32_t pb = PacketBytes(h);
  const std::uint32_t pf = PacketFrames(h);
  if (pb == 0u || pf == 0u) return 0;
  const bool write = frames != nullptr && byteOffsets != nullptr;
  std::uint32_t count = 0;
  auto emit = [&](std::uint64_t f, std::uint64_t off) {
    if (write && count < capacity) {
      frames[count] = f;
      byteOffsets[count] = off;
    }
    if (count < UINT32_MAX) ++count;
  };
  emit(0, 0);
  const std::uint64_t pre = h.pre_skip;
  const std::uint64_t all = pre + h.total_frames;
  const std::uint64_t packets = all / pf + (all % pf != 0u ? 1u : 0u);
  std::uint64_t last = 0;
  std::uint64_t p = 1;
  while (p < packets && count < UINT32_MAX) {
    const std::uint64_t start = p * pf;
    if (start <= pre) {
      p = pre / pf + 1u;
      continue;
    }
    const std::uint64_t f = start - pre;
    if (f >= h.total_frames) break;
    if (intervalFrames != 0u && f - last < intervalFrames) {
      const std::uint64_t want = last + intervalFrames + pre;  // decoded frame we want to reach
      const std::uint64_t q = want / pf + (want % pf != 0u ? 1u : 0u);
      p = q > p ? q : p + 1u;
      continue;
    }
    emit(f, p * pb);
    last = f;
    ++p;
  }
  return count;
}

}  // namespace snd::codec::encode
