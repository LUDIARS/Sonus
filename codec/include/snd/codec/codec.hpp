// Built-in decoders (design.md 6.4, CONTRACTS.md D-23). Runtime side of the codec module.
//
// The built-in codecs implement snd::ICodec (snd/ext/codec.hpp) for
//   SND_CODEC_PCM_F32  little-endian float32, interleaved
//   SND_CODEC_PCM_S16  little-endian int16, interleaved
//   SND_CODEC_ADPCM    IMA ADPCM in WAV/IMA block layout, 1..SND_MAX_CHANNELS
// SND_CODEC_OPUS is not built in (no third-party code, stage0 A0-3). Lookups of
// it fail with LookupFailure::OpusUnavailable (registry.hpp) unless a plugin
// codec registers that fourcc. See codec/README.md.
//
// Decode semantics (D-23, snd/ext.h is normative), as implemented here:
//   - Decode consumes ALL src bytes. Bytes that cannot be decoded in this call
//     (a partial packet, or packets whose frames do not fit into max_frames)
//     are kept in the state in COMPRESSED form, never as decoded floats, so the
//     state size does not depend on max_frames.
//   - Frames are emitted as soon as their bytes are present (an ADPCM block is
//     decoded incrementally), so the output sequence does not depend on how the
//     input is chunked.
//   - Decode(NULL, 0, dst, max) returns held frames ("drain"). It does not mark
//     the end of the stream; the stream end is total_frames from the header.
//   - Holding limit: after a call, the state can keep at most
//     HoldCapacityBytes(header) undecoded bytes. Exceeding it makes Decode
//     return SND_DECODE_ERROR (sticky until Init/Seek). Callers that drain until
//     Decode returns < max_frames before feeding more, and feed at most
//     kHoldInputBytes per call, never hit it. A caller that always gives enough
//     output room may feed any amount.
//   - Output is clamped to the playable range: the first pre_skip decoded
//     frames after Init / Seek(0) are dropped, and nothing is emitted after
//     total_frames playable frames. Excess input is consumed and ignored.
//   - Seek(0) restarts at the very first byte of the stream (pre_skip applies
//     again). Seek(f > 0) means the next byte is the first byte of the packet
//     whose first PLAYABLE frame is f (decoded frame f + pre_skip). For ADPCM
//     (f + pre_skip) must be a multiple of frames_per_packet, otherwise the
//     next Decode fails.
//   - An invalid header (ValidateHeader != None) is accepted by StateBytes and
//     Init (StateBytes returns kMinStateBytes) and every Decode returns
//     SND_DECODE_ERROR. A corrupt ADPCM block header (step index > 88) makes
//     Decode return SND_DECODE_ERROR (sticky until Init/Seek).
//
// Threads: StateBytes / ValidateHeader / helpers are pure and thread-safe.
// Init / Decode / Seek are called by one thread at a time per state (normal
// worker, prefetch lane). None of them allocates, locks or does IO, so they are
// also safe inside SND_RT_SCOPE.
#ifndef SND_CODEC_CODEC_HPP
#define SND_CODEC_CODEC_HPP

#include <cstdint>

#include "snd/ext/codec.hpp"
#include "snd/ext/types.hpp"

namespace snd::codec {

// Decoder states are placed at 64-byte boundaries (decoder pool slots).
inline constexpr std::uint32_t kStateAlignment = 64;
// Size reported for an invalid header (enough for Init to record the error).
inline constexpr std::uint32_t kMinStateBytes = 128;
// Undecoded input a built-in decoder can always hold on top of one packet.
inline constexpr std::uint32_t kHoldInputBytes = 16u * 1024u;
// Upper bound of an IMA ADPCM block accepted by the built-in decoder.
inline constexpr std::uint32_t kMaxAdpcmBlockBytes = 64u * 1024u;
// Largest valid IMA ADPCM step index.
inline constexpr std::uint32_t kAdpcmMaxStepIndex = 88;

enum class HeaderError : std::uint32_t {
  None = 0,
  UnknownCodec = 1,        // fourcc is not a built-in codec
  CodecUnavailable = 2,    // SND_CODEC_OPUS: not compiled into this build
  BadChannels = 3,         // 0 or > SND_MAX_CHANNELS
  BadSampleRate = 4,       // 0
  BadBlockAlign = 5,       // PCM: not 0 / multiple of the frame size; ADPCM: wrong layout or too large
  BadFramesPerPacket = 6,  // does not match block_align
  FrameCountOverflow = 7,  // pre_skip + total_frames overflows
};
const char* HeaderErrorName(HeaderError e) noexcept;

// Validates a header for the built-in codec named by header.codec.
HeaderError ValidateHeader(const CodecHeader& header) noexcept;

// Built-in codec objects (stateless, constant-initialised, valid for the whole
// program). nullptr for SND_CODEC_OPUS and unknown fourccs.
ICodec* BuiltinCodec(std::uint32_t fourcc) noexcept;
bool IsBuiltinCodec(std::uint32_t fourcc) noexcept;

// Bytes of one packet: the frame size for PCM, block_align for ADPCM. 0 if the
// header is invalid.
std::uint32_t PacketBytes(const CodecHeader& header) noexcept;
// Frames of one packet: 1 for PCM, frames per block for ADPCM. 0 if invalid.
std::uint32_t PacketFrames(const CodecHeader& header) noexcept;
// Undecoded bytes a built-in state can hold (>= kHoldInputBytes + one packet - 1).
std::uint32_t HoldCapacityBytes(const CodecHeader& header) noexcept;

// IMA ADPCM block geometry: frames in a block of `blockAlign` bytes, 0 if the
// layout is invalid (blockAlign must be 4*ch*(1+k), k >= 1).
std::uint32_t AdpcmFramesPerBlock(std::uint32_t channels, std::uint32_t blockAlign) noexcept;

// Seek position for fixed-size built-in codecs without a seek table: the packet
// containing playable frame `target` (pass target - pre_roll yourself) is
// returned as {first playable frame of the packet, byte offset within the codec
// data}. A packet that starts inside pre_skip maps to {0, 0} (Seek(0) restarts
// the stream). False if the header is invalid.
struct PacketPosition {
  std::uint64_t frame;
  std::uint64_t byteOffset;
};
bool PacketForFrame(const CodecHeader& header, std::uint64_t target, PacketPosition* out) noexcept;

// Seek table lookup (6.3, D-23): index of the last entry whose frame is
// <= target - preRoll (saturating at 0). `frames` is the SEEK.frame column
// slice of the asset, ascending, with frames[0] == 0. kNoSeekPoint if count == 0.
inline constexpr std::uint32_t kNoSeekPoint = 0xFFFFFFFFu;
std::uint32_t FindSeekPoint(const std::uint64_t* frames, std::uint32_t count, std::uint64_t target,
                            std::uint32_t preRoll) noexcept;

}  // namespace snd::codec

#endif  // SND_CODEC_CODEC_HPP
