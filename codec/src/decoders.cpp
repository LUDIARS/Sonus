// Built-in decoders: PCM float32, PCM int16, IMA ADPCM (design.md 6.4, D-23).
//
// One driver (DecodeImpl) implements the D-23 buffering for all three; the
// codec-specific part is a "run" function that decodes from a byte range that
// starts at the current packet boundary and stops when the output is full or
// the next frame's bytes are missing. See snd/codec/codec.hpp for the rules.
#include <bit>
#include <cstdint>
#include <cstring>

#include "detail.hpp"
#include "snd/codec/codec.hpp"

namespace snd::codec {
namespace {

using detail::DecoderState;

constexpr std::uint32_t kFailedRuntime = 1u;  // cleared by Seek / Init
constexpr std::uint32_t kFailedHeader = 2u;   // cleared by Init with a valid header only

struct Output {
  float* const* dst;
  std::uint32_t written;
  std::uint32_t max;
};

// Decodes from buf[0, avail), which starts at the current packet. *consumed =
// bytes of the packets that were completely processed. False = corrupt data.
using RunFn = bool (*)(DecoderState* s, const std::uint8_t* buf, std::uint32_t avail, Output& out,
                       std::uint32_t* consumed) noexcept;

// --- PCM ------------------------------------------------------------------------
template <bool kFloat>
bool RunPcm(DecoderState* s, const std::uint8_t* buf, std::uint32_t avail, Output& out,
            std::uint32_t* consumed) noexcept {
  const std::uint32_t frameBytes = s->packetBytes;
  const std::uint32_t frames = avail / frameBytes;
  const std::uint32_t channels = s->channels;
  std::uint32_t i = 0;
  while (i < frames) {
    if (s->pos < s->preSkip) {
      const std::uint64_t k64 = s->preSkip - s->pos;
      const std::uint32_t k = k64 < frames - i ? static_cast<std::uint32_t>(k64) : frames - i;
      i += k;
      s->pos += k;
      continue;
    }
    if (s->pos >= s->end) {  // past the playable range: consume and ignore
      i = frames;
      break;
    }
    std::uint32_t k = frames - i;
    if (s->end - s->pos < k) k = static_cast<std::uint32_t>(s->end - s->pos);
    if (out.max - out.written < k) k = out.max - out.written;
    if (k == 0u) break;
    const std::uint8_t* p = buf + static_cast<std::size_t>(i) * frameBytes;
    for (std::uint32_t c = 0; c < channels; ++c) {
      float* d = out.dst[c] + out.written;
      if constexpr (kFloat) {
        const std::uint8_t* q = p + c * 4u;
        for (std::uint32_t f = 0; f < k; ++f, q += frameBytes) {
          d[f] = std::bit_cast<float>(detail::LoadLE32(q));
        }
      } else {
        const std::uint8_t* q = p + c * 2u;
        for (std::uint32_t f = 0; f < k; ++f, q += frameBytes) {
          d[f] = static_cast<float>(static_cast<std::int16_t>(detail::LoadLE16(q))) * (1.0f / 32768.0f);
        }
      }
    }
    i += k;
    s->pos += k;
    out.written += k;
  }
  *consumed = i * frameBytes;
  return true;
}

// --- IMA ADPCM --------------------------------------------------------------------
// Frames of a block whose bytes are all within the first `avail` bytes.
std::uint32_t AdpcmAvailableFrames(std::uint32_t channels, std::uint32_t fpb, std::uint32_t avail) noexcept {
  const std::uint32_t head = 4u * channels;
  if (avail < head) return 0;
  const std::uint32_t rest = avail - head;
  const std::uint32_t groups = rest / head;
  const std::uint32_t partial = rest % head;  // bytes of the next, incomplete group
  std::uint32_t frames = 1u + groups * 8u;
  const std::uint32_t lastChannelStart = (channels - 1u) * 4u;
  if (partial > lastChannelStart) frames += 2u * (partial - lastChannelStart);
  return frames < fpb ? frames : fpb;
}

bool RunAdpcm(DecoderState* s, const std::uint8_t* buf, std::uint32_t avail, Output& out,
              std::uint32_t* consumed) noexcept {
  const std::uint32_t channels = s->channels;
  const std::uint32_t fpb = s->packetFrames;
  const std::uint32_t blockBytes = s->packetBytes;
  std::uint32_t used = 0;
  for (;;) {
    const std::uint8_t* blk = buf + used;
    const std::uint32_t have = avail - used;
    const std::uint32_t availFrames = AdpcmAvailableFrames(channels, fpb, have < blockBytes ? have : blockBytes);
    if (s->packetProgress == 0u && availFrames > 0u && s->pos < s->end) {
      for (std::uint32_t c = 0; c < channels; ++c) {
        const std::uint8_t* h = blk + 4u * c;
        const std::uint32_t index = h[2];
        if (index > kAdpcmMaxStepIndex) return false;
        s->predictor[c] = static_cast<std::int16_t>(detail::LoadLE16(h));
        s->stepIndex[c] = static_cast<std::int32_t>(index);
      }
    }
    while (s->packetProgress < availFrames) {
      if (s->pos >= s->end) {  // past the playable range: skip the rest of the block
        s->packetProgress = availFrames;
        break;
      }
      const bool emit = s->pos >= s->preSkip;
      if (emit && out.written == out.max) break;
      const std::uint32_t f = s->packetProgress;
      if (f > 0u) {
        const std::uint32_t k = f - 1u;
        for (std::uint32_t c = 0; c < channels; ++c) {
          const std::uint8_t b = blk[detail::AdpcmByteOffset(channels, c, k)];
          const std::uint32_t nibble = (k & 1u) ? (b >> 4) : (b & 15u);
          detail::ImaStep(s->predictor[c], s->stepIndex[c], nibble);
        }
      }
      if (emit) {
        for (std::uint32_t c = 0; c < channels; ++c) {
          out.dst[c][out.written] = static_cast<float>(s->predictor[c]) * (1.0f / 32768.0f);
        }
        ++out.written;
      }
      ++s->pos;
      ++s->packetProgress;
    }
    if (s->packetProgress == fpb && have >= blockBytes) {
      used += blockBytes;
      s->packetProgress = 0;
      continue;
    }
    break;
  }
  *consumed = used;
  return true;
}

// --- shared driver -------------------------------------------------------------------
std::uint32_t Fail(DecoderState* s) noexcept {
  s->failed = kFailedRuntime;
  s->holdHead = s->holdTail = 0;
  return kDecodeError;
}

bool Hold(DecoderState* s, const std::uint8_t* src, std::uint32_t n) noexcept {
  std::uint8_t* hold = detail::HoldBuffer(s);
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

std::uint32_t DecodeImpl(void* state, const std::uint8_t* src, std::uint32_t n, float* const* dst,
                         std::uint32_t max, RunFn run) noexcept {
  auto* s = static_cast<DecoderState*>(state);
  if (s == nullptr || s->magic != detail::kStateMagic) return kDecodeError;
  if (s->failed != 0u) return kDecodeError;
  if (src == nullptr && n != 0u) return kDecodeError;
  if (max > 0u) {
    if (dst == nullptr) return kDecodeError;
    for (std::uint32_t c = 0; c < s->channels; ++c) {
      if (dst[c] == nullptr) return kDecodeError;
    }
  }
  Output out{dst, 0, max};
  std::uint8_t* hold = detail::HoldBuffer(s);

  // 1. Held bytes first. They start at the current packet; a packet that is in
  //    progress (ADPCM) is always completed in the hold buffer.
  while (s->holdTail > s->holdHead) {
    std::uint32_t used = 0;
    if (!run(s, hold + s->holdHead, s->holdTail - s->holdHead, out, &used)) return Fail(s);
    s->holdHead += used;
    if (s->holdHead == s->holdTail) {
      s->holdHead = s->holdTail = 0;
      break;
    }
    const std::uint32_t have = s->holdTail - s->holdHead;
    if (have >= s->packetBytes) break;  // stopped because the output is full
    if (n == 0u) break;                 // a partial packet waits for more input
    std::uint32_t take = s->packetBytes - have;
    if (take > n) take = n;
    if (!Hold(s, src, take)) return Fail(s);  // cannot happen: capacity >= one packet
    src += take;
    n -= take;
  }

  // 2. New input directly (also when the output is full: pre-skip and
  //    past-the-end frames are processed without output).
  if (s->holdTail == s->holdHead && n > 0u) {
    std::uint32_t used = 0;
    if (!run(s, src, n, out, &used)) return Fail(s);
    src += used;
    n -= used;
  }

  // 3. Whatever is left is kept, starting at the current packet.
  if (n > 0u && !Hold(s, src, n)) return Fail(s);
  return out.written;
}

std::uint32_t StateBytesImpl(const CodecHeader& h) noexcept {
  if (ValidateHeader(h) != HeaderError::None) return kMinStateBytes;
  return detail::kStateHeaderBytes +
         static_cast<std::uint32_t>(detail::AlignUp(HoldCapacityBytes(h), kStateAlignment));
}

void InitImpl(void* state, const CodecHeader& h) noexcept {
  if (state == nullptr) return;
  auto* s = static_cast<DecoderState*>(state);
  std::memset(s, 0, sizeof(DecoderState));
  s->magic = detail::kStateMagic;
  s->codec = h.codec;
  if (ValidateHeader(h) != HeaderError::None) {
    s->failed = kFailedHeader;
    return;
  }
  s->channels = h.channels;
  s->packetBytes = PacketBytes(h);
  s->packetFrames = PacketFrames(h);
  s->holdCapacity = HoldCapacityBytes(h);
  s->preSkip = h.pre_skip;
  s->end = static_cast<std::uint64_t>(h.pre_skip) + h.total_frames;
}

void SeekImpl(void* state, std::uint64_t frame) noexcept {
  auto* s = static_cast<DecoderState*>(state);
  if (s == nullptr || s->magic != detail::kStateMagic || s->failed == kFailedHeader) return;
  s->failed = 0;
  s->holdHead = s->holdTail = 0;
  s->packetProgress = 0;
  if (frame == 0u) {
    s->pos = 0;
    return;
  }
  if (frame > UINT64_MAX - s->preSkip) {
    s->failed = kFailedRuntime;
    return;
  }
  s->pos = frame + s->preSkip;
  if (s->pos % s->packetFrames != 0u) s->failed = kFailedRuntime;  // not a packet boundary
}

class BuiltinDecoder final : public ICodec {
 public:
  constexpr BuiltinDecoder(std::uint32_t fourcc, RunFn run) noexcept : fourcc_(fourcc), run_(run) {}
  std::uint32_t StateBytes(const CodecHeader& h) const override {
    if (h.codec != fourcc_) return kMinStateBytes;
    return StateBytesImpl(h);
  }
  void Init(void* state, const CodecHeader& h) override {
    if (h.codec != fourcc_) {
      CodecHeader bad = h;
      bad.channels = 0;  // forces the invalid-header path
      InitImpl(state, bad);
      return;
    }
    InitImpl(state, h);
  }
  std::uint32_t Decode(void* state, const std::uint8_t* src, std::uint32_t n, float* const* dst,
                       std::uint32_t max) override {
    return DecodeImpl(state, src, n, dst, max, run_);
  }
  void Seek(void* state, std::uint64_t frame) override { SeekImpl(state, frame); }

 private:
  std::uint32_t fourcc_;
  RunFn run_;
};

constinit BuiltinDecoder gPcmF32{SND_CODEC_PCM_F32, &RunPcm<true>};
constinit BuiltinDecoder gPcmS16{SND_CODEC_PCM_S16, &RunPcm<false>};
constinit BuiltinDecoder gAdpcm{SND_CODEC_ADPCM, &RunAdpcm};

}  // namespace

ICodec* BuiltinCodec(std::uint32_t fourcc) noexcept {
  switch (fourcc) {
    case SND_CODEC_PCM_F32: return &gPcmF32;
    case SND_CODEC_PCM_S16: return &gPcmS16;
    case SND_CODEC_ADPCM: return &gAdpcm;
    default: return nullptr;
  }
}

}  // namespace snd::codec
