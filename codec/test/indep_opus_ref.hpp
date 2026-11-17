// Reference code of the independent Opus tests (indep_opus_*_test.cpp and
// tests/integration/indep_opus_bank_test.cpp). Written from the specification
// only - bank-format.md §6.5 "Opus", CONTRACTS.md D-57, RFC 6716 §3 (TOC and
// frame packing), RFC 6716 Appendix B (self-delimiting framing) and RFC 7845
// §5.1.1 (channel mapping) - not from codec/src. It does not use libopus.
#ifndef SND_CODEC_TEST_INDEP_OPUS_REF_HPP
#define SND_CODEC_TEST_INDEP_OPUS_REF_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/ext/codec.hpp"
#include "snd/test/test.hpp"

#ifndef SND_REQUIRE_MSG
#define SND_REQUIRE_MSG(cond, ...)                                           \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::snd::test::ReportFailureF(__FILE__, __LINE__, #cond, __VA_ARGS__);   \
      return;                                                                \
    }                                                                        \
  } while (0)
#endif

namespace opusref {

using Planar = std::vector<std::vector<float>>;  // [channel][frame]

// Numbers of bank-format.md §6.5 (spelled out, not taken from codec.hpp).
inline constexpr std::uint32_t kRate = 48000;
inline constexpr std::uint32_t kPacket = 960;
inline constexpr std::uint32_t kPreSkip = 312;
inline constexpr std::uint32_t kPreRoll = 3840;
inline constexpr std::uint32_t kPerStream = 1277;
inline constexpr std::uint32_t kHold = 16u * 1024u;
inline constexpr double kTwoPi = 6.283185307179586;

// --- RFC 6716 §3.1: TOC byte -> samples per frame at 48 kHz ---------------------------
inline std::uint32_t TocFrameSamples(std::uint8_t toc) {
  const std::uint32_t config = toc >> 3;
  if (config < 12) {  // SILK-only: NB 0..3, MB 4..7, WB 8..11 -> 10, 20, 40, 60 ms
    static constexpr std::uint32_t k[4] = {480, 960, 1920, 2880};
    return k[config & 3u];
  }
  if (config < 16) return (config & 1u) ? 960u : 480u;  // Hybrid SWB/FB: 10, 20 ms
  static constexpr std::uint32_t k[4] = {120, 240, 480, 960};  // CELT: 2.5, 5, 10, 20 ms
  return k[config & 3u];
}

// RFC 6716 §3.2.1 frame length: 1 or 2 bytes. False if truncated.
inline bool ReadLength(const std::uint8_t* p, std::size_t avail, std::size_t* used, std::size_t* len) {
  if (avail < 1) return false;
  if (p[0] < 252) {
    *used = 1;
    *len = p[0];
    return true;
  }
  if (avail < 2) return false;
  *used = 2;
  *len = static_cast<std::size_t>(p[1]) * 4u + p[0];
  return true;
}

// One Opus packet (RFC 6716 §3.2), undelimited (it ends at p + avail) or self-
// delimited (Appendix B: it carries its own size). *consumed = bytes of the
// packet, *samples = total samples at 48 kHz. False if malformed.
inline bool ParsePacket(const std::uint8_t* p, std::size_t avail, bool selfDelimited, std::size_t* consumed,
                        std::uint32_t* samples) {
  if (avail < 1) return false;
  const std::uint8_t toc = p[0];
  const std::uint32_t fs = TocFrameSamples(toc);
  const std::uint32_t code = toc & 3u;
  std::size_t pos = 1;
  std::size_t total = 0;  // bytes of frame data (+ padding) after the headers
  std::uint32_t frames = 0;
  auto rd = [&](std::size_t* len) {
    std::size_t u = 0;
    if (!ReadLength(p + pos, avail - pos, &u, len)) return false;
    pos += u;
    return true;
  };
  if (code == 0 || code == 1) {
    frames = code == 0 ? 1u : 2u;
    if (selfDelimited) {
      std::size_t l = 0;
      if (!rd(&l)) return false;
      total = l * frames;
    } else {
      total = avail - pos;
      if (code == 1 && total % 2 != 0) return false;
    }
    if (code == 0 && total > 1275) return false;
    if (code == 1 && total / 2 > 1275) return false;
  } else if (code == 2) {
    frames = 2;
    std::size_t l1 = 0;
    if (!rd(&l1)) return false;
    if (selfDelimited) {
      std::size_t l2 = 0;
      if (!rd(&l2)) return false;
      total = l1 + l2;
    } else {
      if (avail - pos < l1) return false;
      total = avail - pos;
    }
  } else {  // code 3
    if (avail - pos < 1) return false;
    const std::uint8_t fc = p[pos++];
    const bool vbr = (fc & 0x80u) != 0;
    const bool padded = (fc & 0x40u) != 0;
    frames = fc & 0x3Fu;
    if (frames == 0) return false;
    std::size_t padding = 0;
    if (padded) {
      for (;;) {
        if (avail - pos < 1) return false;
        const std::uint8_t b = p[pos++];
        padding += b == 255 ? 254u : b;
        if (b != 255) break;
      }
    }
    std::size_t sum = 0;
    if (vbr) {
      for (std::uint32_t i = 0; i + 1 < frames; ++i) {
        std::size_t l = 0;
        if (!rd(&l)) return false;
        sum += l;
      }
      if (selfDelimited) {
        std::size_t l = 0;
        if (!rd(&l)) return false;
        sum += l;
      } else {
        if (avail - pos < sum + padding) return false;
        sum = avail - pos - padding;
      }
    } else {
      if (selfDelimited) {
        std::size_t l = 0;
        if (!rd(&l)) return false;
        sum = l * frames;
      } else {
        if (avail - pos < padding) return false;
        const std::size_t data = avail - pos - padding;
        if (data % frames != 0) return false;
        sum = data;
      }
    }
    total = sum + padding;
  }
  if (static_cast<std::uint64_t>(frames) * fs > 5760u) return false;  // 120 ms at most (RFC 6716 §3.2.5)
  if (avail - pos < total) return false;
  if (!selfDelimited && avail - pos != total) return false;
  *consumed = pos + total;
  *samples = frames * fs;
  return true;
}

// A multistream packet (RFC 7845 §5.1.1, bank-format.md §6.5): streams - 1
// self-delimited packets followed by one undelimited packet. *samples = the
// common duration; false if malformed or the streams disagree.
inline bool ParseMultistream(const std::uint8_t* p, std::size_t len, std::uint32_t streams, std::uint32_t* samples) {
  std::size_t off = 0;
  std::uint32_t dur = 0;
  for (std::uint32_t s = 0; s < streams; ++s) {
    const bool last = s + 1 == streams;
    std::size_t used = 0;
    std::uint32_t n = 0;
    if (off > len) return false;
    if (!ParsePacket(p + off, len - off, !last, &used, &n)) return false;
    if (s > 0 && n != dur) return false;
    dur = n;
    off += used;
  }
  if (off != len) return false;
  *samples = dur;
  return true;
}

// --- setup (ASST.extra*, bank-format.md §6.5) -----------------------------------------
struct Setup {
  std::uint32_t version = 0, channels = 0, streams = 0, coupled = 0;
  std::uint8_t mapping[8] = {};
};
// Checks the setup table of the spec; returns an empty string when valid.
inline std::string ParseSetup(const std::uint8_t* e, std::uint32_t bytes, std::uint32_t channels, Setup* out) {
  if (e == nullptr) return "no extra";
  if (channels < 1 || channels > 8) return "channels";
  if (bytes != 4u + channels) return "extraBytes != 4 + channels";
  Setup s;
  s.version = e[0];
  s.channels = e[1];
  s.streams = e[2];
  s.coupled = e[3];
  if (s.version != 1) return "version";
  if (s.channels != channels) return "channel count";
  if (s.streams < 1 || s.streams > 8) return "streams";
  if (s.coupled > s.streams) return "coupled";
  for (std::uint32_t i = 0; i < channels; ++i) {
    s.mapping[i] = e[4 + i];
    if (s.mapping[i] != 255 && s.mapping[i] >= s.streams + s.coupled) return "mapping";
  }
  *out = s;
  return "";
}

// --- data walk ------------------------------------------------------------------------
struct Packet {
  std::uint64_t offset;  // of the u16 length prefix, from the data start
  std::uint32_t length;  // payload bytes
  std::uint32_t samples; // decoded duration of the payload (RFC 6716 TOC)
};
// Walks `u16 LE length + payload` from data[0, bytes). Empty string when every
// packet is valid (1 <= length <= streams x 1277, payload parses, 960 samples).
inline std::string WalkPackets(const std::uint8_t* d, std::uint64_t bytes, std::uint32_t streams,
                               std::vector<Packet>* out) {
  std::uint64_t off = 0;
  while (off < bytes) {
    if (bytes - off < 2) return "truncated length prefix at " + std::to_string(off);
    const std::uint32_t len = static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8);
    if (len < 1 || len > streams * kPerStream) return "length " + std::to_string(len) + " at " + std::to_string(off);
    if (bytes - off - 2 < len) return "payload past the end at " + std::to_string(off);
    std::uint32_t n = 0;
    if (!ParseMultistream(d + off + 2, len, streams, &n)) return "malformed payload at " + std::to_string(off);
    if (n != kPacket) return "packet of " + std::to_string(n) + " samples at " + std::to_string(off);
    out->push_back(Packet{off, len, n});
    off += 2u + len;
  }
  return "";
}

// --- signals ---------------------------------------------------------------------------
inline Planar Tones(std::uint32_t channels, std::uint64_t frames, const std::vector<double>& hz, double amp,
                    std::uint32_t rate = kRate) {
  Planar p(channels, std::vector<float>(static_cast<std::size_t>(frames)));
  for (std::uint32_t c = 0; c < channels; ++c) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      p[c][static_cast<std::size_t>(i)] =
          static_cast<float>(amp * std::sin(kTwoPi * hz[c % hz.size()] * static_cast<double>(i) / rate));
    }
  }
  return p;
}

// Amplitude of `hz` in x[start, start + n) (single DFT bin; exact for whole periods).
inline double ToneAmp(const std::vector<float>& x, std::size_t start, std::size_t n, double hz) {
  double re = 0, im = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double ph = kTwoPi * hz * static_cast<double>(i) / kRate;
    re += x[start + i] * std::cos(ph);
    im -= x[start + i] * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

inline double Db(double r) { return 20.0 * std::log10(r); }

// --- encoding helper -------------------------------------------------------------------
struct Encoded {
  snd::CodecHeader h{};
  std::vector<std::uint8_t> setup;
  std::vector<std::uint8_t> data;
  std::uint64_t packets = 0;
};
inline bool Encode(const Planar& p, const snd::codec::encode::OpusSettings& st, Encoded* e) {
  namespace ce = snd::codec::encode;
  const std::uint32_t ch = static_cast<std::uint32_t>(p.size());
  const std::uint64_t frames = p.empty() ? 0 : p[0].size();
  std::vector<const float*> ptr;
  for (const auto& c : p) ptr.push_back(c.data());
  const std::uint64_t cap = ce::OpusMaxEncodedBytes(ch, frames);
  e->data.assign(static_cast<std::size_t>(cap), 0);
  auto out = std::make_unique<ce::OpusEncoded>();
  if (ce::EncodeOpus(st, ptr.data(), ch, kRate, frames, e->data.data(), e->data.size(), out.get()) !=
      ce::EncodeStatus::Ok) {
    return false;
  }
  e->data.resize(static_cast<std::size_t>(out->bytes));
  e->setup.assign(out->setup, out->setup + out->setupBytes);
  e->h = out->header;
  e->h.extra = e->setup.data();
  e->h.extra_bytes = static_cast<std::uint32_t>(e->setup.size());
  e->packets = out->packets;
  return true;
}

// --- decoding helpers ------------------------------------------------------------------
// 64-byte aligned state memory with guard zones on both sides.
class State {
 public:
  explicit State(std::size_t bytes) : bytes_(bytes) {
    raw_ = static_cast<std::uint8_t*>(::operator new(bytes + 128, std::align_val_t{64}));
    std::memset(raw_, 0xE7, bytes + 128);
  }
  ~State() { ::operator delete(raw_, std::align_val_t{64}); }
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  void* get() { return raw_ + 64; }
  bool Intact() const {
    for (std::size_t i = 0; i < 64; ++i) {
      if (raw_[i] != 0xE7 || raw_[64 + bytes_ + i] != 0xE7) return false;
    }
    return true;
  }

 private:
  std::uint8_t* raw_;
  std::size_t bytes_;
};

// Decoder driven with a fixed max_frames, following the protocol of snd/ext.h
// (drain until a call returns fewer than max_frames before the next input).
struct Dec {
  snd::ICodec* c;
  std::uint32_t channels;
  State mem;
  std::uint32_t maxFrames;
  std::vector<std::vector<float>> buf;
  std::vector<float*> ptr;
  Planar got;
  bool error = false;
  bool overMax = false;

  Dec(const snd::CodecHeader& h, std::uint32_t maxF)
      : c(snd::codec::BuiltinCodec(SND_CODEC_OPUS)), channels(h.channels), mem(c->StateBytes(h)), maxFrames(maxF),
        buf(h.channels, std::vector<float>(maxF + 1u)), got(h.channels) {
    for (auto& b : buf) ptr.push_back(b.data());
    c->Init(mem.get(), h);
  }
  std::uint32_t Call(const std::uint8_t* src, std::uint32_t n, std::uint32_t max) {
    const std::uint32_t r = c->Decode(mem.get(), src, n, ptr.data(), max);
    if (r == snd::kDecodeError) {
      error = true;
      return r;
    }
    if (r > max) {
      overMax = true;
      error = true;
      return snd::kDecodeError;
    }
    for (std::uint32_t ch = 0; ch < channels; ++ch) got[ch].insert(got[ch].end(), buf[ch].begin(), buf[ch].begin() + r);
    return r;
  }
  bool Feed(const std::uint8_t* src, std::uint32_t n) {
    std::uint32_t r = Call(src, n, maxFrames);
    while (r == maxFrames) r = Call(nullptr, 0, maxFrames);
    return r != snd::kDecodeError;
  }
  // Feeds data in chunks of `chunk` bytes, then drains.
  bool FeedAll(const std::uint8_t* d, std::size_t bytes, std::size_t chunk) {
    for (std::size_t off = 0; off < bytes; off += chunk) {
      const std::size_t n = bytes - off < chunk ? bytes - off : chunk;
      if (!Feed(d + off, static_cast<std::uint32_t>(n))) return false;
    }
    return Feed(nullptr, 0);
  }
  std::uint64_t Frames() const { return got.empty() ? 0 : got[0].size(); }
};

inline Planar DecodeAll(const Encoded& e, bool* ok, std::uint32_t maxFrames = 4096, std::size_t chunk = 4096) {
  Dec d(e.h, maxFrames);
  const bool fed = d.FeedAll(e.data.data(), e.data.size(), chunk);
  *ok = fed && d.mem.Intact();
  return d.got;
}

}  // namespace opusref

#endif  // SND_CODEC_TEST_INDEP_OPUS_REF_HPP
