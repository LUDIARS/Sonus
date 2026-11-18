// Independent re-check of the Opus decisions after the first round
// (CONTRACTS.md D-57 / D-58 as amended 2026-11-17, bank-format.md §6.5):
//   - input after total_frames is consumed and dropped without parsing,
//   - extra_bytes is exactly 4 + channels,
//   - 1-2 ch setups with N != 1 are valid (RFC 7845) and decode,
//   - Seek(f > 0) off a packet boundary: the next Decode fails, sticky until Seek / Init,
//   - the WAV / Vorbis channel table of §6.5 (coupled pairs and LFE).
// Reference code: indep_opus_ref.hpp (from the RFCs, no libopus).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "indep_opus_ref.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/test/test.hpp"

namespace ce = snd::codec::encode;
using opusref::Encoded;
using opusref::Planar;

namespace {

Planar Sig(std::uint32_t ch, std::uint64_t frames, std::uint32_t seed) {
  Planar p(ch, std::vector<float>(static_cast<std::size_t>(frames)));
  std::uint64_t s = seed * 7919u + 3u;
  for (std::uint32_t c = 0; c < ch; ++c) {
    for (std::uint64_t i = 0; i < frames; ++i) {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      const double n = static_cast<double>(static_cast<std::int32_t>(s >> 32)) / 2147483648.0;
      p[c][static_cast<std::size_t>(i)] = static_cast<float>(
          0.3 * std::sin(opusref::kTwoPi * (220.0 + 110.0 * c + seed) * static_cast<double>(i) / 48000.0) + 0.05 * n);
    }
  }
  return p;
}

bool Same(const Planar& a, const Planar& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) return false;
    if (!a[c].empty() && std::memcmp(a[c].data(), b[c].data(), a[c].size() * 4) != 0) return false;
  }
  return true;
}

// RFC 6716 §3.2.1 length bytes.
void PutLen(std::vector<std::uint8_t>& o, std::size_t len) {
  if (len < 252) {
    o.push_back(static_cast<std::uint8_t>(len));
  } else {
    o.push_back(static_cast<std::uint8_t>(252 + (len & 3u)));
    o.push_back(static_cast<std::uint8_t>((len - 252 - (len & 3u)) / 4));
  }
}

// Undelimited Opus packet -> self-delimited form (RFC 6716 Appendix B).
bool SelfDelimit(const std::uint8_t* p, std::size_t len, std::vector<std::uint8_t>* out) {
  if (len < 1) return false;
  const std::uint32_t code = p[0] & 3u;
  std::size_t pos = 1, used = 0, l = 0;
  std::size_t lastLen = 0;
  if (code == 0) {
    lastLen = len - 1;
  } else if (code == 1) {
    lastLen = (len - 1) / 2;
  } else if (code == 2) {
    if (!opusref::ReadLength(p + 1, len - 1, &used, &l)) return false;
    pos = 1 + used;
    lastLen = len - pos - l;
  } else {
    const std::uint8_t fc = p[1];
    pos = 2;
    std::size_t padding = 0;
    if (fc & 0x40u) {
      for (;;) {
        const std::uint8_t b = p[pos++];
        padding += b == 255 ? 254u : b;
        if (b != 255) break;
      }
    }
    const std::uint32_t m = fc & 0x3Fu;
    if (fc & 0x80u) {
      std::size_t sum = 0;
      for (std::uint32_t i = 0; i + 1 < m; ++i) {
        if (!opusref::ReadLength(p + pos, len - pos, &used, &l)) return false;
        pos += used;
        sum += l;
      }
      lastLen = len - pos - padding - sum;
    } else {
      lastLen = (len - pos - padding) / m;
    }
  }
  out->insert(out->end(), p, p + pos);
  PutLen(*out, lastLen);
  out->insert(out->end(), p + pos, p + len);
  return true;
}

// Interleaves the packets of single-stream encodings into one multistream
// payload per packet (all but the last self-delimited).
bool Combine(const std::vector<const Encoded*>& parts, std::vector<std::uint8_t>* data) {
  std::vector<std::vector<opusref::Packet>> pk(parts.size());
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (!opusref::WalkPackets(parts[i]->data.data(), parts[i]->data.size(), 1, &pk[i]).empty()) return false;
    if (pk[i].size() != pk[0].size()) return false;
  }
  for (std::size_t k = 0; k < pk[0].size(); ++k) {
    std::vector<std::uint8_t> payload;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const std::uint8_t* q = parts[i]->data.data() + pk[i][k].offset + 2;
      if (i + 1 < parts.size()) {
        if (!SelfDelimit(q, pk[i][k].length, &payload)) return false;
      } else {
        payload.insert(payload.end(), q, q + pk[i][k].length);
      }
    }
    data->push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    data->push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    data->insert(data->end(), payload.begin(), payload.end());
  }
  return true;
}

}  // namespace

// Input after the end: every boundary named by the acceptance decision.
SND_TEST(IndepOpusRecheck, InputAfterTheEndIsDroppedAtEveryBoundary) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch : {1u, 2u, 6u}) {
    for (std::uint64_t total : {648u, 4800u, 4801u}) {
      Encoded e;
      SND_REQUIRE(opusref::Encode(Sig(ch, total, ch), ce::OpusSettings{}, &e));
      bool ok = false;
      const Planar ref = opusref::DecodeAll(e, &ok);
      SND_REQUIRE(ok && ref[0].size() == total);
      const std::vector<std::uint8_t> zeros(4096, 0);
      std::vector<std::uint8_t> garbage(3000);
      std::uint64_t s = total;
      for (auto& b : garbage) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::uint8_t>(s >> 56);
      }
      // (a) data + padding in one call, room for everything.
      {
        std::vector<std::uint8_t> d = e.data;
        d.insert(d.end(), zeros.begin(), zeros.begin() + 1500);
        opusref::Dec x(e.h, 100000);
        SND_CHECK_MSG(x.Call(d.data(), static_cast<std::uint32_t>(d.size()), 100000) == total, "%u ch %llu: (a)", ch,
                      static_cast<unsigned long long>(total));
        SND_CHECK(x.Call(nullptr, 0, 100000) == 0u);
      }
      // (b) data, then padding in a separate call, then garbage, then more padding.
      {
        opusref::Dec x(e.h, 100000);
        SND_REQUIRE(x.Feed(e.data.data(), static_cast<std::uint32_t>(e.data.size())));
        SND_CHECK_EQ(x.Call(zeros.data(), 2048, 1000), 0u);
        SND_CHECK_EQ(x.Call(garbage.data(), static_cast<std::uint32_t>(garbage.size()), 1000), 0u);
        SND_CHECK_EQ(x.Call(zeros.data(), 1, 0), 0u);
        SND_CHECK_EQ(x.Call(nullptr, 0, 1000), 0u);
        SND_CHECK(!x.error && Same(ref, x.got));
      }
      // (c) data + padding + garbage in one call, small max_frames (protocol drains).
      {
        std::vector<std::uint8_t> d = e.data;
        d.insert(d.end(), zeros.begin(), zeros.begin() + 700);
        d.insert(d.end(), garbage.begin(), garbage.end());
        opusref::Dec x(e.h, 100);
        SND_CHECK(x.FeedAll(d.data(), d.size(), d.size()));
        SND_CHECK_MSG(!x.error && Same(ref, x.got), "%u ch %llu: (c)", ch, static_cast<unsigned long long>(total));
      }
      // (d) the last packet only partly emitted (max_frames), then padding / garbage
      // before draining the rest; then the drain still gives exactly the tail.
      for (std::uint32_t max : {1u, 7u, 300u}) {
        opusref::Dec x(e.h, max);
        // Everything but the last packet, drained.
        std::vector<opusref::Packet> pk;
        opusref::WalkPackets(e.data.data(), e.data.size(), e.setup[2], &pk);
        const std::size_t lastOff = pk.back().offset;
        SND_REQUIRE(x.Feed(e.data.data(), static_cast<std::uint32_t>(lastOff)));
        // The last packet: one call, output full -> part of it is pending.
        const std::uint32_t r = x.Call(e.data.data() + lastOff, static_cast<std::uint32_t>(e.data.size() - lastOff), max);
        SND_REQUIRE(r != snd::kDecodeError);
        // Padding and garbage while frames are still pending (not following the
        // drain-first duty, but within the 16 KiB per call).
        const std::uint32_t r2 = x.Call(zeros.data(), 2048, max);
        const std::uint32_t r3 = x.Call(garbage.data(), static_cast<std::uint32_t>(garbage.size()), max);
        SND_CHECK_MSG(r2 != snd::kDecodeError && r3 != snd::kDecodeError, "%u ch %llu max %u: (d) error", ch,
                      static_cast<unsigned long long>(total), max);
        std::uint32_t q = 0;
        while ((q = x.Call(nullptr, 0, max)) == max) {
        }
        SND_CHECK(q != snd::kDecodeError);
        SND_CHECK_MSG(Same(ref, x.got), "%u ch %llu max %u: (d) output differs (%llu frames)", ch,
                      static_cast<unsigned long long>(total), max, static_cast<unsigned long long>(x.Frames()));
      }
      // Garbage BEFORE the end is still an error (the new rule is only after the end).
      {
        std::vector<std::uint8_t> d(e.data.begin(), e.data.begin() + 2 + (e.data[0] | (e.data[1] << 8)));
        d.push_back(0);
        d.push_back(0);  // a zero length where the second packet should be
        opusref::Dec x(e.h, 100000);
        const std::uint32_t r = x.Call(d.data(), static_cast<std::uint32_t>(d.size()), 100000);
        if (total > 648u) SND_CHECK_MSG(r == snd::kDecodeError, "%u ch %llu: zero length before the end accepted", ch,
                                        static_cast<unsigned long long>(total));
        else SND_CHECK_EQ(r, 648u);  // one packet already covers 312 + 648 = 960: the zeros are past the end
      }
      // Seek past the end: everything given is dropped.
      {
        opusref::Dec x(e.h, 1000);
        const std::uint64_t f = ((total + 312u) / 960u + 1u) * 960u - 312u;  // a boundary after the end
        x.c->Seek(x.mem.get(), f);
        SND_CHECK_EQ(x.Call(garbage.data(), static_cast<std::uint32_t>(garbage.size()), 1000), 0u);
      }
    }
  }
}

// extra_bytes must be exactly 4 + channels (shorter or longer is invalid).
SND_TEST(IndepOpusRecheck, ExtraBytesExactly4PlusChannels) {
  if (!snd::codec::OpusAvailable()) return;
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    std::vector<std::uint8_t> st(4 + ch + 4, 0);
    st[0] = 1;
    st[1] = static_cast<std::uint8_t>(ch);
    st[2] = static_cast<std::uint8_t>(ch);  // ch mono streams: mapping i -> i
    st[3] = 0;
    for (std::uint32_t i = 0; i < ch; ++i) st[4 + i] = static_cast<std::uint8_t>(i);
    snd::CodecHeader h{SND_CODEC_OPUS, ch, 48000, 960, 9600, 312, 3840, 0, 0, st.data()};
    for (std::uint32_t bytes = 0; bytes < st.size(); ++bytes) {
      h.extra_bytes = bytes;
      const auto err = snd::codec::ValidateHeader(h);
      if (bytes == 4 + ch) {
        SND_CHECK_MSG(err == snd::codec::HeaderError::None, "%u ch: exact size refused", ch);
      } else {
        SND_CHECK_MSG(err == snd::codec::HeaderError::BadCodecSetup, "%u ch: %u bytes gave %s", ch, bytes,
                      snd::codec::HeaderErrorName(err));
      }
    }
  }
}

// 1-2 ch setups with N != 1 (RFC 7845) are accepted and decode each stream like
// a stand-alone decoder: two mono encodings combined into one N = 2, M = 0
// multistream give exactly the two mono decodes, in the order of the mapping.
SND_TEST(IndepOpusRecheck, OneOrTwoChannelsWithSeveralStreams) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded a, b;
  SND_REQUIRE(opusref::Encode(Sig(1, 9600, 1), ce::OpusSettings{}, &a));
  ce::OpusSettings voice;
  voice.bitrate = 16000;
  voice.signal = ce::OpusSignal::Voice;  // SILK / hybrid TOCs in the delimited stream too
  SND_REQUIRE(opusref::Encode(Sig(1, 9600, 2), voice, &b));
  bool ok = false;
  const Planar da = opusref::DecodeAll(a, &ok);
  SND_REQUIRE(ok);
  const Planar db = opusref::DecodeAll(b, &ok);
  SND_REQUIRE(ok);
  std::vector<std::uint8_t> ab, ba;
  SND_REQUIRE(Combine({&a, &b}, &ab));
  SND_REQUIRE(Combine({&b, &a}, &ba));
  // The reference parser agrees with the combination (2 streams, 960 each).
  std::vector<opusref::Packet> pk;
  SND_REQUIRE(opusref::WalkPackets(ab.data(), ab.size(), 2, &pk).empty());
  struct Case {
    const std::vector<std::uint8_t>* data;
    std::vector<std::uint8_t> setup;
    std::vector<const Planar*> want;  // per output channel, nullptr = silence
  };
  const Case cases[] = {
      {&ab, {1, 2, 2, 0, 0, 1}, {&da, &db}},
      {&ab, {1, 2, 2, 0, 1, 0}, {&db, &da}},
      {&ba, {1, 2, 2, 0, 0, 1}, {&db, &da}},
      {&ab, {1, 1, 2, 0, 1}, {&db}},          // 1 ch reading the second of two streams
      {&ab, {1, 2, 2, 0, 255, 0}, {nullptr, &da}},
  };
  for (const Case& c : cases) {
    Encoded e;
    e.h = a.h;
    e.h.channels = c.setup[1];
    e.setup = c.setup;
    e.h.extra = e.setup.data();
    e.h.extra_bytes = static_cast<std::uint32_t>(e.setup.size());
    e.data = *c.data;
    SND_REQUIRE(snd::codec::ValidateHeader(e.h) == snd::codec::HeaderError::None);
    const Planar out = opusref::DecodeAll(e, &ok, 777, 1000);
    SND_REQUIRE_MSG(ok, "setup ch %u map %u", c.setup[1], c.setup[4]);
    for (std::size_t i = 0; i < c.want.size(); ++i) {
      if (c.want[i] == nullptr) {
        bool zero = true;
        for (float v : out[i]) zero = zero && v == 0.0f;
        SND_CHECK(zero && out[i].size() == 9600u);
      } else {
        SND_CHECK_MSG(out[i] == (*c.want[i])[0], "setup ch %u: output %zu is not the expected stream", c.setup[1], i);
      }
    }
  }
}

// Seek(f > 0) off a packet boundary: the next Decode fails and keeps failing
// until a valid Seek or Init (bank-format.md §6.5, "ADPCM と同じ").
SND_TEST(IndepOpusRecheck, MisalignedSeekIsStickyUntilSeekOrInit) {
  if (!snd::codec::OpusAvailable()) return;
  Encoded e;
  SND_REQUIRE(opusref::Encode(Sig(2, 9600, 4), ce::OpusSettings{}, &e));
  bool ok = false;
  const Planar ref = opusref::DecodeAll(e, &ok);
  SND_REQUIRE(ok);
  for (int recover = 0; recover < 3; ++recover) {
    opusref::Dec x(e.h, 1000);
    x.c->Seek(x.mem.get(), 1000);  // 1000 + 312 is not a multiple of 960
    for (int i = 0; i < 3; ++i) SND_CHECK_EQ(x.Call(e.data.data(), 100, 1000), snd::kDecodeError);
    SND_CHECK_EQ(x.Call(nullptr, 0, 1000), snd::kDecodeError);
    x.c->Seek(x.mem.get(), 2000);  // still misaligned: still failing
    SND_CHECK_EQ(x.Call(nullptr, 0, 1000), snd::kDecodeError);
    if (recover == 0) x.c->Seek(x.mem.get(), 0);
    if (recover == 1) x.c->Init(x.mem.get(), e.h);
    if (recover == 2) x.c->Seek(x.mem.get(), 960u * 2u - 312u);
    x.got.assign(2, {});
    x.error = false;
    std::size_t from = 0;
    if (recover == 2) {
      std::vector<opusref::Packet> pk;
      opusref::WalkPackets(e.data.data(), e.data.size(), 1, &pk);
      from = pk[2].offset;
    }
    SND_REQUIRE(x.FeedAll(e.data.data() + from, e.data.size() - from, 500));
    const std::uint64_t skip = recover == 2 ? 960u * 2u - 312u : 0u;
    SND_CHECK_EQ(x.Frames(), 9600u - skip);
    bool same = x.Frames() == 9600u - skip;
    for (std::uint32_t c = 0; c < 2 && same && recover != 2; ++c) same = x.got[c] == ref[c];
    SND_CHECK(same);
  }
}

// §6.5 channel table: in the build tool's setup, the WAV channels that the
// table puts into one Vorbis pair (L R, BL BR, SL SR) are the two channels of
// one coupled stream, C / BC / LFE are uncoupled, and the LFE stream is coded
// as LFE (narrowband CELT, RFC 6716 TOC configs 16..19). Together with
// IndepOpusDecode.ChannelIdentity1To8 (each WAV channel comes back in place)
// this pins the table.
SND_TEST(IndepOpusRecheck, ChannelTableOfSection65) {
  if (!snd::codec::OpusAvailable()) return;
  struct Layout {
    std::vector<std::pair<int, int>> pairs;  // WAV indices
    std::vector<int> mono;
    int lfe;  // WAV index, -1 = none
  };
  const Layout layouts[9] = {
      {}, {{}, {0}, -1}, {{{0, 1}}, {}, -1},
      {{{0, 1}}, {2}, -1},                  // L R C
      {{{0, 1}, {2, 3}}, {}, -1},           // L R BL BR
      {{{0, 1}, {3, 4}}, {2}, -1},          // L R C BL BR
      {{{0, 1}, {4, 5}}, {2, 3}, 3},        // L R C LFE BL BR
      // L R C LFE BC SL SR: libopus' 6.1 layout also couples C with BC (RFC 7845
      // leaves the coupling to the encoder; the table only fixes the order).
      {{{0, 1}, {5, 6}, {2, 4}}, {3}, 3},
      {{{0, 1}, {4, 5}, {6, 7}}, {2, 3}, 3},  // L R C LFE BL BR SL SR
  };
  for (std::uint32_t ch = 1; ch <= 8; ++ch) {
    const Layout& L = layouts[ch];
    Encoded e;
    SND_REQUIRE(opusref::Encode(Sig(ch, 9600, 10 + ch), ce::OpusSettings{}, &e));
    opusref::Setup s;
    SND_REQUIRE(opusref::ParseSetup(e.h.extra, e.h.extra_bytes, ch, &s).empty());
    SND_CHECK_EQ(s.coupled, static_cast<std::uint32_t>(L.pairs.size()));
    SND_CHECK_EQ(s.streams, static_cast<std::uint32_t>(L.pairs.size() + L.mono.size()));
    // Decoded channel d: d < 2M is coupled stream d / 2, else mono stream M + d - 2M.
    auto streamOf = [&](int wav) { const std::uint32_t d = s.mapping[wav]; return d < 2 * s.coupled ? d / 2 : s.coupled + d - 2 * s.coupled; };
    for (const auto& pr : L.pairs) {
      const std::uint32_t d0 = s.mapping[pr.first], d1 = s.mapping[pr.second];
      SND_CHECK_MSG(d0 < 2 * s.coupled && d1 < 2 * s.coupled && d0 / 2 == d1 / 2 && d0 != d1,
                    "%u ch: WAV %d and %d are not one coupled stream (decoded %u, %u)", ch, pr.first, pr.second, d0, d1);
      SND_CHECK_MSG(d0 < d1, "%u ch: WAV %d should be the left of its pair", ch, pr.first);
    }
    for (int m : L.mono) SND_CHECK_MSG(s.mapping[m] >= 2 * s.coupled, "%u ch: WAV %d is coupled", ch, m);
    if (L.lfe >= 0) {
      // Every packet of the LFE stream is narrowband CELT (TOC config 16..19).
      const std::uint32_t target = streamOf(L.lfe);
      std::vector<opusref::Packet> pk;
      SND_REQUIRE(opusref::WalkPackets(e.data.data(), e.data.size(), s.streams, &pk).empty());
      std::uint32_t lfeLike = 0, others = 0;
      for (const auto& p : pk) {
        const std::uint8_t* q = e.data.data() + p.offset + 2;
        std::size_t off = 0;
        for (std::uint32_t st = 0; st < s.streams; ++st) {
          std::size_t used = 0;
          std::uint32_t n = 0;
          SND_REQUIRE(opusref::ParsePacket(q + off, p.length - off, st + 1 < s.streams, &used, &n));
          const std::uint32_t config = q[off] >> 3;
          const bool nb = config >= 16 && config <= 19;
          if (st == target) lfeLike += nb ? 1u : 0u;
          else others += nb ? 1u : 0u;
          off += used;
        }
      }
      SND_CHECK_MSG(lfeLike == pk.size(), "%u ch: LFE stream narrowband in %u of %zu packets", ch, lfeLike, pk.size());
      SND_CHECK_MSG(others == 0u, "%u ch: %u narrowband packets outside the LFE stream", ch, others);
    }
  }
}
