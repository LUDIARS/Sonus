// Independent encoder / IMA ADPCM compatibility tests (written by a tester).
// A small IMA ADPCM reference (decoder and encoder, typed from the IMA
// recommended practice, not copied from codec/src) is used to check the
// built-in codec bit for bit; plus round-trip quality per signal type,
// determinism, EncodedBytes vs the bytes written, and seek tables against the
// real packet positions.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "codec_test_util.hpp"
#include "indep_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/test/test.hpp"

using namespace indep;
namespace enc = snd::codec::encode;
using codec_test::EncodeOrDie;
using codec_test::MakeSignal;
using codec_test::SameAudio;

namespace {

// --- IMA ADPCM reference -----------------------------------------------------------
const int kRefStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                          25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                          88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                          307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                          1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                          3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                          12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kRefIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct RefChannel {
  int pred;
  int index;
  int Expand(unsigned n) {
    const int step = kRefStep[index];
    int d = step >> 3;
    if (n & 4) d += step;
    if (n & 2) d += step >> 1;
    if (n & 1) d += step >> 2;
    pred = (n & 8) ? pred - d : pred + d;
    pred = std::clamp(pred, -32768, 32767);
    index = std::clamp(index + kRefIndex[n & 7], 0, 88);
    return pred;
  }
  unsigned Compress(int sample) {
    int step = kRefStep[index];
    int diff = sample - pred;
    unsigned n = 0;
    if (diff < 0) {
      n = 8;
      diff = -diff;
    }
    if (diff >= step) {
      n |= 4;
      diff -= step;
    }
    if (diff >= (step >> 1)) {
      n |= 2;
      diff -= step >> 1;
    }
    if (diff >= (step >> 2)) n |= 1;
    Expand(n);
    return n;
  }
};

// Decodes whole WAV-IMA blocks. Returns false for a step index > 88.
bool RefDecode(const std::uint8_t* data, std::size_t bytes, std::uint32_t ch, std::uint32_t blockAlign,
               std::uint64_t frames, Planar* out) {
  out->assign(ch, {});
  const std::uint32_t perCh = (blockAlign - 4 * ch) / (4 * ch) * 8;  // nibbles per channel
  for (std::size_t b = 0; (b + 1) * blockAlign <= bytes && (*out)[0].size() < frames; ++b) {
    const std::uint8_t* blk = data + b * blockAlign;
    std::vector<RefChannel> st(ch);
    for (std::uint32_t c = 0; c < ch; ++c) {
      st[c].pred = static_cast<std::int16_t>(blk[4 * c] | (blk[4 * c + 1] << 8));
      st[c].index = blk[4 * c + 2];
      if (st[c].index > 88) return false;
    }
    std::vector<std::vector<int>> s(ch);
    for (std::uint32_t c = 0; c < ch; ++c) s[c].push_back(st[c].pred);
    const std::uint8_t* p = blk + 4 * ch;
    for (std::uint32_t g = 0; g < perCh / 8; ++g) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (int i = 0; i < 4; ++i) {
          const std::uint8_t byte = *p++;
          s[c].push_back(st[c].Expand(byte & 15u));
          s[c].push_back(st[c].Expand(byte >> 4));
        }
      }
    }
    for (std::uint32_t i = 0; i < s[0].size() && (*out)[0].size() < frames; ++i) {
      for (std::uint32_t c = 0; c < ch; ++c) (*out)[c].push_back(static_cast<float>(s[c][i]) / 32768.0f);
    }
  }
  return true;
}

int RefS16(float x) {
  const double v = static_cast<double>(x) * 32768.0;
  if (v >= 32767.0) return 32767;
  if (v <= -32768.0) return -32768;
  return static_cast<int>(std::lrint(v));
}

double Snr(const Planar& ref, const Planar& got, std::uint64_t from = 0) {
  double s = 0, n = 0;
  for (std::size_t c = 0; c < ref.size(); ++c) {
    for (std::size_t i = from; i < ref[c].size(); ++i) {
      const double d = static_cast<double>(got[c][i]) - ref[c][i];
      s += static_cast<double>(ref[c][i]) * ref[c][i];
      n += d * d;
    }
  }
  if (n == 0) return 300.0;
  return 10.0 * std::log10(s / n);
}

}  // namespace

// Hand-computed IMA ADPCM vectors (mono and stereo, incl. clamping at both ends).
SND_TEST(IndepAdpcm, KnownVectors) {
  // pred 0, index 0; nibbles 7,7,0,8,F,1,4,C.
  const std::uint8_t a[8] = {0x00, 0x00, 0x00, 0x00, 0x77, 0x80, 0x1F, 0xC4};
  const int ea[9] = {0, 11, 41, 45, 42, -10, 12, 73, -1};
  // pred 32760, index 88; nibbles 7,F,F,0,0,8,3,B.
  const std::uint8_t b[8] = {0xF8, 0x7F, 88, 0x00, 0xF7, 0x0F, 0x80, 0xB3};
  const int eb[9] = {32760, 32767, -28669, -32768, -28673, -24949, -28334, -6791, -26377};
  for (int which = 0; which < 3; ++which) {
    const std::uint32_t ch = which == 2 ? 2u : 1u;
    std::vector<std::uint8_t> blk;
    if (which == 0) blk.assign(a, a + 8);
    if (which == 1) blk.assign(b, b + 8);
    if (which == 2) {  // stereo: headers, then 4 bytes of ch0, 4 bytes of ch1
      blk = {a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], a[4], a[5], a[6], a[7], b[4], b[5], b[6], b[7]};
    }
    snd::CodecHeader h = enc::MakeAdpcmHeader(ch, 48000, 9, 8);
    SND_REQUIRE_EQ(h.block_align, blk.size());
    SND_REQUIRE_EQ(snd::codec::PacketFrames(h), 9u);
    bool ok = false;
    const Planar p = OneShot(h, blk, &ok);
    SND_REQUIRE(ok);
    SND_REQUIRE_EQ(p[0].size(), 9u);
    for (int i = 0; i < 9; ++i) {
      const int* e0 = which == 1 ? eb : ea;
      SND_CHECK_EQ(p[0][static_cast<std::size_t>(i)], static_cast<float>(e0[i]) / 32768.0f);
      if (ch == 2) SND_CHECK_EQ(p[1][static_cast<std::size_t>(i)], static_cast<float>(eb[i]) / 32768.0f);
    }
  }
}

// Random valid WAV-IMA streams: the built-in decoder equals the reference.
SND_TEST(IndepAdpcm, DecoderMatchesReference) {
  Rng rng(77);
  for (int iter = 0; iter < 300; ++iter) {
    const std::uint32_t ch = 1 + rng.Below(8);
    const std::uint32_t bpc = 4u * (2u + rng.Below(300));  // 8..1204 bytes per channel
    snd::CodecHeader h = enc::MakeAdpcmHeader(ch, 48000, 0, bpc);
    SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    const std::uint32_t blocks = 1 + rng.Below(5);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(blocks) * h.block_align);
    for (auto& x : data) x = static_cast<std::uint8_t>(rng.Next());
    for (std::uint32_t bl = 0; bl < blocks; ++bl) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        data[bl * h.block_align + 4 * c + 2] = static_cast<std::uint8_t>(rng.Below(89));
      }
    }
    h.total_frames = static_cast<std::uint64_t>(blocks) * snd::codec::PacketFrames(h) - rng.Below(3);
    Planar ref;
    SND_REQUIRE(RefDecode(data.data(), data.size(), ch, h.block_align, h.total_frames, &ref));
    bool ok = false;
    const Planar got = OneShot(h, data, &ok);
    SND_CHECK(ok);
    SND_CHECK_MSG(SameAudio(got, ref), "iter %d ch %u block %u", iter, ch, h.block_align);
  }
}

// The encoder is standard IMA quantisation (README): block header = the
// first sample rounded to s16, nibbles = the IMA compressor from the header's
// (predictor, index), the final block padded with the last sample; and the
// header index is error-minimal over {previous block's final index, 0, 4, .., 88}.
SND_TEST(IndepAdpcm, EncoderIsStandardIma) {
  for (std::uint32_t ch : {1u, 2u, 5u}) {
    for (std::uint32_t bpc : {8u, 64u, 512u}) {
      const std::uint64_t frames = 9000 + ch;
      snd::CodecHeader h = enc::MakeAdpcmHeader(ch, 48000, frames, bpc);
      Planar src = MakeSignal(ch, frames, ch + bpc);
      for (std::uint64_t i = 3000; i < 3100; ++i) src[0][i] = (i & 1) ? 1.0f : -1.0f;  // stress
      const auto data = EncodeOrDie(h, src);
      const std::uint32_t fpb = snd::codec::PacketFrames(h);
      const std::uint64_t blocks = data.size() / h.block_align;
      std::vector<int> prevFinal(ch, 0);
      std::uint32_t nibbleMismatch = 0, headerMismatch = 0, notMinimal = 0;
      for (std::uint64_t b = 0; b < blocks; ++b) {
        const std::uint8_t* blk = data.data() + b * h.block_align;
        for (std::uint32_t c = 0; c < ch; ++c) {
          auto sample = [&](std::uint64_t k) {
            const std::uint64_t f = std::min<std::uint64_t>(b * fpb + k, frames - 1);
            return RefS16(src[c][f]);
          };
          const int pred = static_cast<std::int16_t>(blk[4 * c] | (blk[4 * c + 1] << 8));
          const int index = blk[4 * c + 2];
          if (pred != sample(0) || index > 88 || blk[4 * c + 3] != 0) ++headerMismatch;
          auto run = [&](int idx, bool compare, int* finalIndex) {
            RefChannel st{sample(0), idx};
            double err = 0;
            for (std::uint32_t k = 0; k + 1 < fpb; ++k) {
              const int s = sample(k + 1);
              const unsigned n = st.Compress(s);
              err += static_cast<double>(s - st.pred) * (s - st.pred);
              if (compare) {
                const std::uint8_t byte = blk[4 * ch + (k / 8) * 4 * ch + c * 4 + (k % 8) / 2];
                const unsigned stored = (k & 1) ? (byte >> 4) : (byte & 15u);
                if (stored != n) ++nibbleMismatch;
              }
            }
            if (finalIndex) *finalIndex = st.index;
            return err;
          };
          int fin = 0;
          const double chosen = run(index, true, &fin);
          double best = run(prevFinal[c], false, nullptr);
          for (int cand = 0; cand <= 88; cand += 4) best = std::min(best, run(cand, false, nullptr));
          if (chosen > best) ++notMinimal;
          prevFinal[c] = fin;
        }
      }
      SND_CHECK_MSG(headerMismatch == 0 && nibbleMismatch == 0 && notMinimal == 0,
                    "ch %u bpc %u: header %u nibble %u notMinimal %u", ch, bpc, headerMismatch, nibbleMismatch,
                    notMinimal);
    }
  }
}

// Round-trip quality per signal type (mono and stereo, default block).
SND_TEST(IndepAdpcm, RoundTripQualityPerSignal) {
  const std::uint64_t n = 48000;
  const double kPi = 3.14159265358979323846;
  Rng rng(5);
  struct Sig {
    const char* name;
    double minSnr;  // dB, < 0 = report only
  };
  const Sig sigs[] = {{"sine 440 Hz -6 dBFS", 35.0}, {"sine 1 kHz 0 dBFS", 30.0}, {"sine 8 kHz -6 dBFS", 15.0},
                      {"white noise -6 dBFS", 8.0},  {"square 100 Hz 0 dBFS", 8.0}, {"dc +1", -1.0}};
  for (std::uint32_t ch : {1u, 2u}) {
    for (std::size_t si = 0; si < sizeof(sigs) / sizeof(sigs[0]); ++si) {
      Planar src(ch, std::vector<float>(n));
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (std::uint64_t i = 0; i < n; ++i) {
          const double t = static_cast<double>(i) / 48000.0;
          double v = 0;
          switch (si) {
            case 0: v = 0.5 * std::sin(2 * kPi * 440 * t + c); break;
            case 1: v = std::sin(2 * kPi * 1000 * t + c); break;
            case 2: v = 0.5 * std::sin(2 * kPi * 8000 * t + c); break;
            case 3: v = 0.5 * rng.Unit(); break;
            case 4: v = (static_cast<std::uint64_t>(t * 200) & 1) ? 1.0 : -1.0; break;
            default: v = 1.0; break;
          }
          src[c][i] = static_cast<float>(v);
        }
      }
      const snd::CodecHeader h = enc::MakeAdpcmHeader(ch, 48000, n);
      const auto data = EncodeOrDie(h, src);
      bool ok = false;
      const Planar out = OneShot(h, data, &ok);
      SND_REQUIRE(ok);
      SND_REQUIRE_EQ(out[0].size(), n);
      SND_CHECK(AllFinite(out, 1.0f));
      // Compare against the s16-quantised source (ADPCM works on s16).
      Planar q = src;
      for (auto& cc : q) {
        for (auto& v : cc) v = static_cast<float>(RefS16(v)) / 32768.0f;
      }
      const double snr = Snr(q, out);
      snd::test::Note("ADPCM %uch %-22s SNR %6.1f dB", ch, sigs[si].name, snr);
      if (sigs[si].minSnr > 0) {
        SND_CHECK_MSG(snr > sigs[si].minSnr, "%uch %s: %.1f dB", ch, sigs[si].name, snr);
      } else {
        // DC is reproduced exactly (a zero nibble at step 7 changes nothing).
        SND_CHECK(SameAudio(out, q));
      }
    }
  }
}

// Silence is exact; an impulse's error dies out within ~150 frames.
SND_TEST(IndepAdpcm, SilenceAndImpulse) {
  for (std::uint32_t ch : {1u, 2u}) {
    const std::uint64_t n = 20000;
    Planar src(ch, std::vector<float>(n, 0.0f));
    const snd::CodecHeader h = enc::MakeAdpcmHeader(ch, 48000, n);
    bool ok = false;
    Planar out = OneShot(h, EncodeOrDie(h, src), &ok);
    SND_REQUIRE(ok);
    SND_CHECK(SameAudio(out, src));
    const std::uint64_t impulses[] = {1000, 5000, 5001, 12345};
    for (std::uint32_t c = 0; c < ch; ++c) {
      for (std::uint64_t at : impulses) src[c][at] = (c == 0) ? 1.0f : -1.0f;
    }
    out = OneShot(h, EncodeOrDie(h, src), &ok);
    SND_REQUIRE(ok);
    float worstTail = 0;
    for (std::uint32_t c = 0; c < ch; ++c) {
      for (std::uint64_t i = 0; i < n; ++i) {
        bool nearImpulse = false;
        for (std::uint64_t at : impulses) nearImpulse = nearImpulse || (i >= at && i < at + 150);
        if (!nearImpulse) worstTail = std::max(worstTail, std::fabs(out[c][i]));
      }
    }
    snd::test::Note("ADPCM %uch impulse: worst |y| outside 150 frames after an impulse = %g", ch, worstTail);
    SND_CHECK(worstTail < 0.002f);
  }
}

// Same input -> same bytes, also when several threads encode at once (pure).
SND_TEST(IndepEncode, DeterministicAcrossCallsAndThreads) {
  for (std::uint32_t codec : kCodecs) {
    snd::CodecHeader h = Hdr(codec, 3, 30000, 256);
    h.pre_skip = 77;
    const Planar src = MakeSignal(3, h.pre_skip + h.total_frames, 12);
    const auto first = EncodeOrDie(h, src);
    std::vector<std::vector<std::uint8_t>> outs(4, std::vector<std::uint8_t>(first.size(), 0));
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
      ts.emplace_back([&, t] {
        auto ptrs = codec_test::Ptrs(src);
        std::uint64_t written = 0;
        for (int rep = 0; rep < 3; ++rep) {
          if (enc::Encode(h, ptrs.data(), h.pre_skip + h.total_frames, outs[static_cast<std::size_t>(t)].data(),
                          outs[static_cast<std::size_t>(t)].size(), &written) != enc::EncodeStatus::Ok ||
              outs[static_cast<std::size_t>(t)] != first)
            bad.fetch_add(1);
        }
      });
    }
    for (auto& t : ts) t.join();
    SND_CHECK_EQ(bad.load(), 0);
  }
}

// EncodedBytes = the bytes Encode writes (formula from README), and Encode
// writes nothing past them. BufferTooSmall one byte short.
SND_TEST(IndepEncode, EncodedBytesMatchesOutput) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 8u}) {
      const snd::CodecHeader probe = Hdr(codec, ch, 0, 36);
      const std::uint32_t pf = snd::codec::PacketFrames(probe);
      const std::uint32_t pb = snd::codec::PacketBytes(probe);
      for (std::uint64_t total : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{pf} - 1 + (pf == 1 ? 1u : 0u),
                                  std::uint64_t{pf}, std::uint64_t{pf} + 1, std::uint64_t{10} * pf + 3}) {
        for (std::uint32_t pre : {0u, 3u}) {
          snd::CodecHeader h = probe;
          h.total_frames = total;
          h.pre_skip = pre;
          const std::uint64_t all = total + pre;
          const std::uint64_t expect = (all + pf - 1) / pf * pb;
          SND_CHECK_EQ(enc::EncodedBytes(h), expect);
          const Planar src = MakeSignal(ch, all, 2);
          auto ptrs = codec_test::Ptrs(src);
          std::vector<std::uint8_t> buf(static_cast<std::size_t>(expect) + 64, 0xCD);
          std::uint64_t written = 12345;
          SND_CHECK(enc::Encode(h, ptrs.data(), all, buf.data(), expect, &written) == enc::EncodeStatus::Ok);
          SND_CHECK_EQ(written, expect);
          bool guard = true;
          for (std::size_t i = static_cast<std::size_t>(expect); i < buf.size(); ++i) guard = guard && buf[i] == 0xCD;
          SND_CHECK_MSG(guard, "codec %08x ch %u total %llu: wrote past EncodedBytes", codec, ch,
                        static_cast<unsigned long long>(total));
          if (expect > 0) {
            SND_CHECK(enc::Encode(h, ptrs.data(), all, buf.data(), expect - 1, &written) ==
                      enc::EncodeStatus::BufferTooSmall);
            SND_CHECK(enc::Encode(h, ptrs.data(), all + 1, buf.data(), expect, &written) ==
                      enc::EncodeStatus::BadArguments);
          }
          // Non-finite anywhere (also inside pre_skip) is rejected.
          if (all > 0) {
            for (std::uint64_t at : {std::uint64_t{0}, all - 1}) {
              Planar bad = src;
              bad[ch - 1][at] = at == 0 ? std::numeric_limits<float>::quiet_NaN() : -INFINITY;
              auto bp = codec_test::Ptrs(bad);
              SND_CHECK(enc::Encode(h, bp.data(), all, buf.data(), expect, &written) ==
                        enc::EncodeStatus::NonFiniteInput);
            }
          }
        }
      }
    }
  }
  // No built-in encoder: Opus and custom fourccs.
  snd::CodecHeader o = Hdr(SND_CODEC_PCM_F32, 1, 10);
  o.codec = SND_CODEC_OPUS;
  float x[10] = {};
  const float* px = x;
  std::uint8_t out[64];
  std::uint64_t w = 0;
  SND_CHECK(enc::Encode(o, &px, 10, out, sizeof(out), &w) == enc::EncodeStatus::BadHeader);
  SND_CHECK_EQ(enc::EncodedBytes(o), 0u);
  o.codec = snd::Fourcc('A', 'B', 'C', 'D');
  SND_CHECK(enc::Encode(o, &px, 10, out, sizeof(out), &w) == enc::EncodeStatus::BadHeader);
}

// PCM_S16 rounding and clamping at the edges (README: nearest, clamp to [-32768, 32767]).
SND_TEST(IndepEncode, PcmS16Edges) {
  const float vals[] = {1.0f, -1.0f, 2.0f, -2.0f, 32767.4f / 32768.0f, 32766.6f / 32768.0f, -32767.6f / 32768.0f,
                        1.4f / 32768.0f, -1.6f / 32768.0f, 1e-30f, -0.0f};
  const int expect[] = {32767, -32768, 32767, -32768, 32767, 32767, -32768, 1, -2, 0, 0};
  const std::uint64_t n = sizeof(vals) / sizeof(vals[0]);
  const snd::CodecHeader h = Hdr(SND_CODEC_PCM_S16, 1, n);
  Planar src(1, std::vector<float>(vals, vals + n));
  const auto data = EncodeOrDie(h, src);
  for (std::uint64_t i = 0; i < n; ++i) {
    const int got = static_cast<std::int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
    SND_CHECK_MSG(got == expect[i], "value %g -> %d, want %d", static_cast<double>(vals[i]), got, expect[i]);
  }
}

// Seek tables: entry 0 = {0,0}; every other entry a packet boundary after
// pre_skip and before the end with byteOffset = its packet index * packet
// bytes; ascending, at least intervalFrames apart, and greedy (the first
// eligible boundary each time). Capacity truncation writes only `capacity`.
SND_TEST(IndepEncode, SeekTableProperties) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 7u}) {
      for (std::uint32_t pre : {0u, 1u, 200u, 1017u}) {
        snd::CodecHeader h = Hdr(codec, ch, 0, 64);
        const std::uint32_t pf = snd::codec::PacketFrames(h);
        const std::uint32_t pb = snd::codec::PacketBytes(h);
        h.total_frames = codec == SND_CODEC_ADPCM ? 50ull * pf + 5 : 3000;
        h.pre_skip = pre;
        for (std::uint64_t interval : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{pf} - 1 + (pf == 1 ? 1u : 0u),
                                       std::uint64_t{pf}, std::uint64_t{pf} + 1, std::uint64_t{1000},
                                       std::uint64_t{100000}, UINT64_MAX}) {
          const std::uint32_t count = enc::BuildSeekTable(h, interval, nullptr, nullptr, 0);
          SND_REQUIRE(count >= 1u);
          std::vector<std::uint64_t> f(count + 4, 0xABABABABull), o(count + 4, 0xABABABABull);
          SND_REQUIRE_EQ(enc::BuildSeekTable(h, interval, f.data(), o.data(), count), count);
          // Expected greedy table.
          std::vector<std::uint64_t> ef = {0}, eo = {0};
          std::uint64_t last = 0;
          for (std::uint64_t p = 1; p * pf < pre + h.total_frames; ++p) {
            if (p * pf <= pre) continue;
            const std::uint64_t fr = p * pf - pre;
            if (interval != 0 && fr - last < interval) continue;
            ef.push_back(fr);
            eo.push_back(p * pb);
            last = fr;
          }
          SND_CHECK_MSG(count == ef.size(), "codec %08x ch %u pre %u interval %llu: count %u want %zu", codec, ch, pre,
                        static_cast<unsigned long long>(interval), count, ef.size());
          const std::size_t m = std::min<std::size_t>(count, ef.size());
          SND_CHECK(std::equal(ef.begin(), ef.begin() + static_cast<std::ptrdiff_t>(m), f.begin()));
          SND_CHECK(std::equal(eo.begin(), eo.begin() + static_cast<std::ptrdiff_t>(m), o.begin()));
          for (std::size_t i = count; i < f.size(); ++i) SND_CHECK(f[i] == 0xABABABABull && o[i] == 0xABABABABull);
          // Truncated.
          if (count > 2) {
            std::vector<std::uint64_t> tf(count, 7), to(count, 7);
            SND_CHECK_EQ(enc::BuildSeekTable(h, interval, tf.data(), to.data(), 2), count);
            SND_CHECK(tf[0] == f[0] && tf[1] == f[1] && to[1] == o[1] && tf[2] == 7 && to[2] == 7);
          }
          std::vector<std::uint64_t> only(count + 1, 9);
          SND_CHECK_EQ(enc::BuildSeekTable(h, interval, only.data(), nullptr, count), count);
          SND_CHECK_EQ(enc::BuildSeekTable(h, interval, nullptr, only.data(), count), count);
        }
      }
    }
  }
  snd::CodecHeader bad = Hdr(SND_CODEC_ADPCM, 2, 100);
  bad.block_align = 5;
  SND_CHECK_EQ(enc::BuildSeekTable(bad, 0, nullptr, nullptr, 0), 0u);
}

// Seek-table byte offsets point at real packet starts: the encoded bytes from
// there decode to the same frames as the continuous decode (interval 1000).
SND_TEST(IndepEncode, SeekTableOffsetsDecodeCorrectly) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t pre : {0u, 333u}) {
      snd::CodecHeader h = Hdr(codec, 2, 20000, 256);
      h.pre_skip = pre;
      const auto data = EncodeOrDie(h, MakeSignal(2, pre + h.total_frames, 6));
      bool ok = false;
      const Planar ref = OneShot(h, data, &ok);
      SND_REQUIRE(ok);
      const std::uint32_t count = enc::BuildSeekTable(h, 1000, nullptr, nullptr, 0);
      std::vector<std::uint64_t> f(count), o(count);
      enc::BuildSeekTable(h, 1000, f.data(), o.data(), count);
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      Fenced mem(c->StateBytes(h));
      c->Init(mem.get(), h);
      for (std::uint32_t e = 0; e < count; ++e) {
        c->Seek(mem.get(), f[e]);
        Driver d(c, mem.get(), 2, 1024);
        Rng rng(e);
        SND_REQUIRE(d.FeedRandom(data.data() + o[e], data.size() - o[e], rng, snd::codec::kHoldInputBytes, 1024));
        SND_CHECK(SameAudio(d.got, SliceOf(ref, f[e], h.total_frames - f[e])));
      }
    }
  }
}
