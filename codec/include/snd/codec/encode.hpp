// Encoders and build-time helpers (design.md 6.3, 6.4, 10.2). Used by buildtool.
//
// This header and the encode_*.cpp / seek_table.cpp objects are build-time
// only. sonus_codec is a static library: a runtime binary that does not call
// anything declared here does not link the encoders.
//
// All functions are pure, thread-safe, allocation-free and write only into
// caller buffers. Input audio is non-interleaved float32 (one pointer per
// channel), output is the bank / stream-file byte layout read by the built-in
// decoders (codec/README.md "Data layout").
#ifndef SND_CODEC_ENCODE_HPP
#define SND_CODEC_ENCODE_HPP

#include <cstdint>

#include "snd/ext/codec.hpp"

namespace snd::codec::encode {

// Default IMA ADPCM block: 512 bytes per channel = 1017 frames (21 ms at 48 kHz).
inline constexpr std::uint32_t kDefaultAdpcmBlockBytesPerChannel = 512;

enum class EncodeStatus : std::uint32_t {
  Ok = 0,
  BadHeader = 1,        // ValidateHeader failed, or the codec has no built-in encoder (OPUS, custom)
  BadArguments = 2,     // null pointers, frame count mismatch
  NonFiniteInput = 3,   // NaN / Inf sample
  BufferTooSmall = 4,   // dstBytes < EncodedBytes(header)
};
const char* EncodeStatusName(EncodeStatus s) noexcept;

// Headers for the built-in codecs. pre_skip = pre_roll = 0, extra = none.
// The result is not validated; check it with snd::codec::ValidateHeader.
CodecHeader MakePcmHeader(std::uint32_t codec, std::uint32_t channels, std::uint32_t sampleRate,
                          std::uint64_t frames) noexcept;  // codec = SND_CODEC_PCM_F32 / _S16
CodecHeader MakeAdpcmHeader(std::uint32_t channels, std::uint32_t sampleRate, std::uint64_t frames,
                            std::uint32_t blockBytesPerChannel = kDefaultAdpcmBlockBytesPerChannel) noexcept;

// Bytes of encoded data for header.pre_skip + header.total_frames frames
// (ADPCM: whole blocks, the last one padded). 0 if the header is invalid.
std::uint64_t EncodedBytes(const CodecHeader& header) noexcept;

// Encodes exactly `frames` (= header.pre_skip + header.total_frames) frames.
// PCM_S16 rounds to nearest and clamps to [-1, 32767/32768]. ADPCM picks the
// initial step index of each block and channel by search (better quality on
// transients). *written = EncodedBytes(header) on Ok.
EncodeStatus Encode(const CodecHeader& header, const float* const* src, std::uint64_t frames,
                    std::uint8_t* dst, std::uint64_t dstBytes, std::uint64_t* written) noexcept;

// Seek table (6.3; bank SEEK rows: frame, byteOffset within the codec data).
// Entries are packet boundaries at least `intervalFrames` apart (0 = every
// packet), in playable frames, ascending, entry 0 = {0, 0}. Packets that start
// inside pre_skip or at/after the end are left out. Returns the number of
// entries; writes at most `capacity` (pass capacity 0 / null arrays to count).
std::uint32_t BuildSeekTable(const CodecHeader& header, std::uint64_t intervalFrames,
                             std::uint64_t* frames, std::uint64_t* byteOffsets,
                             std::uint32_t capacity) noexcept;

}  // namespace snd::codec::encode

#endif  // SND_CODEC_ENCODE_HPP
