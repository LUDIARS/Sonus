// Helpers shared by the codec unit tests.
#ifndef SND_CODEC_TEST_UTIL_HPP
#define SND_CODEC_TEST_UTIL_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/ext/codec.hpp"
#include "snd/test/test.hpp"

namespace codec_test {

using Planar = std::vector<std::vector<float>>;  // [channel][frame]

inline constexpr std::uint8_t kGuard = 0xA5;
inline constexpr std::size_t kGuardBytes = 64;

// Deterministic LCG.
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
  std::uint32_t Next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(s >> 33);
  }
  std::uint32_t Below(std::uint32_t n) { return n == 0 ? 0 : Next() % n; }
  float Unit() { return static_cast<float>(Next()) / 2147483648.0f - 1.0f; }  // [-1, 1)
};

// Test signal: per-channel sines + a little noise + a transient, |x| <= ~0.9.
inline Planar MakeSignal(std::uint32_t channels, std::uint64_t frames, std::uint32_t seed = 1) {
  Planar p(channels, std::vector<float>(static_cast<std::size_t>(frames)));
  Rng rng(seed);
  for (std::uint32_t c = 0; c < channels; ++c) {
    const double f1 = 220.0 * (c + 1), f2 = 1375.0 + 97.0 * c;
    for (std::uint64_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / 48000.0;
      double v = 0.5 * std::sin(2.0 * 3.14159265358979 * f1 * t) + 0.2 * std::sin(2.0 * 3.14159265358979 * f2 * t);
      v += 0.02 * rng.Unit();
      if (i % 4000 == 1234) v = (c & 1) ? -0.9 : 0.9;  // transient
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
  }
  return p;
}

inline std::vector<const float*> Ptrs(const Planar& p) {
  std::vector<const float*> v;
  for (const auto& ch : p) v.push_back(ch.data());
  return v;
}

inline std::vector<std::uint8_t> EncodeOrDie(const snd::CodecHeader& h, const Planar& p) {
  const std::uint64_t bytes = snd::codec::encode::EncodedBytes(h);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
  std::uint64_t written = 0;
  auto ptrs = Ptrs(p);
  const std::uint64_t frames = p.empty() ? 0 : p[0].size();
  const auto st = snd::codec::encode::Encode(h, ptrs.data(), frames, out.data(), out.size(), &written);
  SND_CHECK_MSG(st == snd::codec::encode::EncodeStatus::Ok, "encode failed: %s",
                snd::codec::encode::EncodeStatusName(st));
  SND_CHECK_EQ(written, bytes);
  return out;
}

// 64-byte aligned state memory of exactly `bytes`, followed by guard bytes.
class StateMem {
 public:
  explicit StateMem(std::size_t bytes) : bytes_(bytes) {
    p_ = static_cast<std::uint8_t*>(::operator new(bytes + kGuardBytes, std::align_val_t{64}));
    std::memset(p_, 0x5A, bytes);
    std::memset(p_ + bytes, kGuard, kGuardBytes);
  }
  ~StateMem() { ::operator delete(p_, std::align_val_t{64}); }
  StateMem(const StateMem&) = delete;
  StateMem& operator=(const StateMem&) = delete;
  void* get() { return p_; }
  bool GuardIntact() const {
    for (std::size_t i = 0; i < kGuardBytes; ++i) {
      if (p_[bytes_ + i] != kGuard) return false;
    }
    return true;
  }

 private:
  std::uint8_t* p_;
  std::size_t bytes_;
};

// Output buffers of `max` frames per channel with trailing guards.
class OutBuf {
 public:
  OutBuf(std::uint32_t channels, std::uint32_t max) : max_(max), data_(channels) {
    for (auto& c : data_) c.assign(static_cast<std::size_t>(max) + 16u, 12345.0f);
    for (auto& c : data_) ptrs_.push_back(c.data());
  }
  float* const* dst() { return ptrs_.data(); }
  bool GuardIntact() const {
    for (const auto& c : data_) {
      for (std::size_t i = max_; i < c.size(); ++i) {
        if (c[i] != 12345.0f) return false;
      }
    }
    return true;
  }
  void AppendTo(Planar& out, std::uint32_t frames) const {
    for (std::size_t c = 0; c < data_.size(); ++c) {
      out[c].insert(out[c].end(), data_[c].begin(), data_[c].begin() + frames);
    }
  }

 private:
  std::uint32_t max_;
  std::vector<std::vector<float>> data_;
  std::vector<float*> ptrs_;
};

struct FeedResult {
  Planar out;
  bool error = false;
  bool guardsOk = true;
};

// Feeds data[offset, end) to an initialised state in chunks taken cyclically
// from `chunks` (0 entries = random 1..maxRandom), draining after every chunk
// until Decode returns < maxFrames (the documented protocol), then drains at
// the end. `skip` leading output frames are discarded (seek).
inline void Feed(snd::ICodec& codec, void* state, std::uint32_t channels, const std::uint8_t* data,
                 std::size_t bytes, const std::vector<std::uint32_t>& chunks, std::uint32_t maxFrames,
                 FeedResult* r, std::uint32_t seed = 7, std::uint32_t maxRandom = 5000) {
  OutBuf ob(channels, maxFrames);
  if (r->out.size() != channels) r->out.assign(channels, {});
  Rng rng(seed);
  std::size_t off = 0, ci = 0;
  auto take = [&](std::uint32_t n) {
    if (n == snd::kDecodeError) {
      r->error = true;
      return false;
    }
    if (n > maxFrames) {
      r->error = true;
      SND_CHECK_MSG(false, "Decode returned %u > max %u", n, maxFrames);
      return false;
    }
    ob.AppendTo(r->out, n);
    return true;
  };
  for (;;) {
    std::uint32_t n;
    if (off < bytes) {
      std::uint32_t want = chunks.empty() ? 1u + rng.Below(maxRandom) : chunks[ci++ % chunks.size()];
      if (want > bytes - off) want = static_cast<std::uint32_t>(bytes - off);
      n = codec.Decode(state, data + off, want, ob.dst(), maxFrames);
      off += want;
    } else {
      n = codec.Decode(state, nullptr, 0, ob.dst(), maxFrames);
    }
    if (!take(n)) break;
    // Drain until less than max came back.
    while (n == maxFrames && maxFrames > 0u) {
      n = codec.Decode(state, nullptr, 0, ob.dst(), maxFrames);
      if (!take(n)) break;
    }
    if (r->error) break;
    if (off >= bytes) break;
  }
  r->guardsOk = r->guardsOk && ob.GuardIntact();
}

inline FeedResult DecodeAll(const snd::CodecHeader& h, const std::uint8_t* data, std::size_t bytes,
                            const std::vector<std::uint32_t>& chunks, std::uint32_t maxFrames,
                            std::uint32_t seed = 7, std::uint32_t maxRandom = 5000) {
  snd::ICodec* codec = snd::codec::BuiltinCodec(h.codec);
  FeedResult r;
  r.out.assign(h.channels, {});
  if (codec == nullptr) {
    r.error = true;
    return r;
  }
  StateMem mem(codec->StateBytes(h));
  codec->Init(mem.get(), h);
  Feed(*codec, mem.get(), h.channels, data, bytes, chunks, maxFrames, &r, seed, maxRandom);
  r.guardsOk = r.guardsOk && mem.GuardIntact();
  return r;
}

inline bool SameAudio(const Planar& a, const Planar& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) return false;
    if (!a[c].empty() && std::memcmp(a[c].data(), b[c].data(), a[c].size() * sizeof(float)) != 0) return false;
  }
  return true;
}

inline std::uint64_t Frames(const Planar& p) { return p.empty() ? 0 : p[0].size(); }

// Signal-to-noise ratio in dB of `got` against `ref` over all channels.
inline double SnrDb(const Planar& ref, const Planar& got) {
  double s = 0, n = 0;
  for (std::size_t c = 0; c < ref.size(); ++c) {
    for (std::size_t i = 0; i < ref[c].size(); ++i) {
      const double d = static_cast<double>(got[c][i]) - ref[c][i];
      s += static_cast<double>(ref[c][i]) * ref[c][i];
      n += d * d;
    }
  }
  if (n == 0) return 200.0;
  return 10.0 * std::log10(s / n);
}

}  // namespace codec_test

#endif  // SND_CODEC_TEST_UTIL_HPP
