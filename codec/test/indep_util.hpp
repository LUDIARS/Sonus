// Helpers of the independent codec tests (indep_*_test.cpp). Written from the
// specification (design.md 6.x, CONTRACTS.md D-14/D-23/D-31, snd/ext.h,
// codec/README.md) rather than from the implementation.
#ifndef SND_CODEC_TEST_INDEP_UTIL_HPP
#define SND_CODEC_TEST_INDEP_UTIL_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "codec_test_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/ext/codec.hpp"
#include "snd/test/test.hpp"

namespace indep {

using codec_test::Planar;
using codec_test::Rng;
using snd::CodecHeader;

inline constexpr std::uint32_t kCodecs[] = {SND_CODEC_PCM_F32, SND_CODEC_PCM_S16, SND_CODEC_ADPCM};

inline CodecHeader Hdr(std::uint32_t codec, std::uint32_t ch, std::uint64_t frames,
                       std::uint32_t adpcmBytesPerChannel = 64) {
  if (codec == SND_CODEC_ADPCM) return snd::codec::encode::MakeAdpcmHeader(ch, 48000, frames, adpcmBytesPerChannel);
  return snd::codec::encode::MakePcmHeader(codec, ch, 48000, frames);
}

// 64-byte aligned memory of exactly `bytes` with 64-byte guard zones on both sides.
class Fenced {
 public:
  static constexpr std::size_t kFence = 64;
  static constexpr std::uint8_t kGuard = 0xC3;
  explicit Fenced(std::size_t bytes) : bytes_(bytes) {
    raw_ = static_cast<std::uint8_t*>(::operator new(bytes + 2 * kFence, std::align_val_t{64}));
    std::memset(raw_, kGuard, kFence);
    std::memset(raw_ + kFence, 0x5A, bytes);
    std::memset(raw_ + kFence + bytes, kGuard, kFence);
  }
  ~Fenced() { ::operator delete(raw_, std::align_val_t{64}); }
  Fenced(const Fenced&) = delete;
  Fenced& operator=(const Fenced&) = delete;
  void* get() { return raw_ + kFence; }
  std::size_t size() const { return bytes_; }
  bool Intact() const {
    for (std::size_t i = 0; i < kFence; ++i) {
      if (raw_[i] != kGuard || raw_[kFence + bytes_ + i] != kGuard) return false;
    }
    return true;
  }

 private:
  std::uint8_t* raw_;
  std::size_t bytes_;
};

// Output buffers; before each call the frames [max, max + kTail) are armed with
// a sentinel bit pattern and checked afterwards (no write past max_frames).
class Out {
 public:
  static constexpr std::uint32_t kTail = 16;
  static constexpr std::uint32_t kSentinelBits = 0x7FA5A5A5u;
  Out(std::uint32_t channels, std::uint32_t capacity)
      : cap_(capacity), d_(channels, std::vector<float>(static_cast<std::size_t>(capacity) + kTail)) {
    for (auto& v : d_) p_.push_back(v.data());
  }
  std::uint32_t capacity() const { return cap_; }
  float* const* Arm(std::uint32_t max) {
    armed_ = max;
    float s;
    std::memcpy(&s, &kSentinelBits, 4);
    for (auto& v : d_) std::fill(v.begin() + max, v.begin() + max + kTail, s);
    return p_.data();
  }
  bool TailOk() const {
    for (const auto& v : d_) {
      for (std::uint32_t i = armed_; i < armed_ + kTail; ++i) {
        std::uint32_t b;
        std::memcpy(&b, &v[i], 4);
        if (b != kSentinelBits) return false;
      }
    }
    return true;
  }
  void Append(Planar& out, std::uint32_t n) const {
    if (out.size() != d_.size()) out.resize(d_.size());
    for (std::size_t c = 0; c < d_.size(); ++c) out[c].insert(out[c].end(), d_[c].begin(), d_[c].begin() + n);
  }
  const float* Channel(std::uint32_t c) const { return d_[c].data(); }

 private:
  std::uint32_t cap_;
  std::uint32_t armed_ = 0;
  std::vector<std::vector<float>> d_;
  std::vector<float*> p_;
};

// One decoder state driven by a test. Records the output and every anomaly.
struct Driver {
  snd::ICodec* codec;
  void* state;
  std::uint32_t channels;
  Out out;
  Planar got;
  bool error = false;     // a call returned SND_DECODE_ERROR
  bool tailOk = true;     // nothing written at or after dst[c][max]
  bool overMax = false;   // a call returned more than max_frames
  std::uint64_t calls = 0;

  Driver(snd::ICodec* c, void* s, std::uint32_t ch, std::uint32_t outCap)
      : codec(c), state(s), channels(ch), out(ch == 0 ? 1 : ch, outCap), got(ch == 0 ? 1 : ch) {}

  std::uint32_t Call(const std::uint8_t* src, std::uint32_t n, std::uint32_t max) {
    float* const* d = out.Arm(max);
    const std::uint32_t r = codec->Decode(state, src, n, d, max);
    ++calls;
    if (!out.TailOk()) tailOk = false;
    if (r == snd::kDecodeError) {
      error = true;
      return r;
    }
    if (r > max) {
      overMax = true;
      error = true;
      return snd::kDecodeError;
    }
    out.Append(got, r);
    return r;
  }
  // The documented protocol: after feeding, drain with src = NULL until a call
  // returns fewer than max frames. max >= 1.
  bool Feed(const std::uint8_t* src, std::uint32_t n, std::uint32_t max) {
    std::uint32_t r = Call(src, n, max);
    while (r == max) r = Call(nullptr, 0, max);
    return r != snd::kDecodeError;
  }
  // Feeds data[0, bytes) in random chunks (1..maxChunk bytes) with a random
  // max_frames (1..maxMax) per chunk, then drains. Returns false on error.
  bool FeedRandom(const std::uint8_t* data, std::size_t bytes, Rng& rng, std::uint32_t maxChunk,
                  std::uint32_t maxMax) {
    std::size_t off = 0;
    while (off < bytes) {
      std::uint32_t n = 1u + rng.Below(maxChunk);
      if (n > bytes - off) n = static_cast<std::uint32_t>(bytes - off);
      const std::uint32_t max = 1u + rng.Below(std::min(maxMax, out.capacity()));
      if (!Feed(data + off, n, max)) return false;
      off += n;
    }
    return Feed(nullptr, 0, 1u + rng.Below(std::min(maxMax, out.capacity())));
  }
  std::uint64_t Frames() const { return got.empty() ? 0 : got[0].size(); }
};

inline Planar SliceOf(const Planar& p, std::uint64_t from, std::uint64_t n) {
  Planar o(p.size());
  for (std::size_t c = 0; c < p.size(); ++c) {
    const std::uint64_t a = std::min<std::uint64_t>(from, p[c].size());
    const std::uint64_t b = std::min<std::uint64_t>(from + n, p[c].size());
    o[c].assign(p[c].begin() + static_cast<std::ptrdiff_t>(a), p[c].begin() + static_cast<std::ptrdiff_t>(b));
  }
  return o;
}

// Decodes everything in one call with ample room (reference output).
inline Planar OneShot(const CodecHeader& h, const std::vector<std::uint8_t>& data, bool* ok) {
  snd::ICodec* c = snd::codec::BuiltinCodec(h.codec);
  Fenced mem(c->StateBytes(h));
  c->Init(mem.get(), h);
  const std::uint32_t cap = static_cast<std::uint32_t>(std::min<std::uint64_t>(h.total_frames + 64u, 1u << 22));
  Driver d(c, mem.get(), h.channels, cap);
  const bool fed = d.Feed(data.empty() ? nullptr : data.data(), static_cast<std::uint32_t>(data.size()), cap);
  *ok = fed && d.tailOk && mem.Intact();
  return d.got;
}

inline bool AllFinite(const Planar& p, float bound) {
  for (const auto& c : p) {
    for (float v : c) {
      if (!std::isfinite(v) || std::fabs(v) > bound) return false;
    }
  }
  return true;
}

}  // namespace indep

#endif  // SND_CODEC_TEST_INDEP_UTIL_HPP
