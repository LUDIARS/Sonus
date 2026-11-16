// Built-in decoders (design.md 6.4, CONTRACTS.md D-23). Runtime side of the codec module.
//
// The built-in codecs implement snd::ICodec (snd/ext/codec.hpp) for
//   SND_CODEC_PCM_F32  little-endian float32, interleaved; NaN / +-Inf decode as 0
//                      (corrupt data must not reach feedback DSP such as reverb)
//   SND_CODEC_PCM_S16  little-endian int16, interleaved
//   SND_CODEC_ADPCM    IMA ADPCM in WAV/IMA block layout, 1..SND_MAX_CHANNELS
//   SND_CODEC_OPUS     Opus (bundled libopus, CONTRACTS.md D-56..D-60) when the
//                      build has SND_CODEC_OPUS=ON (OpusAvailable()). Packets
//                      of 20 ms (960 frames at 48 kHz) with a 2-byte length
//                      prefix; setup data (channel mapping) in header.extra.
//                      Without it, lookups fail with LookupFailure::
//                      OpusUnavailable (registry.hpp) and ValidateHeader with
//                      CodecUnavailable, unless a plugin registers 'OPUS'.
// See codec/README.md and bank-format.md §6.5.
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
//   - Opus decodes a whole packet at once (libopus cannot resume inside a
//     packet), so its state also keeps the decoded frames of at most one
//     packet that did not fit into max_frames (as float, 960 x channels).
//     Undecoded input is still held compressed, with the same bound
//     (kHoldInputBytes + one packet). Seek(f > 0) requires f + pre_skip to be a
//     multiple of 960 (a packet boundary).
//   - An invalid header (ValidateHeader != None) is accepted by StateBytes and
//     Init (StateBytes returns kMinStateBytes) and every Decode returns
//     SND_DECODE_ERROR. A corrupt ADPCM block header (step index > 88) makes
//     Decode return SND_DECODE_ERROR (sticky until Init/Seek).
//   - A state initialised by one codec object and passed to another codec's
//     Decode makes it return SND_DECODE_ERROR; such a Seek leaves the state
//     failed (the next Decode errors) until Init or a Seek by the right codec.
//     Init with a header whose codec differs from the object is an invalid header.
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
  BadBlockAlign = 5,       // PCM: not 0 / the frame size; ADPCM: wrong layout or too large
  BadFramesPerPacket = 6,  // PCM: not 0 / 1; ADPCM: not 0 / the block's frame count
  FrameCountOverflow = 7,  // pre_skip + total_frames overflows
  BadCodecSetup = 8,       // Opus: header.extra missing or invalid (version, streams, mapping)
};
const char* HeaderErrorName(HeaderError e) noexcept;

// Validates a header for the built-in codec named by header.codec.
HeaderError ValidateHeader(const CodecHeader& header) noexcept;

// Built-in codec objects (stateless, constant-initialised, valid for the whole
// program). nullptr for unknown fourccs, and for SND_CODEC_OPUS when the build
// has no Opus (OpusAvailable() == false).
ICodec* BuiltinCodec(std::uint32_t fourcc) noexcept;
bool IsBuiltinCodec(std::uint32_t fourcc) noexcept;

// Bytes of one packet: the frame size for PCM, block_align for ADPCM. 0 if the
// header is invalid, and 0 for Opus (variable-size packets).
std::uint32_t PacketBytes(const CodecHeader& header) noexcept;
// Frames of one packet: 1 for PCM, frames per block for ADPCM, 960 for Opus.
// 0 if invalid.
std::uint32_t PacketFrames(const CodecHeader& header) noexcept;
// Undecoded bytes a built-in state can hold (>= kHoldInputBytes + one packet - 1).
// Opus: kHoldInputBytes + the largest packet (length prefix included).
std::uint32_t HoldCapacityBytes(const CodecHeader& header) noexcept;

// --- Opus (bank-format.md §6.5, CONTRACTS.md D-56..D-60) ------------------------------
// True when this build contains the Opus decoder (SND_CODEC_OPUS=ON).
bool OpusAvailable() noexcept;
inline constexpr std::uint32_t kOpusSampleRate = 48000;     // the only rate of an Opus asset
inline constexpr std::uint32_t kOpusPacketFrames = 960;     // every packet is 20 ms
inline constexpr std::uint32_t kOpusPreRollFrames = 3840;   // 80 ms (RFC 7845 4.6), written to pre_roll
inline constexpr std::uint32_t kOpusLengthPrefixBytes = 2;  // u16 LE packet length before each packet
// Largest packet per elementary stream (1275 bytes of one 20 ms frame + 2 bytes
// of self-delimiting length). A packet longer than streams x this is corrupt.
inline constexpr std::uint32_t kOpusMaxPacketBytesPerStream = 1277;
inline constexpr std::uint32_t kOpusMaxStreams = SND_MAX_CHANNELS;
// Setup data (header.extra, ASST.extraOffset/extraBytes), 4 + channels bytes:
//   u8 version (kOpusSetupVersion), u8 channels, u8 streams, u8 coupledStreams,
//   u8 mapping[channels] (output channel i = decoded stream channel mapping[i],
//   255 = silence; RFC 7845 5.1.1 semantics).
inline constexpr std::uint32_t kOpusSetupVersion = 1;
inline constexpr std::uint32_t kOpusSetupHeaderBytes = 4;
struct OpusSetup {
  std::uint32_t streams;
  std::uint32_t coupledStreams;
  std::uint8_t mapping[SND_MAX_CHANNELS];
};
// Parses and checks header.extra for header.channels. BadChannels / BadCodecSetup on failure.
HeaderError ParseOpusSetup(const CodecHeader& header, OpusSetup* out) noexcept;
// streams x kOpusMaxPacketBytesPerStream (packet payload, prefix not included). 0 if invalid.
std::uint32_t OpusMaxPacketBytes(const CodecHeader& header) noexcept;

// IMA ADPCM block geometry: frames in a block of `blockAlign` bytes, 0 if the
// layout is invalid (blockAlign must be 4*ch*(1+k), k >= 1).
std::uint32_t AdpcmFramesPerBlock(std::uint32_t channels, std::uint32_t blockAlign) noexcept;

// Seek position for fixed-size built-in codecs without a seek table: the packet
// containing playable frame `target` (pass target - pre_roll yourself) is
// returned as {first playable frame of the packet, byte offset within the codec
// data}. A packet that starts inside pre_skip maps to {0, 0} (Seek(0) restarts
// the stream). False if the header is invalid, out is null, target >=
// total_frames (past the playable range), or the byte offset does not fit in
// 64 bits.
struct PacketPosition {
  std::uint64_t frame;
  std::uint64_t byteOffset;
};
bool PacketForFrame(const CodecHeader& header, std::uint64_t target, PacketPosition* out) noexcept;

// Seek table lookup (6.3, D-23): index of the last entry whose frame is
// <= target - preRoll (saturating at 0). `frames` is the SEEK.frame column
// slice of the asset, ascending, with frames[0] == 0.
// Returns kNoSeekPoint ("not found") if frames is null, count == 0, or (5-arg
// form) target >= totalFrames (past the playable range; pass the header's
// total_frames). The 4-arg form has no end bound and is kept for callers that
// check the range themselves; source should use the 5-arg form.
inline constexpr std::uint32_t kNoSeekPoint = 0xFFFFFFFFu;
std::uint32_t FindSeekPoint(const std::uint64_t* frames, std::uint32_t count, std::uint64_t target,
                            std::uint32_t preRoll, std::uint64_t totalFrames) noexcept;
std::uint32_t FindSeekPoint(const std::uint64_t* frames, std::uint32_t count, std::uint64_t target,
                            std::uint32_t preRoll) noexcept;

}  // namespace snd::codec

#endif  // SND_CODEC_CODEC_HPP
