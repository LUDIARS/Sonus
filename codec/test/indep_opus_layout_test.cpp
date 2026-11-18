// Independent tests: the Opus byte layout of bank-format.md §6.5 / CONTRACTS.md
// D-57 as produced by snd::codec::encode::EncodeOpus / BuildOpusSeekTable,
// read back with the reference parser of indep_opus_ref.hpp (RFC 6716 TOC and
// self-delimiting framing, written from the RFCs, no libopus). Also the header
// rules (ValidateHeader / ParseOpusSetup) and the SND_CODEC_OPUS=OFF build.
// Properties: scratchpad indep-opus-properties.md A1-A5, F2, F3, G1, H3, I1.
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "indep_opus_ref.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/decoder_pool.hpp"
#include "snd/codec/encode.hpp"
#include "snd/codec/registry.hpp"
#include "snd/config.h"
#include "snd/test/test.hpp"

namespace ce = snd::codec::encode;
using opusref::Encoded;
using opusref::Planar;
using snd::codec::HeaderError;

namespace {

// Mixed content so that the encoder visits several modes: tones, noise bursts, silence.
Planar Mixed(std::uint32_t ch, std::uint64_t frames, std::uint32_t seed) {
  Planar p(ch, std::vector<float>(static_cast<std::size_t>(frames)));
  std::uint64_t s = seed * 2654435761u + 1;
  for (std::uint32_t c = 0; c < ch; ++c) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      const double noise = static_cast<double>(static_cast<std::int32_t>(s >> 32)) / 2147483648.0;
      const std::uint64_t seg = (i / 4800) % 3;
      double v = 0;
      if (seg == 0) v = 0.4 * std::sin(opusref::kTwoPi * (200.0 + 150.0 * c) * static_cast<double>(i) / 48000.0);
      if (seg == 1) v = 0.3 * noise;
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(v);
    }
  }
  return p;
}

std::uint64_t CeilDiv(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

// Checks every rule of §6.5 on one encoding; `what` names the case.
void CheckLayout(const Encoded& e, std::uint32_t ch, std::uint64_t frames, const char* what) {
  const snd::CodecHeader& h = e.h;
  SND_CHECK_MSG(h.codec == SND_CODEC_OPUS, "%s", what);
  SND_CHECK_MSG(h.channels == ch, "%s", what);
  SND_CHECK_MSG(h.sample_rate == 48000u, "%s", what);
  SND_CHECK_MSG(h.frames_per_packet == 960u || h.frames_per_packet == 0u, "%s: fpp %u", what, h.frames_per_packet);
  SND_CHECK_MSG(h.block_align == 0u, "%s", what);
  SND_CHECK_MSG(h.pre_skip == opusref::kPreSkip, "%s: pre_skip %u", what, h.pre_skip);
  SND_CHECK_MSG(h.pre_roll == opusref::kPreRoll, "%s: pre_roll %u", what, h.pre_roll);
  SND_CHECK_MSG(h.total_frames == frames, "%s", what);
  SND_CHECK_MSG(snd::codec::ValidateHeader(h) == HeaderError::None, "%s", what);

  // Setup data: {1, ch, N, M, mapping[ch]}; 1-2 ch are one stream (family 0).
  opusref::Setup s;
  const std::string err = opusref::ParseSetup(h.extra, h.extra_bytes, ch, &s);
  SND_REQUIRE_MSG(err.empty(), "%s: setup %s", what, err.c_str());
  if (ch <= 2) {
    SND_CHECK_MSG(s.streams == 1u && s.coupled == ch - 1u, "%s: N %u M %u", what, s.streams, s.coupled);
    for (std::uint32_t i = 0; i < ch; ++i) SND_CHECK_MSG(s.mapping[i] == i, "%s: mapping[%u]", what, i);
  } else {
    // Family 1 (RFC 7845 §5.1.1.2): every decoded channel is used exactly once,
    // nothing is muted (255), and N + M == channels.
    SND_CHECK_MSG(s.streams + s.coupled == ch, "%s: N %u + M %u != ch", what, s.streams, s.coupled);
    std::set<std::uint32_t> used;
    for (std::uint32_t i = 0; i < ch; ++i) {
      SND_CHECK_MSG(s.mapping[i] != 255u, "%s: channel %u muted", what, i);
      used.insert(s.mapping[i]);
    }
    SND_CHECK_MSG(used.size() == ch, "%s: mapping is not a permutation", what);
  }

  // Data: ceil((frames + 312) / 960) packets of 960 samples each, nothing else.
  std::vector<opusref::Packet> pk;
  const std::string walk = opusref::WalkPackets(e.data.data(), e.data.size(), s.streams, &pk);
  SND_CHECK_MSG(walk.empty(), "%s: %s", what, walk.c_str());
  SND_CHECK_MSG(pk.size() == CeilDiv(frames + opusref::kPreSkip, 960), "%s: %zu packets, want %llu", what, pk.size(),
                static_cast<unsigned long long>(CeilDiv(frames + opusref::kPreSkip, 960)));
  SND_CHECK_MSG(e.packets == pk.size(), "%s: OpusEncoded::packets", what);

  // Seek table (every packet, interval 0) per §6.5 "SEEK".
  const std::uint32_t n = ce::BuildOpusSeekTable(h, e.data.data(), e.data.size(), 0, nullptr, nullptr, 0);
  std::vector<std::uint64_t> fr(n), off(n);
  SND_REQUIRE_EQ(ce::BuildOpusSeekTable(h, e.data.data(), e.data.size(), 0, fr.data(), off.data(), n), n);
  // Expected rows from the rule: {0,0} then packet k with 960k > 312 and 960k - 312 < total.
  std::vector<std::uint64_t> wantF{0}, wantO{0};
  for (std::size_t k = 0; k < pk.size(); ++k) {
    const std::uint64_t d = 960u * k;
    if (d > opusref::kPreSkip && d - opusref::kPreSkip < frames) {
      wantF.push_back(d - opusref::kPreSkip);
      wantO.push_back(pk[k].offset);
    }
  }
  SND_CHECK_MSG(fr == wantF, "%s: SEEK frames (%u rows, want %zu)", what, n, wantF.size());
  SND_CHECK_MSG(off == wantO, "%s: SEEK byte offsets", what);
  for (std::uint32_t i = 1; i < n; ++i) {
    SND_CHECK_MSG((fr[i] + opusref::kPreSkip) % 960u == 0u, "%s: row %u", what, i);
    SND_CHECK_MSG(fr[i] > fr[i - 1] && off[i] >= off[i - 1], "%s: row %u order", what, i);
  }
}

}  // namespace

// The build option and the run-time answer agree (SND_INDEP_OPUS_EXPECTED is set
// from SND_CODEC_OPUS by the test's CMake registration).
SND_TEST(IndepOpusLayout, AvailabilityMatchesTheBuildOption) {
  SND_CHECK_EQ(snd::codec::OpusAvailable(), SND_INDEP_OPUS_EXPECTED != 0);
  SND_CHECK_EQ(ce::OpusEncoderAvailable(), SND_INDEP_OPUS_EXPECTED != 0);
  SND_CHECK_EQ((ce::kOpusEncoderVersion & 0xFFFFFFu), 0x010502u);  // H3: libopus 1.5.2 in the key
}

// G1: with SND_CODEC_OPUS=OFF nothing decodes or encodes Opus, with the documented reasons.
SND_TEST(IndepOpusLayout, OffBuildRefusesOpus) {
  if (snd::codec::OpusAvailable()) return;
  snd::codec::CodecRegistry reg;
  snd::codec::LookupFailure why = snd::codec::LookupFailure::None;
  SND_CHECK(reg.Find(SND_CODEC_OPUS, &why) == nullptr);
  SND_CHECK(why == snd::codec::LookupFailure::OpusUnavailable);
  SND_CHECK(snd::codec::BuiltinCodec(SND_CODEC_OPUS) == nullptr);
  SND_CHECK(!snd::codec::IsBuiltinCodec(SND_CODEC_OPUS));
  const std::uint8_t setup[] = {1, 2, 1, 1, 0, 1};
  const snd::CodecHeader h{SND_CODEC_OPUS, 2, 48000, 960, 48000, 312, 3840, 0, sizeof setup, setup};
  SND_CHECK(snd::codec::ValidateHeader(h) == HeaderError::CodecUnavailable);
  std::vector<float> z(960, 0.0f);
  const float* src[2] = {z.data(), z.data()};
  std::vector<std::uint8_t> out(1 << 16);
  ce::OpusEncoded e{};
  SND_CHECK(ce::EncodeOpus(ce::OpusSettings{}, src, 2, 48000, 960, out.data(), out.size(), &e) ==
            ce::EncodeStatus::CodecUnavailable);
}

// A1-A5 for 1..8 channels and the length boundaries of pre-skip and the packet size.
SND_TEST(IndepOpusLayout, EncoderOutputFollowsTheSpec) {
  if (!snd::codec::OpusAvailable()) return;
  const std::uint64_t lengths[] = {0, 1, 647, 648, 649, 960, 1607, 1608, 1609, 4800, 9601};
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    for (std::uint64_t frames : lengths) {
      if (ch > 2 && frames != 0 && frames != 649 && frames != 9601) continue;  // keep the run short
      Encoded e;
      SND_REQUIRE_MSG(opusref::Encode(Mixed(ch, frames, ch * 7u + 1u), ce::OpusSettings{}, &e), "ch %u frames %llu",
                      ch, static_cast<unsigned long long>(frames));
      char what[64];
      std::snprintf(what, sizeof what, "%u ch, %llu frames", ch, static_cast<unsigned long long>(frames));
      CheckLayout(e, ch, frames, what);
    }
  }
}

// The same rules under every setting the project can choose (CBR / constrained,
// voice at low bitrates = SILK / hybrid packets, low complexity, prediction on).
SND_TEST(IndepOpusLayout, EverySettingKeepsTheLayout) {
  if (!snd::codec::OpusAvailable()) return;
  struct Case {
    std::uint32_t ch, bitrate, complexity;
    ce::OpusVbr vbr;
    ce::OpusSignal signal;
    bool independent;
  };
  const Case cases[] = {
      {1, 6000, 10, ce::OpusVbr::Vbr, ce::OpusSignal::Voice, true},
      {1, 12000, 10, ce::OpusVbr::Cbr, ce::OpusSignal::Voice, true},
      {1, 24000, 0, ce::OpusVbr::ConstrainedVbr, ce::OpusSignal::Voice, false},
      {2, 510000 * 2, 10, ce::OpusVbr::Cbr, ce::OpusSignal::Music, true},
      {2, 16000, 5, ce::OpusVbr::Vbr, ce::OpusSignal::Auto, false},
      {6, 6 * 510000, 10, ce::OpusVbr::Cbr, ce::OpusSignal::Music, true},
      {8, 48000, 3, ce::OpusVbr::Vbr, ce::OpusSignal::Voice, true},
  };
  for (const Case& c : cases) {
    ce::OpusSettings st;
    st.bitrate = c.bitrate;
    st.complexity = c.complexity;
    st.vbr = c.vbr;
    st.signal = c.signal;
    st.independentFrames = c.independent;
    Encoded e;
    SND_REQUIRE_MSG(opusref::Encode(Mixed(c.ch, 14400, c.bitrate), st, &e), "bitrate %u", c.bitrate);
    char what[64];
    std::snprintf(what, sizeof what, "%u ch @ %u bit/s", c.ch, c.bitrate);
    CheckLayout(e, c.ch, 14400, what);
    // The u16 length always holds the largest packet (8 x 1277 < 65536); CBR at
    // the top rate stays within streams x 1277 per packet.
    std::vector<opusref::Packet> pk;
    opusref::WalkPackets(e.data.data(), e.data.size(), e.setup.size() > 2 ? e.setup[2] : 1u, &pk);
    std::uint32_t biggest = 0;
    for (const auto& p : pk) biggest = p.length > biggest ? p.length : biggest;
    std::printf("  %s: %zu packets, largest %u bytes, %zu bytes total\n", what, pk.size(), biggest, e.data.size());
  }
}

// Settings range (project-format.md, encode.hpp): bitrate 0 or 6000..510000 x ch, complexity 0..10.
SND_TEST(IndepOpusLayout, SettingsOutOfRangeAreRejected) {
  if (!snd::codec::OpusAvailable()) return;
  const Planar p = opusref::Tones(2, 960, {440.0}, 0.3);
  auto bad = [&](ce::OpusSettings st) {
    std::vector<const float*> ptr{p[0].data(), p[1].data()};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(ce::OpusMaxEncodedBytes(2, 960)));
    ce::OpusEncoded r{};
    return ce::EncodeOpus(st, ptr.data(), 2, 48000, 960, out.data(), out.size(), &r);
  };
  ce::OpusSettings st;
  st.bitrate = 5999;
  SND_CHECK(bad(st) == ce::EncodeStatus::BadSettings);
  st.bitrate = 510000 * 2 + 1;
  SND_CHECK(bad(st) == ce::EncodeStatus::BadSettings);
  st.bitrate = 6000;
  SND_CHECK(bad(st) == ce::EncodeStatus::Ok);
  st.bitrate = 510000 * 2;
  SND_CHECK(bad(st) == ce::EncodeStatus::Ok);
  st = {};
  st.complexity = 11;
  SND_CHECK(bad(st) == ce::EncodeStatus::BadSettings);
  st.complexity = 0;
  SND_CHECK(bad(st) == ce::EncodeStatus::Ok);
  // Only 48 kHz input is accepted by the encoder (the build converts first).
  std::vector<const float*> ptr{p[0].data(), p[1].data()};
  std::vector<std::uint8_t> out(static_cast<std::size_t>(ce::OpusMaxEncodedBytes(2, 960)));
  ce::OpusEncoded r{};
  SND_CHECK(ce::EncodeOpus(ce::OpusSettings{}, ptr.data(), 2, 44100, 960, out.data(), out.size(), &r) ==
            ce::EncodeStatus::BadArguments);
  SND_CHECK(ce::EncodeOpus(ce::OpusSettings{}, ptr.data(), 9, 48000, 960, out.data(), out.size(), &r) ==
            ce::EncodeStatus::BadArguments);
  // Too small a buffer is reported, not overrun.
  SND_CHECK(ce::EncodeOpus(ce::OpusSettings{}, ptr.data(), 2, 48000, 960, out.data(), 10, &r) ==
            ce::EncodeStatus::BufferTooSmall);
  // Encoding is deterministic (the build's bytes are reproducible).
  Encoded a, b;
  SND_REQUIRE(opusref::Encode(Mixed(2, 9600, 3), ce::OpusSettings{}, &a));
  SND_REQUIRE(opusref::Encode(Mixed(2, 9600, 3), ce::OpusSettings{}, &b));
  SND_CHECK(a.data == b.data && a.setup == b.setup);
}

// Seek table spacing (intervalFrames) and refusal of a broken packet walk.
SND_TEST(IndepOpusLayout, SeekTableIntervalAndBrokenData) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Mixed(1, 48000, 5), ce::OpusSettings{}, &e));
  for (std::uint64_t interval : {1u, 960u, 961u, 4800u, 100000u}) {
    const std::uint32_t n = ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), interval, nullptr, nullptr, 0);
    std::vector<std::uint64_t> f(n), o(n);
    SND_REQUIRE_EQ(ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), interval, f.data(), o.data(), n), n);
    SND_REQUIRE(n >= 1u);
    SND_CHECK(f[0] == 0u && o[0] == 0u);
    for (std::uint32_t i = 1; i < n; ++i) {
      SND_CHECK_MSG(f[i] - f[i - 1] >= interval, "interval %llu row %u", static_cast<unsigned long long>(interval), i);
      SND_CHECK((f[i] + 312u) % 960u == 0u);
      SND_CHECK(f[i] < e.h.total_frames);
      // The byte offset points at a length prefix: walking from it parses to the end.
      std::vector<opusref::Packet> pk;
      SND_CHECK(opusref::WalkPackets(e.data.data() + o[i], e.data.size() - o[i], 1, &pk).empty());
      SND_CHECK_EQ(pk.size(), (e.packets - (f[i] + 312u) / 960u));
    }
    if (interval == 100000u) SND_CHECK_EQ(n, 1u);
  }
  // Capacity smaller than the table: count returned, only `capacity` rows written.
  std::uint64_t f2[3] = {7, 7, 7}, o2[3] = {7, 7, 7};
  const std::uint32_t all = ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, nullptr, nullptr, 0);
  SND_CHECK_EQ(ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size(), 0, f2, o2, 2), all);
  SND_CHECK(f2[2] == 7u && o2[2] == 7u);
  // Broken walks: zero length, too long, truncated.
  std::vector<std::uint8_t> d = e.data;
  d[0] = 0;
  d[1] = 0;
  SND_CHECK_EQ(ce::BuildOpusSeekTable(e.h, d.data(), d.size(), 0, nullptr, nullptr, 0), 0u);
  d = e.data;
  d[0] = 0xFE;
  d[1] = 0x04;  // 1278 > 1 x 1277
  SND_CHECK_EQ(ce::BuildOpusSeekTable(e.h, d.data(), d.size(), 0, nullptr, nullptr, 0), 0u);
  SND_CHECK_EQ(ce::BuildOpusSeekTable(e.h, e.data.data(), e.data.size() - 1, 0, nullptr, nullptr, 0), 0u);
}

// F2 / F3: header and setup validation (bank-format.md §6.5 tables).
SND_TEST(IndepOpusLayout, HeaderAndSetupValidation) {
  if (!snd::codec::OpusAvailable()) return;
  std::uint8_t s2[6] = {1, 2, 1, 1, 0, 1};
  const snd::CodecHeader good{SND_CODEC_OPUS, 2, 48000, 960, 48000, 312, 3840, 0, 6, s2};
  SND_CHECK(snd::codec::ValidateHeader(good) == HeaderError::None);
  snd::CodecHeader h = good;
  h.frames_per_packet = 0;  // "0 も 960 とみなす"
  SND_CHECK(snd::codec::ValidateHeader(h) == HeaderError::None);
  // Header fields.
  for (std::uint32_t rate : {44100u, 96000u, 24000u, 16000u, 0u}) {
    h = good;
    h.sample_rate = rate;
    SND_CHECK_MSG(snd::codec::ValidateHeader(h) != HeaderError::None, "rate %u", rate);
  }
  for (std::uint32_t fpp : {480u, 1920u, 1u}) {
    h = good;
    h.frames_per_packet = fpp;
    SND_CHECK_MSG(snd::codec::ValidateHeader(h) != HeaderError::None, "fpp %u", fpp);
  }
  h = good;
  h.block_align = 960;
  SND_CHECK(snd::codec::ValidateHeader(h) != HeaderError::None);
  h = good;
  h.channels = 0;
  SND_CHECK(snd::codec::ValidateHeader(h) != HeaderError::None);
  h = good;
  h.channels = 9;
  SND_CHECK(snd::codec::ValidateHeader(h) != HeaderError::None);
  // Setup: each bad field gives BadCodecSetup.
  auto setupErr = [&](std::vector<std::uint8_t> s, std::uint32_t ch) {
    snd::CodecHeader x = good;
    x.channels = ch;
    x.extra = s.empty() ? nullptr : s.data();
    x.extra_bytes = static_cast<std::uint32_t>(s.size());
    const HeaderError e = snd::codec::ValidateHeader(x);
    snd::codec::OpusSetup os{};
    const HeaderError p = snd::codec::ParseOpusSetup(x, &os);
    SND_CHECK(e == p);
    return e;
  };
  SND_CHECK(setupErr({}, 2) == HeaderError::BadCodecSetup);                         // missing
  SND_CHECK(setupErr({2, 2, 1, 1, 0, 1}, 2) == HeaderError::BadCodecSetup);         // version 2
  SND_CHECK(setupErr({0, 2, 1, 1, 0, 1}, 2) == HeaderError::BadCodecSetup);         // version 0
  SND_CHECK(setupErr({1, 1, 1, 1, 0, 1}, 2) == HeaderError::BadCodecSetup);         // ch byte != ASST.channels
  SND_CHECK(setupErr({1, 2, 0, 0, 0, 1}, 2) == HeaderError::BadCodecSetup);         // N = 0
  SND_CHECK(setupErr({1, 2, 9, 0, 0, 1}, 2) == HeaderError::BadCodecSetup);         // N > 8
  SND_CHECK(setupErr({1, 2, 1, 2, 0, 1}, 2) == HeaderError::BadCodecSetup);         // M > N
  SND_CHECK(setupErr({1, 2, 1, 1, 0, 2}, 2) == HeaderError::BadCodecSetup);         // mapping >= N + M
  SND_CHECK(setupErr({1, 2, 1, 1, 0, 254}, 2) == HeaderError::BadCodecSetup);       // mapping 254
  SND_CHECK(setupErr({1, 2, 1, 1, 0}, 2) == HeaderError::BadCodecSetup);            // short
  SND_CHECK(setupErr({1, 2, 1}, 2) == HeaderError::BadCodecSetup);                  // shorter than 4
  // Valid variants: 255 = silence, family-1 style 8 ch.
  SND_CHECK(setupErr({1, 2, 1, 1, 0, 255}, 2) == HeaderError::None);
  SND_CHECK(setupErr({1, 8, 5, 3, 0, 4, 1, 7, 5, 6, 2, 3}, 8) == HeaderError::None);
  SND_CHECK(setupErr({1, 8, 8, 8, 0, 15, 1, 14, 2, 13, 3, 12}, 8) == HeaderError::None);
  // ParseOpusSetup reports what it read.
  {
    std::vector<std::uint8_t> s = {1, 3, 2, 1, 0, 2, 1};
    snd::CodecHeader x = good;
    x.channels = 3;
    x.extra = s.data();
    x.extra_bytes = 7;
    snd::codec::OpusSetup os{};
    SND_REQUIRE(snd::codec::ParseOpusSetup(x, &os) == HeaderError::None);
    SND_CHECK(os.streams == 2u && os.coupledStreams == 1u);
    SND_CHECK(os.mapping[0] == 0 && os.mapping[1] == 2 && os.mapping[2] == 1);
    SND_CHECK_EQ(snd::codec::OpusMaxPacketBytes(x), 2u * 1277u);
    SND_CHECK_EQ(snd::codec::HoldCapacityBytes(x), 16384u + 2u + 2u * 1277u);
    SND_CHECK_EQ(snd::codec::PacketFrames(x), 960u);
    SND_CHECK_EQ(snd::codec::PacketBytes(x), 0u);  // variable size
    snd::codec::PacketPosition pp{};
    SND_CHECK(!snd::codec::PacketForFrame(x, 1000, &pp));  // Opus uses the seek table
  }
  // An invalid header: StateBytes still answers, Init records it, Decode errors.
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  SND_REQUIRE(c != nullptr);
  h = good;
  h.sample_rate = 44100;
  const std::uint32_t sb = c->StateBytes(h);
  SND_CHECK(sb >= snd::codec::kMinStateBytes);
  opusref::State st(sb);
  c->Init(st.get(), h);
  std::vector<float> buf(2048);
  float* dst[2] = {buf.data(), buf.data() + 1024};
  const std::uint8_t pkt[4] = {2, 0, 0xF8, 0xFF};
  SND_CHECK_EQ(c->Decode(st.get(), pkt, 4, dst, 1024), snd::kDecodeError);
  c->Seek(st.get(), 0);  // a header error is not cleared by Seek
  SND_CHECK_EQ(c->Decode(st.get(), pkt, 4, dst, 1024), snd::kDecodeError);
  SND_CHECK(st.Intact());
}

// D-58 / E3 / E4: state sizes against the default slot, and the pool's verdict.
SND_TEST(IndepOpusLayout, StateBytesAndTheDefaultSlot) {
  if (!snd::codec::OpusAvailable()) return;
  snd_config cfg;
  snd_config_init(&cfg);
  SND_CHECK_EQ(cfg.memory.decoder_slot_bytes, 65536u);
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_OPUS);
  std::uint32_t prev = 0;
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    Encoded e;
    SND_REQUIRE(opusref::Encode(Planar(ch, std::vector<float>(960, 0.0f)), ce::OpusSettings{}, &e));
    const std::uint32_t sb = c->StateBytes(e.h);
    SND_CHECK_EQ(sb % 64u, 0u);
    SND_CHECK_MSG(sb >= prev, "%u ch: %u < %u", ch, sb, prev);
    prev = sb;
    SND_CHECK_MSG(sb <= 192u * 1024u, "%u ch: %u bytes > 192 KiB", ch, sb);
    // The state must at least hold the D-51 input bound and one decoded packet.
    SND_CHECK(sb >= 16384u + 2u + 1277u + 960u * ch * 4u);
    // Default pool (cfg slot size, 2 slots): 1-2 ch fit, 3+ ch are refused with POOL_FULL.
    snd::codec::DecoderPool pool;
    const std::size_t need = snd::codec::DecoderPool::RequiredBytes(2, cfg.memory.decoder_slot_bytes);
    void* area = ::operator new(need, std::align_val_t{64});
    SND_REQUIRE(pool.Init(area, need, 2, cfg.memory.decoder_slot_bytes));
    snd::codec::DecoderSlot slot{};
    const auto stt = pool.Acquire(sb, &slot);
    if (ch <= 2) {
      SND_CHECK(stt == snd::codec::DecoderPoolStatus::Ok);
      // Decode inside the slot; the bytes after StateBytes inside the slot are not touched.
      auto* bytes = static_cast<std::uint8_t*>(slot.state);
      std::memset(bytes + sb, 0x6B, pool.SlotBytes() - sb);
      c->Init(slot.state, e.h);
      std::vector<std::vector<float>> o(ch, std::vector<float>(960));
      std::vector<float*> dp;
      for (auto& v : o) dp.push_back(v.data());
      SND_CHECK_EQ(c->Decode(slot.state, e.data.data(), static_cast<std::uint32_t>(e.data.size()), dp.data(), 960),
                   960u);
      bool clean = true;
      for (std::uint32_t i = sb; i < pool.SlotBytes(); ++i) clean = clean && bytes[i] == 0x6B;
      SND_CHECK_MSG(clean, "%u ch: decoder wrote past StateBytes", ch);
      SND_CHECK(pool.Release(slot));
    } else {
      SND_CHECK(stt == snd::codec::DecoderPoolStatus::StateTooLarge);
      SND_CHECK(snd::codec::EndDetailFor(stt) == SND_END_DETAIL_DECODER_POOL_FULL);
    }
    pool.Shutdown();
    ::operator delete(area, std::align_val_t{64});
    std::printf("  %u ch: StateBytes %u (N %u, M %u)\n", ch, sb, e.setup[2], e.setup[3]);
  }
}
