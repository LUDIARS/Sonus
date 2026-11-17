// Encoders and build-time helpers (design.md 6.3, 6.4, 10.2). Used by buildtool.
//
// This header and the encode_*.cpp / seek_table.cpp objects are build-time
// only. sonus_codec is a static library: a runtime binary that does not call
// anything declared here does not link the encoders.
//
// All functions are pure, thread-safe and write only into caller buffers. They
// are allocation-free except EncodeOpus, which creates a libopus encoder on the
// heap for the duration of the call (build time only). Input audio is non-interleaved float32 (one pointer per
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
  CodecUnavailable = 5, // EncodeOpus in a build without Opus (SND_CODEC_OPUS=OFF)
  BadSettings = 6,      // EncodeOpus: settings out of range
  EncoderFailed = 7,    // EncodeOpus: libopus reported an error
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

// --- Opus (bank-format.md §6.5, CONTRACTS.md D-56..D-60) ------------------------------
// Encode() does not take Opus (BadHeader): Opus packets are variable-size, so
// it has its own entry point that returns the header it produced.

enum class OpusVbr : std::uint32_t { Vbr = 0, ConstrainedVbr = 1, Cbr = 2 };
enum class OpusSignal : std::uint32_t { Auto = 0, Music = 1, Voice = 2 };

struct OpusSettings {
  // Bits per second for the whole asset (all channels). 0 = kOpusDefaultBitratePerChannel
  // x channels. Otherwise 6000 .. 510000 x channels.
  std::uint32_t bitrate = 0;
  std::uint32_t complexity = 10;  // 0 .. 10 (libopus OPUS_SET_COMPLEXITY)
  OpusVbr vbr = OpusVbr::Vbr;
  OpusSignal signal = OpusSignal::Auto;
  // Packets that do not depend on the previous packet's prediction state
  // (libopus OPUS_SET_PREDICTION_DISABLED). With it, a decode that starts at a
  // seek point is bit-identical to one continuous decode after two packets, so
  // the 80 ms pre-roll gives sample-exact seeks and loop restarts. Without it
  // the decoder only converges (errors of ~1e-1 after 80 ms, ~2e-3 after
  // 240 ms) in exchange for a few percent of bitrate.
  bool independentFrames = true;
};
inline constexpr std::uint32_t kOpusDefaultBitratePerChannel = 64000;
inline constexpr std::uint32_t kOpusMinBitrate = 6000;
inline constexpr std::uint32_t kOpusMaxBitratePerChannel = 510000;
// Version of the Opus byte layout and encoder settings (differential-build
// caches): changes when the layout, the defaults or the bundled libopus change.
// Upper byte: Sonus' layout / settings revision; low 24 bits: libopus 1.5.2.
inline constexpr std::uint32_t kOpusEncoderVersion = (1u << 24) | 0x010502u;

// Largest setup block: kOpusSetupHeaderBytes + SND_MAX_CHANNELS.
inline constexpr std::uint32_t kOpusMaxSetupBytes = 4u + 8u;

struct OpusEncoded {
  // Ready to validate / decode: codec OPUS, 48000 Hz, frames_per_packet 960,
  // block_align 0, pre_skip = encoder lookahead (312), pre_roll 3840,
  // total_frames = the input frame count; extra points to `setup` (re-point
  // it if this struct is copied).
  CodecHeader header;
  std::uint8_t setup[kOpusMaxSetupBytes];
  std::uint32_t setupBytes;
  std::uint64_t packets;
  std::uint64_t bytes;  // bytes written to dst
};

// True when EncodeOpus works in this build (SND_CODEC_OPUS=ON).
bool OpusEncoderAvailable() noexcept;
// Upper bound of the encoded bytes for `frames` input frames (0 if channels is
// not 1..8): packets x (prefix + streams x kOpusMaxPacketBytesPerStream).
std::uint64_t OpusMaxEncodedBytes(std::uint32_t channels, std::uint64_t frames) noexcept;

// Encodes `frames` frames of 48 kHz audio (non-interleaved, channels 1..8; the
// channel order of a WAV file: L R C LFE BL BR SL SR, re-ordered internally to
// the Vorbis order of RFC 7845 mapping family 1 and back through the mapping).
// 1-2 channels use mapping family 0 (one stream), 3-8 family 1 (surround
// multistream). Pre-skip and padding: ceil((frames + pre_skip) / 960) packets,
// the tail padded with silence. Deterministic for a given build.
// sampleRate != 48000, channels out of range or null pointers -> BadArguments.
EncodeStatus EncodeOpus(const OpusSettings& settings, const float* const* src, std::uint32_t channels,
                        std::uint32_t sampleRate, std::uint64_t frames, std::uint8_t* dst, std::uint64_t dstBytes,
                        OpusEncoded* out) noexcept;

// Seek table of an Opus asset (same contract as BuildSeekTable): walks the
// length prefixes of data[0, dataBytes). A packet whose first decoded frame d
// has d > pre_skip and d - pre_skip < total_frames is a candidate {d - pre_skip,
// byte offset of its prefix}; entries are at least intervalFrames apart, entry
// 0 = {0, 0}. Returns 0 if the header or the packet walk is invalid.
std::uint32_t BuildOpusSeekTable(const CodecHeader& header, const std::uint8_t* data, std::uint64_t dataBytes,
                                 std::uint64_t intervalFrames, std::uint64_t* frames, std::uint64_t* byteOffsets,
                                 std::uint32_t capacity) noexcept;

}  // namespace snd::codec::encode

#endif  // SND_CODEC_ENCODE_HPP
