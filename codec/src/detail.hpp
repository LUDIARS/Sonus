// Private helpers of the codec module (snd::codec::detail). Some of these are
// generic (little-endian access, alignment) and belong to base once base has
// them; see codec/README.md "Generic parts kept in codec".
#ifndef SND_CODEC_SRC_DETAIL_HPP
#define SND_CODEC_SRC_DETAIL_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "snd/codec/codec.hpp"

namespace snd::codec::detail {

// --- generic ------------------------------------------------------------------
constexpr std::uint64_t AlignUp(std::uint64_t v, std::uint64_t a) noexcept { return (v + (a - 1)) & ~(a - 1); }

inline std::uint16_t LoadLE16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t LoadLE32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void StoreLE16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void StoreLE32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

// --- IMA ADPCM ----------------------------------------------------------------
inline constexpr std::int16_t kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline constexpr std::int8_t kImaIndexAdjust[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Decoder update for one nibble (shared by encoder and decoder so they agree bit for bit).
inline void ImaStep(std::int32_t& predictor, std::int32_t& index, std::uint32_t nibble) noexcept {
  const std::int32_t step = kImaStep[index];
  std::int32_t diff = step >> 3;
  if (nibble & 4u) diff += step;
  if (nibble & 2u) diff += step >> 1;
  if (nibble & 1u) diff += step >> 2;
  predictor += (nibble & 8u) ? -diff : diff;
  if (predictor > 32767) predictor = 32767;
  if (predictor < -32768) predictor = -32768;
  index += kImaIndexAdjust[nibble & 15u];
  if (index < 0) index = 0;
  if (index > 88) index = 88;
}

// Layout of a block (WAV IMA ADPCM): per channel a 4-byte header
// {s16 predictor, u8 step index, u8 reserved}, then groups of 4 bytes per
// channel, each holding 8 samples (low nibble first). Byte of channel c,
// sample k (k >= 0 counted after the header sample):
inline std::uint32_t AdpcmByteOffset(std::uint32_t channels, std::uint32_t c, std::uint32_t k) noexcept {
  return 4u * channels + (k / 8u) * 4u * channels + c * 4u + (k % 8u) / 2u;
}

// --- decoder state --------------------------------------------------------------
inline constexpr std::uint32_t kStateMagic = 0x53434453u;  // 'SDCS'

// Placed at the start of the caller's state memory; the hold buffer follows at
// kStateHeaderBytes.
struct DecoderState {
  std::uint32_t magic;
  std::uint32_t codec;
  std::uint32_t channels;
  std::uint32_t failed;           // non-zero: every Decode returns SND_DECODE_ERROR
  std::uint32_t packetBytes;
  std::uint32_t packetFrames;
  std::uint32_t holdCapacity;     // bytes of the hold buffer
  std::uint32_t holdHead;         // valid bytes are [holdHead, holdTail)
  std::uint32_t holdTail;
  std::uint32_t packetProgress;   // frames of the current packet already processed (ADPCM)
  std::uint32_t preSkip;
  std::uint32_t reserved;
  std::uint64_t pos;              // decoded-timeline index of the next frame (includes pre_skip)
  std::uint64_t end;              // pre_skip + total_frames
  std::int32_t predictor[SND_MAX_CHANNELS];
  std::int32_t stepIndex[SND_MAX_CHANNELS];
};
inline constexpr std::uint32_t kStateHeaderBytes =
    static_cast<std::uint32_t>(AlignUp(sizeof(DecoderState), kStateAlignment));
static_assert(kStateHeaderBytes <= kMinStateBytes);

inline std::uint8_t* HoldBuffer(DecoderState* s) noexcept {
  return reinterpret_cast<std::uint8_t*>(s) + kStateHeaderBytes;
}

}  // namespace snd::codec::detail

#endif  // SND_CODEC_SRC_DETAIL_HPP
