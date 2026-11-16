// Built-in Opus decoder (bundled libopus, CONTRACTS.md D-56..D-60, bank-format.md §6.5).
//
// Byte layout: a sequence of packets, each a u16 little-endian length L
// (1 <= L <= streams x kOpusMaxPacketBytesPerStream) followed by L bytes of an
// Opus multistream packet (RFC 7845 5.1.1 / libopus opus_multistream_decode)
// of exactly 960 frames (20 ms at 48 kHz). The channel mapping comes from the
// setup data in header.extra (snd::codec::ParseOpusSetup).
//
// State (one decoder pool slot, 64-byte aligned):
//   [OpusState 128 B][input hold: HoldCapacityBytes][decoded frames: 960 x ch
//   float, interleaved][libopus OpusMSDecoder]
// libopus is built with VAR_ARRAYS (scratch on the stack), so Init / Decode /
// Seek do not allocate, lock or do IO: they may run inside SND_RT_SCOPE.
//
// D-23 as implemented here (the rest is the same as decoders.cpp):
//   - Decode consumes all input. Undecoded bytes are held compressed (bound:
//     kHoldInputBytes + one largest packet with its prefix). libopus decodes a
//     packet at once, so the frames of the one packet that did not fit into
//     max_frames are kept decoded (float) and returned first by the next call.
//   - A packet is decoded only when the output has room (or a pre_skip-only /
//     past-the-end packet is reached with room), so a caller that stops after
//     a full output and then seeks does not pay for packets it never uses.
//   - Pre-skip frames after Init / Seek(0) are dropped, nothing is emitted after
//     total_frames; packets that start at or after the end are consumed
//     without decoding.
//   - Corrupt data (bad length, libopus error, a packet that is not 960 frames)
//     returns SND_DECODE_ERROR, sticky until Init / Seek.
#include "detail.hpp"
#include "snd/codec/codec.hpp"

#if SND_CODEC_OPUS_ENABLED

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <opus_multistream.h>

namespace snd::codec {
namespace {

using detail::kFailedHeader;
using detail::kFailedRuntime;

constexpr std::uint32_t kHeaderBytes = kMinStateBytes;  // the OpusState block

struct OpusState {
  // Same first four fields as detail::DecoderState (foreign-state detection).
  std::uint32_t magic;
  std::uint32_t codec;
  std::uint32_t channels;
  std::uint32_t failed;
  std::uint32_t holdCapacity;   // bytes of the hold buffer
  std::uint32_t holdHead;       // valid held bytes are [holdHead, holdTail)
  std::uint32_t holdTail;
  std::uint32_t pcmHead;        // next decoded frame to emit
  std::uint32_t pcmCount;       // decoded frames of the current packet that are playable
  std::uint32_t maxPacket;      // largest valid packet payload (bytes)
  std::uint32_t preSkip;
  std::uint32_t pcmOffset;      // from the state start
  std::uint32_t decoderOffset;  // from the state start
  std::uint32_t reserved;
  std::uint64_t pos;            // decoded-timeline index of the next packet's first frame
  std::uint64_t end;            // pre_skip + total_frames
};
static_assert(sizeof(OpusState) <= kHeaderBytes);
static_assert(offsetof(OpusState, magic) == offsetof(detail::DecoderState, magic));
static_assert(offsetof(OpusState, codec) == offsetof(detail::DecoderState, codec));
static_assert(offsetof(OpusState, channels) == offsetof(detail::DecoderState, channels));
static_assert(offsetof(OpusState, failed) == offsetof(detail::DecoderState, failed));

struct Output {
  float* const* dst;
  std::uint32_t written;
  std::uint32_t max;
};

std::uint8_t* Base(OpusState* s) noexcept { return reinterpret_cast<std::uint8_t*>(s); }
std::uint8_t* HoldBuffer(OpusState* s) noexcept { return Base(s) + kHeaderBytes; }
float* Pcm(OpusState* s) noexcept { return reinterpret_cast<float*>(Base(s) + s->pcmOffset); }
OpusMSDecoder* Decoder(OpusState* s) noexcept { return reinterpret_cast<OpusMSDecoder*>(Base(s) + s->decoderOffset); }

struct Layout {
  std::uint32_t hold;
  std::uint32_t pcmOffset;
  std::uint32_t decoderOffset;
  std::uint32_t total;
};

// Memory layout for a valid header (ValidateHeader == None). False if libopus
// rejects the stream configuration.
bool ComputeLayout(const CodecHeader& h, const OpusSetup& setup, Layout* out) noexcept {
  const int decBytes = opus_multistream_decoder_get_size(static_cast<int>(setup.streams),
                                                         static_cast<int>(setup.coupledStreams));
  if (decBytes <= 0) return false;
  Layout l{};
  l.hold = HoldCapacityBytes(h);
  l.pcmOffset = kHeaderBytes + static_cast<std::uint32_t>(base::AlignUp64(l.hold, kStateAlignment));
  const std::uint32_t pcmBytes = kOpusPacketFrames * h.channels * static_cast<std::uint32_t>(sizeof(float));
  l.decoderOffset = l.pcmOffset + static_cast<std::uint32_t>(base::AlignUp64(pcmBytes, kStateAlignment));
  l.total = l.decoderOffset +
            static_cast<std::uint32_t>(base::AlignUp64(static_cast<std::uint32_t>(decBytes), kStateAlignment));
  *out = l;
  return true;
}

// Copies pending decoded frames to the output.
void Emit(OpusState* s, Output& out) noexcept {
  std::uint32_t k = s->pcmCount - s->pcmHead;
  if (out.max - out.written < k) k = out.max - out.written;
  if (k == 0u) return;
  const std::uint32_t ch = s->channels;
  const float* src = Pcm(s) + static_cast<std::size_t>(s->pcmHead) * ch;
  for (std::uint32_t c = 0; c < ch; ++c) {
    float* d = out.dst[c] + out.written;
    const float* q = src + c;
    for (std::uint32_t f = 0; f < k; ++f, q += ch) d[f] = *q;
  }
  s->pcmHead += k;
  out.written += k;
}

bool PcmPending(const OpusState* s) noexcept { return s->pcmHead < s->pcmCount; }

// Processes whole packets from buf[0, avail) (which starts at a packet
// boundary) while the output has room. *used = bytes of the packets processed.
// Stops at an incomplete packet, or when decoded frames are left over. False =
// corrupt data.
bool Run(OpusState* s, const std::uint8_t* buf, std::uint32_t avail, Output& out, std::uint32_t* used) noexcept {
  std::uint32_t u = 0;
  for (;;) {
    Emit(s, out);
    if (PcmPending(s) || out.written == out.max) break;
    if (avail - u < kOpusLengthPrefixBytes) break;
    const std::uint32_t len = base::LoadLE16(buf + u);
    if (len == 0u || len > s->maxPacket) return false;
    if (avail - u - kOpusLengthPrefixBytes < len) break;
    const std::uint8_t* packet = buf + u + kOpusLengthPrefixBytes;
    if (s->pos < s->end) {
      const int n = opus_multistream_decode_float(Decoder(s), packet, static_cast<opus_int32>(len), Pcm(s),
                                                  static_cast<int>(kOpusPacketFrames), 0);
      if (n != static_cast<int>(kOpusPacketFrames)) return false;  // libopus error or not a 20 ms packet
      const std::uint64_t skip = s->pos < s->preSkip ? s->preSkip - s->pos : 0u;
      const std::uint64_t left = s->end - s->pos;
      s->pcmCount = left < kOpusPacketFrames ? static_cast<std::uint32_t>(left) : kOpusPacketFrames;
      s->pcmHead = skip < s->pcmCount ? static_cast<std::uint32_t>(skip) : s->pcmCount;
    }
    // else: past the playable range; consumed without decoding.
    s->pos += kOpusPacketFrames;
    u += kOpusLengthPrefixBytes + len;
  }
  *used = u;
  return true;
}

std::uint32_t Fail(OpusState* s) noexcept {
  s->failed = kFailedRuntime;
  s->holdHead = s->holdTail = 0;
  s->pcmHead = s->pcmCount = 0;
  return kDecodeError;
}

bool Hold(OpusState* s, const std::uint8_t* src, std::uint32_t n) noexcept {
  std::uint8_t* hold = HoldBuffer(s);
  const std::uint32_t len = s->holdTail - s->holdHead;
  if (n > s->holdCapacity - len) return false;
  if (n > s->holdCapacity - s->holdTail) {  // compact
    std::memmove(hold, hold + s->holdHead, len);
    s->holdHead = 0;
    s->holdTail = len;
  }
  std::memcpy(hold + s->holdTail, src, n);
  s->holdTail += n;
  return true;
}

class OpusDecoderCodec final : public ICodec {
 public:
  constexpr OpusDecoderCodec() noexcept = default;

  std::uint32_t StateBytes(const CodecHeader& h) const override {
    OpusSetup setup;
    Layout l;
    if (h.codec != SND_CODEC_OPUS || ValidateHeader(h) != HeaderError::None ||
        ParseOpusSetup(h, &setup) != HeaderError::None || !ComputeLayout(h, setup, &l)) {
      return kMinStateBytes;
    }
    return l.total;
  }

  void Init(void* state, const CodecHeader& h) override {
    if (state == nullptr) return;
    auto* s = static_cast<OpusState*>(state);
    std::memset(s, 0, sizeof(OpusState));
    s->magic = detail::kStateMagic;
    s->codec = SND_CODEC_OPUS;
    OpusSetup setup;
    Layout l;
    if (h.codec != SND_CODEC_OPUS || ValidateHeader(h) != HeaderError::None ||
        ParseOpusSetup(h, &setup) != HeaderError::None || !ComputeLayout(h, setup, &l)) {
      s->failed = kFailedHeader;
      return;
    }
    s->channels = h.channels;
    s->holdCapacity = l.hold;
    s->maxPacket = OpusMaxPacketBytes(h);
    s->preSkip = h.pre_skip;
    s->pcmOffset = l.pcmOffset;
    s->decoderOffset = l.decoderOffset;
    s->end = static_cast<std::uint64_t>(h.pre_skip) + h.total_frames;
    if (opus_multistream_decoder_init(Decoder(s), static_cast<opus_int32>(kOpusSampleRate),
                                      static_cast<int>(h.channels), static_cast<int>(setup.streams),
                                      static_cast<int>(setup.coupledStreams), setup.mapping) != OPUS_OK) {
      s->failed = kFailedHeader;
    }
  }

  std::uint32_t Decode(void* state, const std::uint8_t* src, std::uint32_t n, float* const* dst,
                       std::uint32_t max) override {
    if (!OwnsState(state)) return kDecodeError;
    auto* s = static_cast<OpusState*>(state);
    if (s->failed != 0u) return kDecodeError;
    if (src == nullptr && n != 0u) return kDecodeError;
    if (max > 0u) {
      if (dst == nullptr) return kDecodeError;
      for (std::uint32_t c = 0; c < s->channels; ++c) {
        if (dst[c] == nullptr) return kDecodeError;
      }
    }
    Output out{dst, 0, max};
    std::uint8_t* hold = HoldBuffer(s);

    // 1. Held bytes first (they start at a packet boundary).
    while (s->holdTail > s->holdHead) {
      std::uint32_t used = 0;
      if (!Run(s, hold + s->holdHead, s->holdTail - s->holdHead, out, &used)) return Fail(s);
      s->holdHead += used;
      if (s->holdHead == s->holdTail) {
        s->holdHead = s->holdTail = 0;
        break;
      }
      if (PcmPending(s) || out.written == out.max) break;  // output full: new input is held below
      if (n == 0u) break;                                  // an incomplete packet waits for more input
      // The held bytes are one incomplete packet: complete it from the input.
      const std::uint32_t have = s->holdTail - s->holdHead;
      std::uint32_t need = kOpusLengthPrefixBytes - (have < kOpusLengthPrefixBytes ? have : kOpusLengthPrefixBytes);
      if (need == 0u) need = kOpusLengthPrefixBytes + base::LoadLE16(hold + s->holdHead) - have;
      const std::uint32_t take = need < n ? need : n;
      if (!Hold(s, src, take)) return Fail(s);  // cannot happen: capacity >= one packet
      src += take;
      n -= take;
    }
    // Pending frames that did not fit before this call come first even without held bytes.
    Emit(s, out);

    // 2. New input directly.
    if (s->holdTail == s->holdHead && n > 0u && !PcmPending(s)) {
      std::uint32_t used = 0;
      if (!Run(s, src, n, out, &used)) return Fail(s);
      src += used;
      n -= used;
    }

    // 3. The rest is kept compressed, starting at a packet boundary.
    if (n > 0u && !Hold(s, src, n)) return Fail(s);
    return out.written;
  }

  void Seek(void* state, std::uint64_t frame) override {
    if (!OwnsState(state)) {
      auto* f = static_cast<detail::DecoderState*>(state);
      if (f != nullptr && f->magic == detail::kStateMagic && f->failed == 0u) f->failed = kFailedRuntime;
      return;
    }
    auto* s = static_cast<OpusState*>(state);
    if (s->failed == kFailedHeader) return;
    s->failed = 0;
    s->holdHead = s->holdTail = 0;
    s->pcmHead = s->pcmCount = 0;
    if (opus_multistream_decoder_ctl(Decoder(s), OPUS_RESET_STATE) != OPUS_OK) {
      s->failed = kFailedRuntime;
      return;
    }
    if (frame == 0u) {
      s->pos = 0;
      return;
    }
    if (frame > UINT64_MAX - s->preSkip) {
      s->failed = kFailedRuntime;
      return;
    }
    s->pos = frame + s->preSkip;
    if (s->pos % kOpusPacketFrames != 0u) s->failed = kFailedRuntime;  // not a packet boundary
  }

 private:
  static bool OwnsState(const void* state) noexcept {
    const auto* s = static_cast<const OpusState*>(state);
    return s != nullptr && s->magic == detail::kStateMagic && s->codec == SND_CODEC_OPUS;
  }
};

constinit OpusDecoderCodec gOpus;

}  // namespace

ICodec* detail::OpusCodec() noexcept { return &gOpus; }

}  // namespace snd::codec

#else  // !SND_CODEC_OPUS_ENABLED

namespace snd::codec {
ICodec* detail::OpusCodec() noexcept { return nullptr; }
}  // namespace snd::codec

#endif
