// Opus codec tests (CONTRACTS.md D-56..D-60, bank-format.md §6.5).
//
// Built in every configuration. With SND_CODEC_OPUS=OFF (OpusAvailable() ==
// false) only the "unavailable" behaviour is checked.
//
// Expected values are analytic: tone levels by DFT at exact bins, harmonic
// distortion, band energies of white noise, zero delay by cross-correlation,
// channel identity by per-channel tones; seeks and chunking are compared with
// one continuous decode.
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "codec_test_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/codec/encode.hpp"
#include "snd/codec/registry.hpp"
#include "snd/test/rt_guard.hpp"
#include "snd/test/test.hpp"

using namespace codec_test;
using snd::CodecHeader;
namespace enc = snd::codec::encode;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;

struct OpusAsset {
  std::vector<std::uint8_t> data;
  std::uint8_t setup[enc::kOpusMaxSetupBytes] = {};
  CodecHeader h{};
  std::uint64_t packets = 0;
};

std::unique_ptr<OpusAsset> EncodeOpusOrDie(const Planar& p, const enc::OpusSettings& st = {}) {
  auto a = std::make_unique<OpusAsset>();
  const auto ch = static_cast<std::uint32_t>(p.size());
  const std::uint64_t frames = Frames(p);
  a->data.resize(static_cast<std::size_t>(enc::OpusMaxEncodedBytes(ch, frames)));
  enc::OpusEncoded e{};
  auto ptrs = Ptrs(p);
  const auto s = enc::EncodeOpus(st, ptrs.data(), ch, kRate, frames, a->data.data(), a->data.size(), &e);
  SND_CHECK_MSG(s == enc::EncodeStatus::Ok, "EncodeOpus: %s", enc::EncodeStatusName(s));
  a->data.resize(static_cast<std::size_t>(e.bytes));
  std::memcpy(a->setup, e.setup, sizeof(a->setup));
  a->h = e.header;
  a->h.extra = a->setup;
  a->packets = e.packets;
  return a;
}

Planar Tone(std::uint32_t channels, std::uint64_t frames, const std::vector<double>& freqPerChannel, double amp) {
  Planar p(channels, std::vector<float>(static_cast<std::size_t>(frames)));
  for (std::uint32_t c = 0; c < channels; ++c) {
    const double f = freqPerChannel[c % freqPerChannel.size()];
    for (std::uint64_t i = 0; i < frames; ++i) {
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / kRate));
    }
  }
  return p;
}

// Uniform white noise in [-amp, amp), zero mean.
Planar Noise(std::uint32_t channels, std::uint64_t frames, double amp, std::uint32_t seed) {
  Planar p(channels, std::vector<float>(static_cast<std::size_t>(frames)));
  Rng rng(seed);
  for (auto& c : p) {
    for (auto& v : c) v = static_cast<float>(amp * (static_cast<double>(rng.Next()) / 1073741824.0 - 1.0));
  }
  return p;
}

// Amplitude of the component at `freq` (integer Hz) over x[start, start + 48000):
// with a 1 s window every integer frequency is an exact DFT bin (rectangular window).
double ToneAmplitude(const std::vector<float>& x, std::size_t start, double freq) {
  double re = 0, im = 0;
  const std::size_t n = kRate;
  for (std::size_t i = 0; i < n; ++i) {
    const double ph = 2.0 * kPi * freq * static_cast<double>(i) / kRate;
    re += x[start + i] * std::cos(ph);
    im -= x[start + i] * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

double Power(const std::vector<float>& x, std::size_t start, std::size_t n) {
  double s = 0;
  for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[start + i]) * x[start + i];
  return s / static_cast<double>(n);
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

// Lag (in [-maxLag, maxLag]) maximising sum ref[i] * got[i + lag] over [start, start + n).
int BestLag(const std::vector<float>& ref, const std::vector<float>& got, std::size_t start, std::size_t n, int maxLag) {
  int best = 0;
  double bestV = -1e300;
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    double s = 0;
    for (std::size_t i = 0; i < n; ++i) {
      s += static_cast<double>(ref[start + i]) * got[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(start + i) + lag)];
    }
    if (s > bestV) {
      bestV = s;
      best = lag;
    }
  }
  return best;
}

FeedResult DecodeBulk(const OpusAsset& a, std::uint32_t maxFrames = 4096) {
  return DecodeAll(a.h, a.data.data(), a.data.size(), {16384}, maxFrames);
}

// Power of x in [lo, hi) Hz over a 1 s window (sum of DFT bin powers, bin = 1 Hz).
// Uses a decimated set of bins (every `step` Hz) to stay fast.
double BandPower(const std::vector<float>& x, std::size_t start, double lo, double hi, double step) {
  double s = 0;
  for (double f = lo; f < hi; f += step) {
    const double a = ToneAmplitude(x, start, f);
    s += a * a;
  }
  return s;
}

}  // namespace

SND_TEST(Opus, AvailabilityMatchesTheBuild) {
  snd::codec::CodecRegistry reg;
  snd::codec::LookupFailure why = snd::codec::LookupFailure::None;
  if (!snd::codec::OpusAvailable()) {
    // SND_CODEC_OPUS=OFF: nothing is registered and the reasons say why (A0-3 behaviour).
    SND_CHECK(snd::codec::BuiltinCodec(SND_CODEC_OPUS) == nullptr);
    SND_CHECK(!snd::codec::IsBuiltinCodec(SND_CODEC_OPUS));
    SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == nullptr);
    SND_CHECK(why == snd::codec::LookupFailure::OpusUnavailable);
    SND_CHECK(!enc::OpusEncoderAvailable());
    float x[4] = {};
    const float* px = x;
    std::uint8_t out[8192];
    enc::OpusEncoded e{};
    SND_CHECK(enc::EncodeOpus({}, &px, 1, kRate, 4, out, sizeof(out), &e) == enc::EncodeStatus::CodecUnavailable);
    std::uint8_t setup[5] = {1, 1, 1, 0, 0};
    CodecHeader h{SND_CODEC_OPUS, 1, kRate, 960, 100, 312, 3840, 0, 5, setup};
    SND_CHECK(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::CodecUnavailable);
    return;
  }
  SND_CHECK(snd::codec::BuiltinCodec(SND_CODEC_OPUS) != nullptr);
  SND_CHECK(snd::codec::IsBuiltinCodec(SND_CODEC_OPUS));
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == snd::codec::BuiltinCodec(SND_CODEC_OPUS));
  SND_CHECK(why == snd::codec::LookupFailure::None);
  SND_CHECK(enc::OpusEncoderAvailable());
}

SND_TEST(Opus, HeaderValidation) {
  if (!snd::codec::OpusAvailable()) return;
  using HE = snd::codec::HeaderError;
  std::uint8_t setup[12] = {1, 2, 1, 1, 0, 1};
  const CodecHeader good{SND_CODEC_OPUS, 2, kRate, 960, 1000, 312, 3840, 0, 6, setup};
  SND_CHECK(snd::codec::ValidateHeader(good) == HE::None);
  CodecHeader t = good;
  t.frames_per_packet = 0;
  SND_CHECK(snd::codec::ValidateHeader(t) == HE::None);
  t = good; t.frames_per_packet = 480; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadFramesPerPacket);
  t = good; t.sample_rate = 44100; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadSampleRate);
  t = good; t.sample_rate = 24000; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadSampleRate);
  t = good; t.block_align = 4; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadBlockAlign);
  t = good; t.channels = 0; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadChannels);
  t = good; t.channels = 9; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadChannels);
  t = good; t.total_frames = UINT64_MAX; SND_CHECK(snd::codec::ValidateHeader(t) == HE::FrameCountOverflow);
  t = good; t.extra = nullptr; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadCodecSetup);
  t = good; t.extra_bytes = 5; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadCodecSetup);
  t = good; t.extra_bytes = 7; SND_CHECK(snd::codec::ValidateHeader(t) == HE::BadCodecSetup);
  auto withSetup = [&](std::initializer_list<std::uint8_t> bytes, std::uint32_t ch) {
    static std::uint8_t buf[16];
    std::size_t i = 0;
    for (std::uint8_t b : bytes) buf[i++] = b;
    CodecHeader x = good;
    x.channels = ch;
    x.extra = buf;
    x.extra_bytes = static_cast<std::uint32_t>(i);
    return snd::codec::ValidateHeader(x);
  };
  SND_CHECK(withSetup({2, 2, 1, 1, 0, 1}, 2) == HE::BadCodecSetup);   // version
  SND_CHECK(withSetup({1, 1, 1, 1, 0, 1}, 2) == HE::BadCodecSetup);   // channel count mismatch
  SND_CHECK(withSetup({1, 2, 0, 0, 0, 1}, 2) == HE::BadCodecSetup);   // no stream
  SND_CHECK(withSetup({1, 2, 1, 2, 0, 1}, 2) == HE::BadCodecSetup);   // coupled > streams
  SND_CHECK(withSetup({1, 2, 9, 0, 0, 1}, 2) == HE::BadCodecSetup);   // too many streams
  SND_CHECK(withSetup({1, 2, 1, 1, 0, 2}, 2) == HE::BadCodecSetup);   // mapping >= streams + coupled
  SND_CHECK(withSetup({1, 2, 1, 1, 0, 255}, 2) == HE::None);          // 255 = silent channel
  SND_CHECK(withSetup({1, 1, 1, 0, 0}, 1) == HE::None);
  // Geometry.
  SND_CHECK_EQ(snd::codec::PacketBytes(good), 0u);
  SND_CHECK_EQ(snd::codec::PacketFrames(good), 960u);
  SND_CHECK_EQ(snd::codec::OpusMaxPacketBytes(good), 1277u);
  SND_CHECK_EQ(snd::codec::HoldCapacityBytes(good), snd::codec::kHoldInputBytes + 2u + 1277u);
  snd::codec::PacketPosition pos{};
  SND_CHECK(!snd::codec::PacketForFrame(good, 0, &pos));  // variable size: use the seek table
  // Invalid header: minimal state, every Decode fails.
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  t = good;
  t.extra = nullptr;
  SND_CHECK_EQ(c->StateBytes(t), snd::codec::kMinStateBytes);
  StateMem mem(snd::codec::kMinStateBytes);
  c->Init(mem.get(), t);
  OutBuf ob(2, 16);
  std::uint8_t junk[8] = {3, 0, 1, 2, 3};
  SND_CHECK_EQ(c->Decode(mem.get(), junk, sizeof(junk), ob.dst(), 16), snd::kDecodeError);
  c->Seek(mem.get(), 0);
  SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 0, ob.dst(), 16), snd::kDecodeError);
  SND_CHECK(mem.GuardIntact());
  // A PCM header given to the Opus object, and an Opus header to the PCM object.
  CodecHeader pcm = enc::MakePcmHeader(SND_CODEC_PCM_F32, 2, kRate, 10);
  SND_CHECK_EQ(c->StateBytes(pcm), snd::codec::kMinStateBytes);
  SND_CHECK_EQ(snd::codec::BuiltinCodec(SND_CODEC_PCM_F32)->StateBytes(good), snd::codec::kMinStateBytes);
}

SND_TEST(Opus, EncoderOutputLayout) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    const std::uint64_t frames = 9000;
    auto a = EncodeOpusOrDie(Noise(ch, frames, 0.3, ch));
    const CodecHeader& h = a->h;
    SND_CHECK(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    SND_CHECK_EQ(h.codec, SND_CODEC_OPUS);
    SND_CHECK_EQ(h.sample_rate, kRate);
    SND_CHECK_EQ(h.frames_per_packet, 960u);
    SND_CHECK_EQ(h.block_align, 0u);
    SND_CHECK_EQ(h.pre_skip, 312u);  // libopus 1.5.2 lookahead at 48 kHz (OPUS_APPLICATION_AUDIO)
    SND_CHECK_EQ(h.pre_roll, snd::codec::kOpusPreRollFrames);
    SND_CHECK_EQ(h.total_frames, frames);
    SND_CHECK_EQ(h.extra_bytes, 4u + ch);
    SND_CHECK_EQ(a->packets, (frames + 312u + 959u) / 960u);
    // Walk the length prefixes.
    std::uint64_t off = 0, n = 0;
    while (off < a->data.size()) {
      const std::uint32_t len = a->data[off] | (a->data[off + 1] << 8);
      SND_CHECK(len >= 1u && len <= snd::codec::OpusMaxPacketBytes(h));
      off += 2u + len;
      ++n;
    }
    SND_CHECK_EQ(off, a->data.size());
    SND_CHECK_EQ(n, a->packets);
    // Surround streams (RFC 7845 family 1): 3 ch = 2 streams, 6 ch = 4, 8 ch = 5.
    const std::uint32_t expectStreams[9] = {0, 1, 1, 2, 2, 3, 4, 4, 5};
    SND_CHECK_EQ(static_cast<std::uint32_t>(a->setup[2]), expectStreams[ch]);
  }
  // Settings and arguments.
  Planar p = Tone(1, 2000, {440}, 0.5);
  auto ptrs = Ptrs(p);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(enc::OpusMaxEncodedBytes(1, 2000)));
  enc::OpusEncoded e{};
  enc::OpusSettings bad;
  bad.complexity = 11;
  SND_CHECK(enc::EncodeOpus(bad, ptrs.data(), 1, kRate, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::BadSettings);
  bad = {};
  bad.bitrate = 5999;
  SND_CHECK(enc::EncodeOpus(bad, ptrs.data(), 1, kRate, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::BadSettings);
  bad.bitrate = 510001;
  SND_CHECK(enc::EncodeOpus(bad, ptrs.data(), 1, kRate, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::BadSettings);
  SND_CHECK(enc::EncodeOpus({}, ptrs.data(), 1, 44100, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::BadArguments);
  SND_CHECK(enc::EncodeOpus({}, ptrs.data(), 9, kRate, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::BadArguments);
  SND_CHECK(enc::EncodeOpus({}, ptrs.data(), 1, kRate, 2000, out.data(), out.size() - 1, &e) ==
            enc::EncodeStatus::BufferTooSmall);
  p[0][100] = NAN;
  SND_CHECK(enc::EncodeOpus({}, ptrs.data(), 1, kRate, 2000, out.data(), out.size(), &e) == enc::EncodeStatus::NonFiniteInput);
  // Encode() / EncodedBytes() / BuildSeekTable() are for fixed-size codecs only.
  p[0][100] = 0.0f;
  auto good = EncodeOpusOrDie(p);
  std::uint64_t w = 0;
  SND_CHECK(enc::Encode(good->h, ptrs.data(), good->h.pre_skip + 2000u, out.data(), out.size(), &w) ==
            enc::EncodeStatus::BadHeader);
  SND_CHECK_EQ(enc::EncodedBytes(good->h), 0u);
  SND_CHECK_EQ(enc::BuildSeekTable(good->h, 0, nullptr, nullptr, 0), 0u);
}

SND_TEST(Opus, SeekTable) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t frames = 48000;
  auto a = EncodeOpusOrDie(Noise(2, frames, 0.3, 5));
  const CodecHeader& h = a->h;
  // Every packet: entries at 960k - 312 for k >= 1 while < total_frames.
  const std::uint32_t n = enc::BuildOpusSeekTable(h, a->data.data(), a->data.size(), 0, nullptr, nullptr, 0);
  std::vector<std::uint64_t> f(n), o(n);
  SND_REQUIRE_EQ(enc::BuildOpusSeekTable(h, a->data.data(), a->data.size(), 0, f.data(), o.data(), n), n);
  SND_CHECK_EQ(f[0], 0u);
  SND_CHECK_EQ(o[0], 0u);
  std::uint64_t expect = 1;
  for (std::uint64_t k = 1; 960u * k - 312u < frames; ++k) ++expect;
  SND_CHECK_EQ(static_cast<std::uint64_t>(n), expect);
  std::uint64_t off = 0;
  for (std::uint32_t i = 1; i < n; ++i) {
    SND_CHECK_EQ(f[i], 960u * i - 312u);
    // byte offset = sum of the previous packets
    const std::uint32_t len = a->data[off] | (a->data[off + 1] << 8);
    off += 2u + len;
    SND_CHECK_EQ(o[i], off);
  }
  // Spacing: 100 ms.
  const std::uint32_t m = enc::BuildOpusSeekTable(h, a->data.data(), a->data.size(), 4800, nullptr, nullptr, 0);
  std::vector<std::uint64_t> f2(m), o2(m);
  enc::BuildOpusSeekTable(h, a->data.data(), a->data.size(), 4800, f2.data(), o2.data(), m);
  for (std::uint32_t i = 1; i < m; ++i) SND_CHECK(f2[i] - f2[i - 1] >= 4800u);
  SND_CHECK(m > 5u && m < 12u);
  // Capacity smaller than the count: count returned, only `capacity` written.
  std::vector<std::uint64_t> f3(3, 77), o3(3, 77);
  SND_CHECK_EQ(enc::BuildOpusSeekTable(h, a->data.data(), a->data.size(), 0, f3.data(), o3.data(), 2), n);
  SND_CHECK_EQ(f3[2], 77u);
  // Corrupt walk: truncated data or a zero length.
  SND_CHECK_EQ(enc::BuildOpusSeekTable(h, a->data.data(), a->data.size() - 1, 0, nullptr, nullptr, 0), 0u);
  std::vector<std::uint8_t> z = a->data;
  z[0] = 0;
  z[1] = 0;
  SND_CHECK_EQ(enc::BuildOpusSeekTable(h, z.data(), z.size(), 0, nullptr, nullptr, 0), 0u);
}

// 1 kHz and other tones: fundamental level within +-0.5 dB, harmonics and
// noise well below, no delay.
SND_TEST(Opus, RoundTripTones) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t frames = 60000;  // 1.25 s
  const double amp = 0.5;              // -6 dBFS
  for (double freq : {100.0, 440.0, 1000.0, 3000.0, 8000.0}) {
    auto a = EncodeOpusOrDie(Tone(1, frames, {freq}, amp));  // 64 kbit/s mono (default)
    FeedResult r = DecodeBulk(*a);
    SND_REQUIRE(!r.error);
    SND_REQUIRE_EQ(Frames(r.out), frames);
    const std::size_t start = 6000;
    const double level = ToneAmplitude(r.out[0], start, freq);
    const double levelDb = Db(level / amp);
    double harm = 0;
    for (int k = 2; k * freq < 20000.0; ++k) {
      const double hk = ToneAmplitude(r.out[0], start, k * freq);
      harm += hk * hk;
    }
    const double thdDb = 10.0 * std::log10(harm / (level * level) + 1e-30);
    const double total = Power(r.out[0], start, kRate);
    const double rest = total - level * level / 2.0;  // everything but the fundamental
    const double thdnDb = 10.0 * std::log10(std::max(rest, 1e-30) / (level * level / 2.0));
    std::printf("  opus tone %6.0f Hz: level %+.3f dB, THD %.1f dB, THD+N %.1f dB\n", freq, levelDb, thdDb, thdnDb);
    SND_CHECK_MSG(std::fabs(levelDb) <= 0.5, "%g Hz: fundamental %+.3f dB", freq, levelDb);
    SND_CHECK_MSG(thdDb < -40.0, "%g Hz: THD %.1f dB", freq, thdDb);
    SND_CHECK_MSG(thdnDb < -30.0, "%g Hz: THD+N %.1f dB", freq, thdnDb);
    // Phase: the decoded sine is aligned with the input (no pre-skip offset).
    const double ref = ToneAmplitude(r.out[0], start, freq);
    double re = 0, im = 0, rre = 0, rim = 0;
    const Planar in = Tone(1, frames, {freq}, amp);
    for (std::size_t i = 0; i < kRate; ++i) {
      const double ph = 2.0 * kPi * freq * static_cast<double>(i) / kRate;
      re += r.out[0][start + i] * std::cos(ph);
      im -= r.out[0][start + i] * std::sin(ph);
      rre += in[0][start + i] * std::cos(ph);
      rim -= in[0][start + i] * std::sin(ph);
    }
    double dphi = std::atan2(im, re) - std::atan2(rim, rre);
    while (dphi > kPi) dphi -= 2 * kPi;
    while (dphi < -kPi) dphi += 2 * kPi;
    // The encoder's DC rejection (one-pole high-pass at 3 Hz, libopus dc_reject)
    // leads the phase by atan(3 / f); anything else would be a delay.
    const double hpLead = std::atan(3.0 / freq);
    const double delaySamples = -(dphi - hpLead) / (2 * kPi * freq) * kRate;
    SND_CHECK_MSG(std::fabs(delaySamples) < 0.25, "%g Hz: delay %.3f samples", freq, delaySamples);
    (void)ref;
    SND_CHECK(r.guardsOk);
  }
}

// White noise: zero delay (cross-correlation peak at lag 0) and band energies
// kept within +-1.5 dB.
SND_TEST(Opus, RoundTripWhiteNoise) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t frames = 60000;
  const Planar in = Noise(1, frames, 0.4, 99);
  enc::OpusSettings st;
  st.bitrate = 128000;
  auto a = EncodeOpusOrDie(in, st);
  FeedResult r = DecodeBulk(*a);
  SND_REQUIRE(!r.error);
  SND_REQUIRE_EQ(Frames(r.out), frames);
  const int lag = BestLag(in[0], r.out[0], 2000, 20000, 400);
  SND_CHECK_EQ(lag, 0);
  const std::size_t start = 6000;
  const double bands[][2] = {{50, 200}, {200, 800}, {800, 2000}, {2000, 5000}, {5000, 10000}, {10000, 16000}};
  for (const auto& b : bands) {
    const double pin = BandPower(in[0], start, b[0], b[1], 7.0);
    const double pout = BandPower(r.out[0], start, b[0], b[1], 7.0);
    const double d = 10.0 * std::log10(pout / pin);
    std::printf("  opus noise band %5.0f-%5.0f Hz: %+.2f dB\n", b[0], b[1], d);
    SND_CHECK_MSG(std::fabs(d) <= 1.5, "band %g-%g Hz: %+.2f dB", b[0], b[1], d);
  }
  // Fullband Opus codes up to 20 kHz: white noise keeps 20/24 of its power.
  const double rmsDb = 10.0 * std::log10(Power(r.out[0], start, kRate) / Power(in[0], start, kRate));
  const double expectDb = 10.0 * std::log10(20.0 / 24.0);
  std::printf("  opus noise RMS %+.2f dB (expected %+.2f dB: band limit 20 kHz)\n", rmsDb, expectDb);
  SND_CHECK_MSG(std::fabs(rmsDb - expectDb) <= 0.3, "RMS %+.2f dB", rmsDb);
  SND_CHECK(BandPower(r.out[0], start, 20500, 23500, 7.0) < 1e-3 * BandPower(in[0], start, 20500, 23500, 7.0));
}

SND_TEST(Opus, RoundTripSilence) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    auto a = EncodeOpusOrDie(Planar(ch, std::vector<float>(20000, 0.0f)));
    FeedResult r = DecodeBulk(*a);
    SND_REQUIRE(!r.error);
    SND_REQUIRE_EQ(Frames(r.out), 20000u);
    float peak = 0;
    for (const auto& c : r.out) {
      for (float v : c) peak = std::max(peak, std::fabs(v));
    }
    SND_CHECK_MSG(peak < 1e-4f, "%u ch: silence decodes to peak %g", ch, static_cast<double>(peak));
    // Silence costs few bytes (VBR).
    SND_CHECK_MSG(a->data.size() < a->packets * 16u * ch, "%u ch: %zu bytes for %llu packets", ch, a->data.size(),
                  static_cast<unsigned long long>(a->packets));
  }
}

// Different signals per channel: each channel keeps its tone, crosstalk is low
// (also checks that the WAV channel order survives the surround mapping).
SND_TEST(Opus, ChannelsKeepTheirSignals) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t frames = 60000;
  const std::vector<double> f = {440, 1000, 1700, 60, 2300, 2900, 3500, 4100};
  for (std::uint32_t ch : {2u, 3u, 6u, 8u}) {
    const Planar in = Tone(ch, frames, f, 0.4);
    auto a = EncodeOpusOrDie(in);  // 64 kbit/s per channel
    FeedResult r = DecodeBulk(*a);
    SND_REQUIRE(!r.error);
    SND_REQUIRE_EQ(Frames(r.out), frames);
    for (std::uint32_t c = 0; c < ch; ++c) {
      const double own = ToneAmplitude(r.out[c], 6000, f[c]);
      SND_CHECK_MSG(std::fabs(Db(own / 0.4)) <= 0.5, "%u ch, channel %u: own tone %+.2f dB", ch, c, Db(own / 0.4));
      for (std::uint32_t o = 0; o < ch; ++o) {
        if (o == c) continue;
        // The LFE stream (channel 3 of 6 / 7 / 8, coded in libopus' LFE mode)
        // carries more coding noise around its 60 Hz tone; the streams
        // themselves are independent.
        const double limit = (ch >= 6 && c == 3) ? -30.0 : -40.0;
        const double x = ToneAmplitude(r.out[c], 6000, f[o]);
        SND_CHECK_MSG(Db(x / 0.4) < limit, "%u ch: channel %u has %.1f dB of channel %u's tone", ch, c,
                      Db(x / 0.4), o);
      }
    }
  }
}

// Output does not depend on how input is chunked or on max_frames.
SND_TEST(Opus, ChunkingInvariance) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    auto a = EncodeOpusOrDie(MakeSignal(ch, 20000, ch));
    FeedResult ref = DecodeAll(a->h, a->data.data(), a->data.size(), {static_cast<std::uint32_t>(a->data.size())}, 65536);
    SND_REQUIRE(!ref.error);
    SND_REQUIRE_EQ(Frames(ref.out), 20000u);
    const std::vector<std::vector<std::uint32_t>> chunkings = {{1}, {2, 3, 5}, {7000}, {}, {16384}, {1, 700, 2}};
    for (std::uint32_t maxFrames : {1u, 7u, 256u, 960u, 961u, 4096u}) {
      for (const auto& chunks : chunkings) {
        if (maxFrames == 1u && chunks.size() == 1 && chunks[0] == 1) continue;  // slow and covered by others
        FeedResult r = DecodeAll(a->h, a->data.data(), a->data.size(), chunks, maxFrames, 11, 3000);
        SND_CHECK_MSG(!r.error, "%u ch, max %u: error", ch, maxFrames);
        SND_CHECK_MSG(SameAudio(ref.out, r.out), "%u ch, max %u, %zu chunk sizes: output differs", ch, maxFrames,
                      chunks.size());
        SND_CHECK(r.guardsOk);
      }
    }
  }
}

// End: exactly total_frames, trailing input ignored, drains return 0 afterwards.
SND_TEST(Opus, StopsAtTotalFrames) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint64_t frames : {1ull, 647ull, 648ull, 649ull, 960ull, 1607ull, 1608ull, 1609ull, 10000ull}) {
    auto a = EncodeOpusOrDie(MakeSignal(2, frames, 3));
    FeedResult r = DecodeBulk(*a, 333);
    SND_CHECK(!r.error);
    SND_CHECK_MSG(Frames(r.out) == frames, "frames %llu: decoded %llu", static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(Frames(r.out)));
    // Extra packets after the end are consumed and dropped.
    std::vector<std::uint8_t> more = a->data;
    more.insert(more.end(), a->data.begin(), a->data.end());
    FeedResult r2 = DecodeAll(a->h, more.data(), more.size(), {500}, 128);
    SND_CHECK(!r2.error);
    SND_CHECK(SameAudio(r.out, r2.out));
  }
}

namespace {

// Largest |seeked - continuous| over every frame from the target to the end,
// for seeks through the seek table with the header's pre-roll. Also checks
// frame counts and that a seek to frame 0 is bit-exact.
double SeekWorst(const OpusAsset& a, const std::vector<std::uint64_t>& targets) {
  const CodecHeader& h = a.h;
  const std::uint32_t ch = h.channels;
  const std::uint64_t frames = h.total_frames;
  FeedResult ref = DecodeBulk(a);
  SND_CHECK(!ref.error);
  const std::uint32_t n = enc::BuildOpusSeekTable(h, a.data.data(), a.data.size(), 0, nullptr, nullptr, 0);
  std::vector<std::uint64_t> sf(n), so(n);
  enc::BuildOpusSeekTable(h, a.data.data(), a.data.size(), 0, sf.data(), so.data(), n);
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  StateMem mem(c->StateBytes(h));
  c->Init(mem.get(), h);
  double worst = 0;
  for (std::uint64_t target : targets) {
    if (target >= frames) continue;
    const std::uint32_t e = snd::codec::FindSeekPoint(sf.data(), n, target, h.pre_roll, h.total_frames);
    SND_CHECK(e != snd::codec::kNoSeekPoint);
    if (e == snd::codec::kNoSeekPoint) return 1e9;
    SND_CHECK(sf[e] + h.pre_roll <= target || sf[e] == 0u);
    c->Seek(mem.get(), sf[e]);
    FeedResult r;
    Feed(*c, mem.get(), ch, a.data.data() + so[e], a.data.size() - static_cast<std::size_t>(so[e]), {2000}, 512, &r);
    SND_CHECK(!r.error);
    SND_CHECK_EQ(Frames(r.out), frames - sf[e]);
    if (r.error || Frames(r.out) != frames - sf[e]) return 1e9;
    for (std::uint32_t k = 0; k < ch; ++k) {
      for (std::uint64_t i = target; i < frames; ++i) {
        const double d = std::fabs(static_cast<double>(r.out[k][static_cast<std::size_t>(i - sf[e])]) -
                                   ref.out[k][static_cast<std::size_t>(i)]);
        worst = std::max(worst, d);
      }
      if (sf[e] == 0u) SND_CHECK(std::memcmp(r.out[k].data(), ref.out[k].data(), r.out[k].size() * sizeof(float)) == 0);
    }
  }
  SND_CHECK(mem.GuardIntact());
  return worst;
}

const std::vector<std::uint64_t> kSeekTargets = {0,     1,     100,   648,   1919,  1920,  3839,  3840,
                                                 3841,  4000,  12345, 47999, 50000, 95000, 95999};

}  // namespace

// Seek: from the seek table with the 80 ms pre-roll, the output from the
// target on is bit-identical to one continuous decode for CELT packets
// (independent frames and 64 kbit/s per channel, the build defaults). SILK /
// hybrid packets (voice at low bitrates) keep cross-packet state even with
// independent frames: after 80 ms they are close but not exact.
SND_TEST(Opus, SeekIsSampleExact) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t frames = 96000;
  struct Case {
    std::uint32_t ch;
    std::uint32_t bitrate;
    enc::OpusSignal signal;
    double tolerance;
  };
  for (const Case& cs : {Case{1, 0, enc::OpusSignal::Auto, 0.0}, Case{2, 0, enc::OpusSignal::Auto, 0.0},
                         Case{6, 0, enc::OpusSignal::Auto, 0.0}, Case{2, 0, enc::OpusSignal::Music, 0.0},
                         Case{1, 12000, enc::OpusSignal::Voice, 0.1}, Case{1, 24000, enc::OpusSignal::Voice, 0.1},
                         Case{2, 32000, enc::OpusSignal::Voice, 0.1}}) {
    enc::OpusSettings st;
    st.bitrate = cs.bitrate;
    st.signal = cs.signal;
    auto a = EncodeOpusOrDie(MakeSignal(cs.ch, frames, 21 + cs.ch), st);
    const double worst = SeekWorst(*a, kSeekTargets);
    std::printf("  opus %u ch, %u bit/s, signal %u: worst |seek - continuous| = %.2e\n", cs.ch, cs.bitrate,
                static_cast<unsigned>(cs.signal), worst);
    SND_CHECK_MSG(worst <= cs.tolerance, "%u ch %u bit/s: seek differs from the continuous decode by %g", cs.ch,
                  cs.bitrate, worst);
    // A frame that is not a packet boundary fails the next Decode; Seek recovers.
    snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
    StateMem mem(c->StateBytes(a->h));
    c->Init(mem.get(), a->h);
    c->Seek(mem.get(), 100);
    OutBuf ob(cs.ch, 64);
    SND_CHECK_EQ(c->Decode(mem.get(), a->data.data(), 10, ob.dst(), 64), snd::kDecodeError);
    c->Seek(mem.get(), 960u - 312u);
    SND_CHECK(c->Decode(mem.get(), nullptr, 0, ob.dst(), 64) != snd::kDecodeError);
  }
  // With prediction across packets (independentFrames = false) the decoder
  // only converges: the 80 ms pre-roll leaves an error, but bounded.
  enc::OpusSettings st;
  st.independentFrames = false;
  auto a = EncodeOpusOrDie(MakeSignal(1, frames, 22), st);
  const double worst = SeekWorst(*a, kSeekTargets);
  std::printf("  opus 1 ch, prediction across packets: worst |seek - continuous| = %.2e\n", worst);
  SND_CHECK(worst < 1.5);
}

// Holding: following the protocol never errors; ignoring it hits the bound.
SND_TEST(Opus, HoldBound) {
  if (!snd::codec::OpusAvailable()) return;
  auto a = EncodeOpusOrDie(MakeSignal(2, 200000, 8));
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  StateMem mem(c->StateBytes(a->h));
  c->Init(mem.get(), a->h);
  OutBuf ob(2, 256);
  // max_frames = 0: everything is held; 16 KiB fits, a lot more does not.
  SND_CHECK(c->Decode(mem.get(), a->data.data(), 16384, ob.dst(), 0) == 0u);
  SND_CHECK_EQ(c->Decode(mem.get(), a->data.data() + 16384, 4000, ob.dst(), 0), snd::kDecodeError);
  SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 0, ob.dst(), 256), snd::kDecodeError);  // sticky
  c->Seek(mem.get(), 0);
  SND_CHECK_EQ(c->Decode(mem.get(), a->data.data(), 16384, ob.dst(), 256), 256u);
  SND_CHECK(mem.GuardIntact());
}

SND_TEST(Opus, ForeignStates) {
  if (!snd::codec::OpusAvailable()) return;
  auto a = EncodeOpusOrDie(MakeSignal(1, 3000, 2));
  snd::ICodec* opus = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  snd::ICodec* pcm = snd::codec::BuiltinCodec(SND_CODEC_PCM_F32);
  StateMem mem(opus->StateBytes(a->h));
  opus->Init(mem.get(), a->h);
  OutBuf ob(1, 64);
  SND_CHECK_EQ(pcm->Decode(mem.get(), a->data.data(), 10, ob.dst(), 64), snd::kDecodeError);
  pcm->Seek(mem.get(), 0);  // marks the state failed
  SND_CHECK_EQ(opus->Decode(mem.get(), nullptr, 0, ob.dst(), 64), snd::kDecodeError);
  opus->Seek(mem.get(), 0);
  SND_CHECK(opus->Decode(mem.get(), a->data.data(), 100, ob.dst(), 64) != snd::kDecodeError);
  // A PCM state given to the Opus object.
  const CodecHeader ph = enc::MakePcmHeader(SND_CODEC_PCM_F32, 1, kRate, 10);
  StateMem pm(pcm->StateBytes(ph));
  pcm->Init(pm.get(), ph);
  SND_CHECK_EQ(opus->Decode(pm.get(), nullptr, 0, ob.dst(), 64), snd::kDecodeError);
  SND_CHECK(mem.GuardIntact() && pm.GuardIntact());
}

// Corrupt data: bit flips, truncation, bad lengths, random bytes. Never a
// crash or an out-of-bounds write; errors are sticky.
SND_TEST(Opus, CorruptInputFuzz) {
  if (!snd::codec::OpusAvailable()) return;
  Rng rng(0x0505u);
  std::vector<std::unique_ptr<OpusAsset>> assets;
  for (std::uint32_t ch : {1u, 2u, 6u}) assets.push_back(EncodeOpusOrDie(MakeSignal(ch, 6000, ch)));
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  std::uint64_t errors = 0, ok = 0;
  for (int iter = 0; iter < 600; ++iter) {
    const OpusAsset& a = *assets[rng.Below(static_cast<std::uint32_t>(assets.size()))];
    std::vector<std::uint8_t> bytes = a.data;
    switch (rng.Below(5)) {
      case 0:
        for (std::uint32_t i = 0, n = 1 + rng.Below(8); i < n; ++i)
          bytes[rng.Below(static_cast<std::uint32_t>(bytes.size()))] ^= static_cast<std::uint8_t>(1u << rng.Below(8));
        break;
      case 1: bytes.resize(rng.Below(static_cast<std::uint32_t>(bytes.size()))); break;
      case 2: {  // a length prefix rewritten
        const std::uint32_t at = rng.Below(static_cast<std::uint32_t>(bytes.size() - 1));
        bytes[at] = static_cast<std::uint8_t>(rng.Next());
        bytes[at + 1] = static_cast<std::uint8_t>(rng.Below(3) == 0 ? rng.Next() : 0u);
        break;
      }
      case 3:
        for (auto& b : bytes) b = static_cast<std::uint8_t>(rng.Next());
        break;
      default:  // packet payloads scrambled, lengths intact
        for (std::size_t off = 0; off + 2 < bytes.size();) {
          const std::uint32_t len = bytes[off] | (bytes[off + 1] << 8);
          for (std::uint32_t i = 0; i < len && off + 2 + i < bytes.size(); ++i)
            if (rng.Below(4) == 0) bytes[off + 2 + i] = static_cast<std::uint8_t>(rng.Next());
          off += 2u + len;
        }
        break;
    }
    StateMem mem(c->StateBytes(a.h));
    c->Init(mem.get(), a.h);
    FeedResult r;
    Feed(*c, mem.get(), a.h.channels, bytes.data(), bytes.size(), {}, 1 + rng.Below(2000), &r, iter, 3000);
    if (r.error) {
      ++errors;
      OutBuf ob(a.h.channels, 16);
      SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 0, ob.dst(), 16), snd::kDecodeError);  // sticky
    } else {
      ++ok;
      SND_CHECK(Frames(r.out) <= a.h.total_frames);
      for (const auto& ch : r.out) {
        for (float v : ch) SND_CHECK(std::isfinite(v));
      }
    }
    SND_CHECK(r.guardsOk);
    SND_CHECK(mem.GuardIntact());
  }
  std::printf("  opus fuzz: %llu errors, %llu decoded\n", static_cast<unsigned long long>(errors),
              static_cast<unsigned long long>(ok));
  SND_CHECK(errors > 0u);
}

// Everything on the decode path runs without allocation, locks or IO.
SND_TEST(Opus, NoRtViolationsInDecodePath) {
  if (!snd::codec::OpusAvailable()) return;
  auto a = EncodeOpusOrDie(MakeSignal(2, 30000, 4));
  auto a6 = EncodeOpusOrDie(MakeSignal(6, 10000, 6));
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  snd::codec::CodecRegistry reg;
  StateMem mem(c->StateBytes(a6->h));
  OutBuf ob(6, 512);
  std::uint64_t total = 0;
  snd::test::rt::Reset();
  {
    SND_RT_SCOPE();
    for (const OpusAsset* x : {a.get(), a6.get()}) {
      SND_CHECK(reg.Find(SND_CODEC_OPUS) == c);
      (void)snd::codec::ValidateHeader(x->h);
      SND_CHECK(c->StateBytes(x->h) <= c->StateBytes(a6->h));
      c->Init(mem.get(), x->h);
      for (int pass = 0; pass < 2; ++pass) {
        // Pass 1 seeks to the 4th packet (decoded frame 2880) and feeds from its prefix.
        std::size_t off = 0;
        if (pass == 1) {
          for (int k = 0; k < 3; ++k) off += 2u + (x->data[off] | (x->data[off + 1] << 8));
        }
        c->Seek(mem.get(), pass == 0 ? 0u : 960u * 3u - x->h.pre_skip);
        while (off < x->data.size()) {
          const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(700, x->data.size() - off));
          std::uint32_t got = c->Decode(mem.get(), x->data.data() + off, n, ob.dst(), 512);
          off += n;
          while (got != snd::kDecodeError) {
            total += got;
            if (got < 512) break;
            got = c->Decode(mem.get(), nullptr, 0, ob.dst(), 512);
          }
          if (got == snd::kDecodeError) break;
        }
      }
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  SND_CHECK(total >= 40000u);
}

namespace {

struct StackProbe {
  const OpusAsset* asset;
  std::uint64_t frames;
  double seconds;
};

void* DecodeOnThread(void* arg) {
  auto* p = static_cast<StackProbe*>(arg);
  const OpusAsset& a = *p->asset;
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  StateMem mem(c->StateBytes(a.h));
  OutBuf ob(a.h.channels, 1024);
  const auto t0 = std::chrono::steady_clock::now();
  c->Init(mem.get(), a.h);
  std::size_t off = 0;
  for (;;) {
    std::uint32_t n = 0;
    std::uint32_t got;
    if (off < a.data.size()) {
      n = static_cast<std::uint32_t>(std::min<std::size_t>(4096, a.data.size() - off));
      got = c->Decode(mem.get(), a.data.data() + off, n, ob.dst(), 1024);
      off += n;
    } else {
      got = c->Decode(mem.get(), nullptr, 0, ob.dst(), 1024);
    }
    if (got == snd::kDecodeError) break;
    p->frames += got;
    while (got == 1024u) {
      got = c->Decode(mem.get(), nullptr, 0, ob.dst(), 1024);
      if (got == snd::kDecodeError) break;
      p->frames += got;
    }
    if (off >= a.data.size() && got < 1024u) break;
  }
  p->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return nullptr;
}

}  // namespace

// Stack used by Init + Decode (libopus scratch is on the stack, VAR_ARRAYS):
// must fit the RT threads' 256 KiB (D-47). Measured on a painted stack. Also
// prints the decode time per channel.
SND_TEST(Opus, StackUsageAndSpeed) {
  if (!snd::codec::OpusAvailable()) return;
  constexpr std::size_t kStack = 1024u * 1024u;  // painted area; the bound checked is 256 KiB
  for (std::uint32_t ch : {1u, 2u, 6u, 8u}) {
    auto a = EncodeOpusOrDie(MakeSignal(ch, 48000 * 2, ch));
    std::vector<std::uint8_t> stack(kStack + 4096);
    std::uint8_t* base = stack.data() + (4096 - reinterpret_cast<std::uintptr_t>(stack.data()) % 4096);
    std::memset(base, 0xCD, kStack);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    SND_REQUIRE(pthread_attr_setstack(&attr, base, kStack) == 0);
    StackProbe probe{a.get(), 0, 0};
    pthread_t th;
    SND_REQUIRE(pthread_create(&th, &attr, &DecodeOnThread, &probe) == 0);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    std::size_t untouched = 0;
    while (untouched < kStack && base[untouched] == 0xCD) ++untouched;  // the stack grows down
    const std::size_t used = kStack - untouched;
    const double perChannelUs = probe.seconds / (static_cast<double>(probe.frames) / 960.0) / ch * 1e6;
    std::printf("  opus %u ch: stack %zu bytes (thread total), decode %.1f us per 20 ms packet per channel "
                "(%.2f%% of real time per channel)\n",
                ch, used, perChannelUs, perChannelUs / 20000.0 * 100.0);
    SND_CHECK_EQ(probe.frames, a->h.total_frames);
#if !defined(SND_SANITIZER_ACTIVE)
    // Without sanitizers (whose instrumentation enlarges frames) the whole
    // thread, decode included, stays far below the 256 KiB RT stack.
    SND_CHECK_MSG(used < 128u * 1024u, "%u ch: %zu bytes of stack", ch, used);
#endif
  }
}

// State sizes vs. the decoder pool slot (A1-19, D-58): 1-2 ch fit 64 KiB.
SND_TEST(Opus, StateBytesTable) {
  if (!snd::codec::OpusAvailable()) return;
  std::printf("  opus StateBytes: ch  streams  coupled  bytes\n");
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    auto a = EncodeOpusOrDie(Planar(ch, std::vector<float>(960, 0.0f)));
    const std::uint32_t sb = snd::codec::BuiltinCodec(SND_CODEC_OPUS)->StateBytes(a->h);
    std::printf("                   %u   %u        %u        %u\n", ch, a->setup[2], a->setup[3], sb);
    SND_CHECK_EQ(sb % 64u, 0u);
    if (ch <= 2) SND_CHECK_MSG(sb <= 64u * 1024u, "%u ch: %u bytes > 64 KiB", ch, sb);
    SND_CHECK(sb <= 192u * 1024u);
    // Pool: a 64 KiB slot takes 1-2 ch; the old default 32 KiB takes none (D-14 rejection).
    for (std::uint32_t slot : {32u * 1024u, 64u * 1024u}) {
      snd::codec::DecoderPool pool;
      const std::size_t need = snd::codec::DecoderPool::RequiredBytes(2, slot);
      void* area = ::operator new(need, std::align_val_t{64});
      SND_REQUIRE(pool.Init(area, need, 2, slot));
      snd::codec::DecoderSlot ds{};
      const auto st = pool.Acquire(sb, &ds);
      const bool fits = sb <= slot;
      SND_CHECK(st == (fits ? snd::codec::DecoderPoolStatus::Ok : snd::codec::DecoderPoolStatus::StateTooLarge));
      if (fits) {
        // Decode in the slot itself.
        snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
        c->Init(ds.state, a->h);
        OutBuf ob(ch, 1024);
        SND_CHECK_EQ(c->Decode(ds.state, a->data.data(), static_cast<std::uint32_t>(a->data.size()), ob.dst(), 1024),
                     960u);
        SND_CHECK(pool.Release(ds));
      }
      pool.Shutdown();
      ::operator delete(area, std::align_val_t{64});
    }
    SND_CHECK(ch > 2 || sb > 32u * 1024u);
  }
}
