// Independent tests: Opus decoding against analytic expectations and the
// snd_codec.decode contract (snd/ext.h, CONTRACTS.md D-23 / D-51 / D-57 / D-60).
// Properties: scratchpad indep-opus-properties.md B1-B3, B5, C1-C7, D1-D5.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "indep_opus_ref.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/test/test.hpp"

namespace ce = snd::codec::encode;
using opusref::Encoded;
using opusref::Planar;

namespace {

std::uint64_t Lcg(std::uint64_t& s) {
  s = s * 6364136223846793005ull + 1442695040888963407ull;
  return s >> 33;
}

// Broadband test signal: 24 tones of random phase between 100 Hz and 8 kHz plus a
// little noise, |x| < 0.6. Different per channel (seed).
Planar Broadband(std::uint32_t ch, std::uint64_t frames, std::uint64_t seed) {
  Planar p(ch, std::vector<float>(static_cast<std::size_t>(frames)));
  for (std::uint32_t c = 0; c < ch; ++c) {
    std::uint64_t s = seed * 977u + c * 131u + 7u;
    double hz[24], ph[24];
    for (int k = 0; k < 24; ++k) {
      hz[k] = 100.0 + static_cast<double>(Lcg(s) % 7900u);
      ph[k] = static_cast<double>(Lcg(s) % 6283u) / 1000.0;
    }
    for (std::uint64_t i = 0; i < frames; ++i) {
      double v = 0;
      const double t = static_cast<double>(i) / 48000.0;
      for (int k = 0; k < 24; ++k) v += 0.022 * std::sin(opusref::kTwoPi * hz[k] * t + ph[k]);
      v += 0.02 * (static_cast<double>(Lcg(s) % 2001u) / 1000.0 - 1.0);
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
  }
  return p;
}

// Voice-like signal: 120..180 Hz glottal pulse train through two moving
// formant-ish resonances, syllable envelope, pauses. Used for SILK / hybrid.
Planar VoiceLike(std::uint64_t frames, std::uint64_t seed) {
  Planar p(1, std::vector<float>(static_cast<std::size_t>(frames)));
  std::uint64_t s = seed;
  double y1 = 0, y2 = 0, z1 = 0, z2 = 0, phase = 0;
  for (std::uint64_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / 48000.0;
    const double f0 = 150.0 + 30.0 * std::sin(opusref::kTwoPi * 0.7 * t);
    phase += f0 / 48000.0;
    double x = 0;
    if (phase >= 1.0) {
      phase -= 1.0;
      x = 1.0;
    }
    x += 0.05 * (static_cast<double>(Lcg(s) % 2001u) / 1000.0 - 1.0);
    const double f1 = 600.0 + 250.0 * std::sin(opusref::kTwoPi * 2.1 * t), f2 = 1700.0 + 500.0 * std::sin(opusref::kTwoPi * 1.3 * t);
    const double r = 0.995;
    const double a1 = 2 * r * std::cos(opusref::kTwoPi * f1 / 48000.0), b1 = 2 * r * std::cos(opusref::kTwoPi * f2 / 48000.0);
    const double y = x + a1 * y1 - r * r * y2;
    y2 = y1;
    y1 = y;
    const double z = x + b1 * z1 - r * r * z2;
    z2 = z1;
    z1 = z;
    const double env = std::max(0.0, std::sin(opusref::kTwoPi * 2.5 * t));  // syllables with pauses
    p[0][static_cast<std::size_t>(i)] = static_cast<float>(0.02 * env * (y + 0.6 * z));
  }
  return p;
}

bool SameBits(const Planar& a, const Planar& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) return false;
    if (!a[c].empty() && std::memcmp(a[c].data(), b[c].data(), a[c].size() * sizeof(float)) != 0) return false;
  }
  return true;
}

// Decodes from the seek point FindSeekPoint picks for `target`, drops the
// leading frames and returns what follows (up to the end).
Planar DecodeFrom(const Encoded& e, const std::vector<std::uint64_t>& fr, const std::vector<std::uint64_t>& off,
                  std::uint64_t target, std::uint32_t maxFrames, bool* ok) {
  *ok = false;
  const std::uint32_t idx = snd::codec::FindSeekPoint(fr.data(), static_cast<std::uint32_t>(fr.size()), target,
                                                      e.h.pre_roll, e.h.total_frames);
  if (idx == snd::codec::kNoSeekPoint) return {};
  if (fr[idx] > target || (target >= e.h.pre_roll && fr[idx] > target - e.h.pre_roll)) return {};
  opusref::Dec d(e.h, maxFrames);
  d.c->Seek(d.mem.get(), fr[idx]);
  if (!d.FeedAll(e.data.data() + off[idx], e.data.size() - off[idx], 3000)) return {};
  const std::uint64_t drop = target - fr[idx];
  Planar out(d.got.size());
  for (std::size_t c = 0; c < d.got.size(); ++c) {
    if (d.got[c].size() < drop) return {};
    out[c].assign(d.got[c].begin() + static_cast<std::ptrdiff_t>(drop), d.got[c].end());
  }
  *ok = d.mem.Intact();
  return out;
}

double MaxAbsDiff(const Planar& a, const Planar& b, std::uint64_t from, std::uint64_t n) {
  double m = 0;
  for (std::size_t c = 0; c < a.size(); ++c) {
    for (std::uint64_t i = 0; i < n; ++i) m = std::max(m, std::fabs(static_cast<double>(a[c][from + i]) - b[c][i]));
  }
  return m;
}

}  // namespace

// B1: single tones keep their level and the waveform (SNR) at the default bitrate.
SND_TEST(IndepOpusDecode, ToneLevelAndSnr) {
  if (!snd::codec::OpusAvailable()) return;
  const double tones[] = {100, 300, 1000, 3000, 6000, 10000};
  for (double hz : tones) {
    const Planar in = opusref::Tones(1, 24000, {hz}, 0.25);
    Encoded e;
    SND_REQUIRE(opusref::Encode(in, ce::OpusSettings{}, &e));
    bool ok = false;
    const Planar out = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    SND_REQUIRE_EQ(out[0].size(), 24000u);
    const std::size_t a = 4800, n = 19200;  // 0.4 s: whole periods of every tone
    const double gain = opusref::Db(opusref::ToneAmp(out[0], a, n, hz) / opusref::ToneAmp(in[0], a, n, hz));
    double sig = 0, err = 0;
    for (std::size_t i = a; i < a + n; ++i) {
      sig += static_cast<double>(in[0][i]) * in[0][i];
      const double d = static_cast<double>(out[0][i]) - in[0][i];
      err += d * d;
    }
    const double snr = 10.0 * std::log10(sig / std::max(err, 1e-30));
    std::printf("  %6.0f Hz: gain %+.2f dB, SNR %.1f dB\n", hz, gain, snr);
    SND_CHECK_MSG(std::fabs(gain) < 1.0, "%.0f Hz: gain %.2f dB", hz, gain);
    SND_CHECK_MSG(snr > 20.0, "%.0f Hz: SNR %.1f dB", hz, snr);
  }
}

// B2: no delay - playable frame n is input frame n (pre-skip applied), for 1, 2 and 6 ch.
SND_TEST(IndepOpusDecode, ZeroDelay) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    const Planar in = Broadband(ch, 19200, ch);
    Encoded e;
    SND_REQUIRE(opusref::Encode(in, ce::OpusSettings{}, &e));
    bool ok = false;
    const Planar out = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    for (std::uint32_t c = 0; c < ch; ++c) {
      if (ch == 6 && c == 3) continue;  // LFE (WAV index 3) is band limited by the surround encoder
      // Cross-correlation over two windows: the start (incl. frame 0) and the middle.
      for (std::size_t start : {std::size_t{0}, std::size_t{9600}}) {
        int best = 99;
        double bestV = -1e300;
        for (int lag = -64; lag <= 64; ++lag) {
          double v = 0;
          for (std::size_t i = 64; i < 4096 + 64; ++i) v += static_cast<double>(in[c][start + i]) * out[c][start + i + lag];
          if (v > bestV) {
            bestV = v;
            best = lag;
          }
        }
        SND_CHECK_MSG(best == 0, "%u ch, channel %u, window %zu: lag %d", ch, c, start, best);
      }
    }
  }
}

// B3: each channel keeps its own signal, in WAV order, for 1..8 channels.
SND_TEST(IndepOpusDecode, ChannelIdentity1To8) {
  if (!snd::codec::OpusAvailable()) return;
  // WAV order L R C LFE BL BR SL SR: LFE (index 3) gets a low tone.
  const double hz[8] = {330, 550, 770, 60, 1210, 1430, 1650, 1870};
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    std::vector<double> f(hz, hz + ch);
    if (ch == 3) f[2] = 770;
    const Planar in = opusref::Tones(ch, 24000, f, 0.3);
    Encoded e;
    SND_REQUIRE(opusref::Encode(in, ce::OpusSettings{}, &e));
    bool ok = false;
    const Planar out = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    SND_REQUIRE_EQ(out.size(), ch);
    for (std::uint32_t c = 0; c < ch; ++c) {
      const double own = opusref::ToneAmp(out[c], 4800, 19200, f[c]);
      SND_CHECK_MSG(std::fabs(opusref::Db(own / 0.3)) < 2.0, "%u ch: channel %u level %.2f dB", ch, c,
                    opusref::Db(own / 0.3));
      for (std::uint32_t o = 0; o < ch; ++o) {
        if (o == c) continue;
        const double leak = opusref::ToneAmp(out[c], 4800, 19200, f[o]);
        SND_CHECK_MSG(leak < own * 0.1, "%u ch: channel %u has %.4f of channel %u's tone (own %.4f)", ch, c, leak, o,
                      own);
      }
    }
  }
}

// B5 + C5: exactly total_frames come out, bit-identical for every chunking of
// the input and every max_frames.
SND_TEST(IndepOpusDecode, ExactLengthAndChunkingInvariance) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t totals[] = {0, 1, 647, 648, 649, 960, 1607, 1608, 1609, 9601};
  struct Way {
    std::uint32_t max;
    std::size_t chunk;
  };
  const Way ways[] = {{1, 1}, {7, 13}, {960, 960}, {961, 3}, {4096, 16384}, {100000, 1u << 20}};
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    for (std::uint64_t total : totals) {
      if (ch == 6 && total != 1609 && total != 9601) continue;
      Encoded e;
      SND_REQUIRE(opusref::Encode(Broadband(ch, total, total + ch), ce::OpusSettings{}, &e));
      bool ok = false;
      const Planar ref = opusref::DecodeAll(e, &ok, 65536, 1u << 20);
      SND_REQUIRE(ok);
      SND_CHECK_MSG(ref[0].size() == total, "%u ch: %zu frames for total %llu", ch, ref[0].size(),
                    static_cast<unsigned long long>(total));
      for (const Way& w : ways) {
        if (w.max == 1 && total > 2000) continue;  // keep it fast
        bool ok2 = false;
        const Planar got = opusref::DecodeAll(e, &ok2, w.max, w.chunk);
        SND_CHECK_MSG(ok2, "%u ch total %llu max %u chunk %zu", ch, static_cast<unsigned long long>(total), w.max,
                      w.chunk);
        SND_CHECK_MSG(SameBits(ref, got), "%u ch total %llu: max %u chunk %zu differs", ch,
                      static_cast<unsigned long long>(total), w.max, w.chunk);
      }
      // Random chunks and random max_frames per call.
      std::uint64_t s = total * 31u + ch;
      opusref::Dec d(e.h, 4096);
      std::size_t off = 0;
      bool good = true;
      while (off < e.data.size() && good) {
        const std::size_t n = std::min<std::size_t>(1u + Lcg(s) % 3000u, e.data.size() - off);
        const std::uint32_t max = 1u + static_cast<std::uint32_t>(Lcg(s) % 4096u);
        std::uint32_t r = d.Call(e.data.data() + off, static_cast<std::uint32_t>(n), max);
        while (r == max) r = d.Call(nullptr, 0, max);
        good = r != snd::kDecodeError;
        off += n;
      }
      std::uint32_t r = 0;
      while (good && (r = d.Call(nullptr, 0, 333)) == 333u) {
      }
      SND_CHECK(good && r != snd::kDecodeError);
      SND_CHECK_MSG(SameBits(ref, d.got), "%u ch total %llu: random chunking differs", ch,
                    static_cast<unsigned long long>(total));
    }
  }
}

// C2 / C6: draining returns only what is held and does not end the stream.
SND_TEST(IndepOpusDecode, DrainIsNotTheEnd) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Broadband(2, 9600, 3), ce::OpusSettings{}, &e));
  bool ok = false;
  const Planar ref = opusref::DecodeAll(e, &ok);
  SND_REQUIRE(ok);
  for (std::size_t cut : {std::size_t{1}, std::size_t{2}, std::size_t{3}, e.data.size() / 3, e.data.size() - 1}) {
    opusref::Dec d(e.h, 512);
    SND_CHECK_EQ(d.Call(nullptr, 0, 512), 0u);  // fresh state: nothing held
    SND_CHECK_EQ(d.Call(nullptr, 0, 0), 0u);    // max_frames 0
    SND_REQUIRE(d.Feed(e.data.data(), static_cast<std::uint32_t>(cut)));
    for (int i = 0; i < 5; ++i) SND_CHECK_EQ(d.Call(nullptr, 0, 512), 0u);  // repeated drains: nothing more
    SND_REQUIRE(d.Feed(e.data.data() + cut, static_cast<std::uint32_t>(e.data.size() - cut)));
    SND_CHECK_MSG(SameBits(ref, d.got), "cut %zu", cut);
    SND_CHECK(!d.error);
  }
  // Feeding with max_frames 0 holds everything; drains then return it all.
  opusref::Dec d(e.h, 700);
  const std::uint32_t part = 4000 < e.data.size() ? 4000u : static_cast<std::uint32_t>(e.data.size());
  SND_CHECK_EQ(d.Call(e.data.data(), part, 0), 0u);
  std::uint32_t r;
  while ((r = d.Call(nullptr, 0, 700)) == 700u) {
  }
  SND_CHECK(r != snd::kDecodeError);
  SND_REQUIRE(d.Feed(e.data.data() + part, static_cast<std::uint32_t>(e.data.size() - part)));
  SND_CHECK(SameBits(ref, d.got));
}

// C3: the holding limit of D-51 / D-58 is "16 KiB + one largest packet with its
// prefix" of undecoded input; one byte more is SND_DECODE_ERROR until Init / Seek.
SND_TEST(IndepOpusDecode, HoldLimitIsExact) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 8u}) {
    ce::OpusSettings st;
    st.bitrate = 510000u * ch;  // large packets
    st.vbr = ce::OpusVbr::Cbr;
    Encoded e;
    SND_REQUIRE(opusref::Encode(Broadband(ch, 48000, 9), st, &e));
    const std::uint32_t streams = e.setup[2];
    const std::uint32_t bound = opusref::kHold + 2u + streams * opusref::kPerStream;
    SND_CHECK_EQ(snd::codec::HoldCapacityBytes(e.h), bound);
    SND_REQUIRE(e.data.size() > bound + 1u);
    opusref::Dec d(e.h, 960);
    // max_frames = 0: nothing can be decoded, so everything given is held.
    std::uint32_t given = 0;
    while (given < bound) {
      const std::uint32_t n = std::min<std::uint32_t>(1000u, bound - given);
      SND_REQUIRE_EQ(d.Call(e.data.data() + given, n, 0), 0u);
      given += n;
    }
    SND_CHECK_MSG(d.Call(e.data.data() + given, 1, 0) == snd::kDecodeError, "%u ch: bound %u + 1 accepted", ch, bound);
    SND_CHECK_EQ(d.Call(nullptr, 0, 960), snd::kDecodeError);  // sticky
    // Seek(0) clears it; the protocol of snd/ext.h (<= 16 KiB per call, drain
    // until < max_frames) never hits the limit.
    d.error = false;
    d.c->Seek(d.mem.get(), 0);
    d.got.assign(ch, {});
    for (std::uint32_t max : {1u, 64u, 960u}) {
      d.c->Init(d.mem.get(), e.h);
      d.got.assign(ch, {});
      d.error = false;
      d.maxFrames = max;
      for (std::size_t off = 0; off < e.data.size(); off += opusref::kHold) {
        const std::size_t n = std::min<std::size_t>(opusref::kHold, e.data.size() - off);
        SND_REQUIRE_MSG(d.Feed(e.data.data() + off, static_cast<std::uint32_t>(n)), "%u ch max %u at %zu", ch, max, off);
      }
      SND_REQUIRE(d.Feed(nullptr, 0));
      SND_CHECK_EQ(d.Frames(), 48000u);
    }
    // With room for everything, any amount can be given at once.
    bool ok = false;
    const Planar all = opusref::DecodeAll(e, &ok, 60000, e.data.size());
    SND_CHECK(ok && all[0].size() == 48000u);
  }
}

// D-23 / snd/ext.h: "the end is fixed by header.total_frames (input past the
// end is consumed and dropped)". After the last playable frame, whatever follows
// - another packet or the zero padding of a stream file's last sector (6.3) -
// must not turn the call into an error (the frames of that call would be lost).
SND_TEST(IndepOpusDecode, InputPastTheEndIsDropped) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Broadband(2, 4800, 5), ce::OpusSettings{}, &e));
  bool ok = false;
  const Planar ref = opusref::DecodeAll(e, &ok);
  SND_REQUIRE(ok);
  // (a) One more valid packet after the end: consumed, nothing emitted.
  {
    std::vector<std::uint8_t> d = e.data;
    const std::uint32_t l0 = d[0] | (d[1] << 8);
    d.insert(d.end(), e.data.begin(), e.data.begin() + 2 + l0);
    opusref::Dec x(e.h, 100000);
    SND_CHECK(x.Feed(d.data(), static_cast<std::uint32_t>(d.size())));
    SND_CHECK(SameBits(ref, x.got));
  }
  // (b) Sector padding (zeros) in the same call as the last packets.
  {
    std::vector<std::uint8_t> d = e.data;
    d.resize((d.size() / 2048 + 1) * 2048, 0);
    opusref::Dec x(e.h, 100000);
    const std::uint32_t r = x.Call(d.data(), static_cast<std::uint32_t>(d.size()), 100000);
    SND_CHECK_MSG(r == 4800u, "data + zero padding in one call returned %u (want 4800)", r);
  }
  // (c) Padding given after everything was decoded.
  {
    opusref::Dec x(e.h, 100000);
    SND_REQUIRE(x.Feed(e.data.data(), static_cast<std::uint32_t>(e.data.size())));
    const std::vector<std::uint8_t> zeros(512, 0);
    const std::uint32_t r = x.Call(zeros.data(), 512, 1000);
    SND_CHECK_MSG(r == 0u, "zero padding after the end returned %u (want 0)", r);
  }
}

// D1 / D2 / D5: seeks through the seek table with the pre-roll are sample exact
// with the default (independent) CELT packets.
SND_TEST(IndepOpusDecode, SeekIsSampleExactForCelt) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    const std::uint64_t total = ch == 6 ? 24000 : 96000 + 123;
    Encoded e;
    SND_REQUIRE(opusref::Encode(Broadband(ch, total, 40 + ch), ce::OpusSettings{}, &e));
    bool ok = false;
    const Planar ref = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    const std::uint32_t n = ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, nullptr, nullptr, 0);
    std::vector<std::uint64_t> fr(n), off(n);
    ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, fr.data(), off.data(), n);
    std::vector<std::uint64_t> targets = {0, 1, 647, 648, 649, 3839, 3840, 3841, 4487, 4488, 4489, total - 1, total - 2,
                                          total - 960, total - 961};
    for (std::uint64_t k = 5; k * 960 < total; k += 17) {
      const std::uint64_t b = k * 960 - 312;  // a packet boundary
      targets.insert(targets.end(), {b - 1, b, b + 1});
    }
    std::uint64_t s = ch;
    for (int i = 0; i < 10; ++i) targets.push_back(Lcg(s) % total);
    double worst = 0;
    for (std::uint64_t t : targets) {
      bool ok2 = false;
      const Planar got = DecodeFrom(e, fr, off, t, 1 + static_cast<std::uint32_t>(t % 2000), &ok2);
      SND_REQUIRE_MSG(ok2, "%u ch: seek to %llu failed", ch, static_cast<unsigned long long>(t));
      SND_REQUIRE_MSG(got[0].size() == total - t, "%u ch: seek %llu gave %zu frames, want %llu", ch,
                      static_cast<unsigned long long>(t), got[0].size(), static_cast<unsigned long long>(total - t));
      const double m = MaxAbsDiff(ref, got, t, total - t);
      worst = std::max(worst, m);
      SND_CHECK_MSG(m == 0.0, "%u ch: seek to %llu differs by %g", ch, static_cast<unsigned long long>(t), m);
    }
    std::printf("  %u ch: %zu seeks, worst %g\n", ch, targets.size(), worst);
    // Past the range: no seek point; a seek beyond the data gives nothing.
    SND_CHECK_EQ(snd::codec::FindSeekPoint(fr.data(), n, total, e.h.pre_roll, total), snd::codec::kNoSeekPoint);
  }
}

// D4: Seek(0) restarts with pre-skip; Seek(f) with f + pre_skip not on a packet
// boundary is refused (the next Decode fails, D-57 "それ以外を拒む").
SND_TEST(IndepOpusDecode, SeekZeroAndMisalignedSeek) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Broadband(1, 9600, 77), ce::OpusSettings{}, &e));
  bool ok = false;
  const Planar ref = opusref::DecodeAll(e, &ok);
  SND_REQUIRE(ok);
  opusref::Dec d(e.h, 1000);
  SND_REQUIRE(d.Feed(e.data.data(), static_cast<std::uint32_t>(e.data.size() / 2)));  // part of the stream
  d.c->Seek(d.mem.get(), 0);
  d.got.assign(1, {});
  SND_REQUIRE(d.FeedAll(e.data.data(), e.data.size(), 777));
  SND_CHECK(SameBits(ref, d.got));
  for (std::uint64_t f : {1u, 312u, 647u, 649u, 960u, 1919u}) {
    d.c->Seek(d.mem.get(), f);
    d.error = false;
    const std::uint32_t r = d.Call(e.data.data(), 200, 1000);
    SND_CHECK_MSG(r == snd::kDecodeError, "Seek(%llu) accepted (returned %u)", static_cast<unsigned long long>(f), r);
  }
  // A valid seek recovers from the refused one.
  d.c->Seek(d.mem.get(), 648);  // 648 + 312 = 960
  d.error = false;
  d.got.assign(1, {});
  const std::uint32_t l0 = e.data[0] | (e.data[1] << 8);
  SND_REQUIRE(d.FeedAll(e.data.data() + 2 + l0, e.data.size() - 2 - l0, 4096));
  SND_CHECK_EQ(d.Frames(), 9600u - 648u);
}

// D3: SILK / hybrid (voice at low bitrates) converge after the pre-roll. The
// spec only promises an approximation (observed max error 1.5e-2 .. 3.4e-2);
// this checks a loose bound and prints the measured error.
SND_TEST(IndepOpusDecode, SeekApproximationForVoice) {
  if (!snd::codec::OpusAvailable()) return;
  const Planar in = VoiceLike(96000, 3);
  for (std::uint32_t bitrate : {12000u, 16000u, 24000u, 32000u}) {
    ce::OpusSettings st;
    st.bitrate = bitrate;
    st.signal = ce::OpusSignal::Voice;
    Encoded e;
    SND_REQUIRE(opusref::Encode(in, st, &e));
    bool ok = false;
    const Planar ref = opusref::DecodeAll(e, &ok);
    SND_REQUIRE(ok);
    const std::uint32_t n = ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, nullptr, nullptr, 0);
    std::vector<std::uint64_t> fr(n), off(n);
    ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, fr.data(), off.data(), n);
    double worst = 0, peak = 0;
    for (float v : ref[0]) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    std::uint32_t silkish = 0;
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint8_t toc = e.data[off[k] + 2];
      silkish += (toc >> 3) < 16 ? 1u : 0u;
    }
    for (std::uint64_t t = 4000; t + 2000 < 96000; t += 3001) {
      bool ok2 = false;
      const Planar got = DecodeFrom(e, fr, off, t, 960, &ok2);
      SND_REQUIRE(ok2 && got[0].size() == 96000 - t);
      worst = std::max(worst, MaxAbsDiff(ref, got, t, 96000 - t));
    }
    std::printf("  voice %u bit/s: %u of %u packets SILK/hybrid, worst seek error %.4f (peak %.3f)\n", bitrate,
                silkish, n, worst, peak);
    SND_CHECK_MSG(worst < 0.1, "%u bit/s: seek error %.4f", bitrate, worst);
  }
}
