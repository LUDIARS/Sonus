// Independent decoder tests (written by a tester, not the implementer).
// Targets the D-23 semantics in snd/ext.h and codec/README.md "D-23 の実装で決めたこと":
// chunking / max_frames invariance with random steps, tiny streams, trailing
// input past the end, seeking to every packet boundary, seek -> partial read ->
// reseek, seek helpers at their bounds, the holding limit and the caller's
// promise, header field extremes, ADPCM corruption, state relocation.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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

std::vector<std::uint64_t> FrameCountsFor(std::uint32_t fpb) {
  std::vector<std::uint64_t> v = {0, 1, 2, 3, 1500};
  if (fpb > 1) {
    for (std::uint64_t x : {std::uint64_t{fpb} - 1, std::uint64_t{fpb}, std::uint64_t{fpb} + 1,
                            std::uint64_t{2} * fpb + 3, std::uint64_t{3} * fpb})
      v.push_back(x);
  }
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// Random trailing bytes (whole "packets" of junk) placed after the encoded data.
void AppendJunk(std::vector<std::uint8_t>& data, std::uint32_t packetBytes, std::uint32_t packets, Rng& rng) {
  for (std::uint32_t i = 0; i < packets * packetBytes + rng.Below(packetBytes + 1); ++i) {
    data.push_back(static_cast<std::uint8_t>(rng.Next()));
  }
}

}  // namespace

// D-23 / README 2: the output sequence does not depend on how input bytes and
// max_frames are chosen per call. All codecs, 1..8 channels, several ADPCM
// block sizes, tiny streams (0/1/2 frames, < 1 block, non block-aligned
// totals), pre_skip, and junk after the end of the stream (README 3: consumed
// and ignored).
SND_TEST(IndepDecode, RandomStepsMatchOneShotAllShapes) {
  std::uint32_t combos = 0;
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch = 1; ch <= 8; ++ch) {
      const std::vector<std::uint32_t> blocks =
          codec == SND_CODEC_ADPCM ? std::vector<std::uint32_t>{8, 12, 36, 260} : std::vector<std::uint32_t>{0};
      for (std::uint32_t bpc : blocks) {
        const CodecHeader base = Hdr(codec, ch, 0, bpc);
        SND_REQUIRE(snd::codec::ValidateHeader(base) == snd::codec::HeaderError::None);
        const std::uint32_t fpb = snd::codec::PacketFrames(base);
        const std::uint32_t pb = snd::codec::PacketBytes(base);
        for (std::uint64_t total : FrameCountsFor(fpb)) {
          for (std::uint32_t preSkip : {0u, fpb > 1 ? fpb / 2 + 1 : 3u, fpb}) {
            CodecHeader h = base;
            h.total_frames = total;
            h.pre_skip = preSkip;
            const Planar src = MakeSignal(ch, preSkip + total, ch * 7 + bpc);
            std::vector<std::uint8_t> data = EncodeOrDie(h, src);
            SND_REQUIRE_EQ(data.size(), enc::EncodedBytes(h));
            bool ok = false;
            const Planar ref = OneShot(h, data, &ok);
            SND_REQUIRE(ok);
            SND_REQUIRE_EQ(ref[0].size(), total);
            if (codec == SND_CODEC_PCM_F32) SND_CHECK(SameAudio(ref, SliceOf(src, preSkip, total)));
            if (codec != SND_CODEC_PCM_F32) SND_CHECK(AllFinite(ref, 1.0f));
            Rng rng(combos * 977u + 13u);
            AppendJunk(data, pb, rng.Below(3), rng);
            for (int pass = 0; pass < 3; ++pass) {
              snd::ICodec* c = snd::codec::BuiltinCodec(codec);
              Fenced mem(c->StateBytes(h));
              c->Init(mem.get(), h);
              Driver d(c, mem.get(), ch, 2048);
              const std::uint32_t maxChunk = pass == 0 ? 7u : pass == 1 ? pb * 2u + 1u : 16384u;
              const std::uint32_t maxMax = pass == 0 ? 2048u : pass == 1 ? 5u : 300u;
              const bool fed = d.FeedRandom(data.data(), data.size(), rng, maxChunk, maxMax);
              SND_CHECK_MSG(fed && !d.overMax && d.tailOk && mem.Intact() && SameAudio(d.got, ref),
                            "codec %08x ch %u bpc %u total %llu preSkip %u pass %d: fed %d over %d tail %d mem %d "
                            "frames %llu",
                            codec, ch, bpc, static_cast<unsigned long long>(total), preSkip, pass, fed, d.overMax,
                            d.tailOk, mem.Intact(), static_cast<unsigned long long>(d.Frames()));
            }
            ++combos;
          }
        }
      }
    }
  }
  snd::test::Note("random-step combinations: %u", combos);
}

// A zero-length stream: nothing comes out, junk input is consumed silently.
SND_TEST(IndepDecode, EmptyStream) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t preSkip : {0u, 5u}) {
      CodecHeader h = Hdr(codec, 2, 0);
      h.pre_skip = preSkip;
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      Fenced mem(c->StateBytes(h));
      c->Init(mem.get(), h);
      Driver d(c, mem.get(), 2, 64);
      SND_CHECK_EQ(d.Call(nullptr, 0, 64), 0u);
      std::vector<std::uint8_t> junk(3000);
      Rng rng(5);
      for (auto& b : junk) b = static_cast<std::uint8_t>(rng.Next());
      if (preSkip == 0) {
        // Past the end from the first byte: even a corrupt ADPCM header is ignored.
        SND_CHECK_EQ(d.Call(junk.data(), static_cast<std::uint32_t>(junk.size()), 64), 0u);
        SND_CHECK_EQ(d.Call(nullptr, 0, 64), 0u);
      }
      SND_CHECK(d.tailOk);
      SND_CHECK(mem.Intact());
    }
  }
}

// Less than one packet / exactly the first ADPCM frame (header sample only).
SND_TEST(IndepDecode, ShorterThanOneBlock) {
  for (std::uint32_t ch : {1u, 3u, 8u}) {
    const CodecHeader h = Hdr(SND_CODEC_ADPCM, ch, 1);
    const Planar src = MakeSignal(ch, 1, 2);
    const auto data = EncodeOrDie(h, src);
    SND_REQUIRE_EQ(data.size(), h.block_align);
    snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
    Fenced mem(c->StateBytes(h));
    c->Init(mem.get(), h);
    Driver d(c, mem.get(), ch, 16);
    // 4*ch - 1 bytes: the last channel's header is incomplete -> no frame yet.
    SND_CHECK_EQ(d.Call(data.data(), 4 * ch - 1, 16), 0u);
    SND_CHECK_EQ(d.Call(nullptr, 0, 16), 0u);
    SND_CHECK_EQ(d.Call(data.data() + 4 * ch - 1, 1, 16), 1u);  // header sample complete
    SND_CHECK_EQ(d.Call(data.data() + 4 * ch, h.block_align - 4 * ch, 16), 0u);  // padding only
    for (std::uint32_t k = 0; k < ch; ++k) {
      const float expect = static_cast<float>(static_cast<std::int16_t>(data[4 * k] | (data[4 * k + 1] << 8))) / 32768.0f;
      SND_CHECK_EQ(d.got[k][0], expect);
      SND_CHECK_NEAR(d.got[k][0], src[k][0], 1.0f / 32768.0f);
    }
    SND_CHECK(d.tailOk && mem.Intact());
  }
}

// Seek to every packet boundary (seek table with interval 0) and compare the
// rest with the one-shot decode. The table is also checked against the
// expected packet boundaries computed from the README layout.
SND_TEST(IndepDecode, SeekToEveryPacketBoundary) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 5u, 8u}) {
      for (std::uint32_t bpc : {8u, 36u, 64u}) {
        if (codec != SND_CODEC_ADPCM && bpc != 8u) continue;
        const CodecHeader probe = Hdr(codec, ch, 0, bpc);
        const std::uint32_t fpb = snd::codec::PacketFrames(probe);
        const std::uint32_t pb = snd::codec::PacketBytes(probe);
        for (std::uint32_t preSkip : {0u, fpb > 1 ? fpb / 2 + 1 : 2u, fpb}) {
          CodecHeader h = probe;
          h.total_frames = codec == SND_CODEC_ADPCM ? 1200u + ch * 17u : 260u;
          h.pre_skip = preSkip;
          const auto data = EncodeOrDie(h, MakeSignal(ch, preSkip + h.total_frames, 4));
          bool ok = false;
          const Planar ref = OneShot(h, data, &ok);
          SND_REQUIRE(ok);
          // Expected entries: {0,0} plus every packet start p*fpb with pre < p*fpb < pre+total.
          std::vector<std::uint64_t> ef = {0}, eo = {0};
          for (std::uint64_t p = 1;; ++p) {
            const std::uint64_t start = p * fpb;
            if (start >= preSkip + h.total_frames) break;
            if (start <= preSkip) continue;
            ef.push_back(start - preSkip);
            eo.push_back(p * pb);
          }
          const std::uint32_t count = enc::BuildSeekTable(h, 0, nullptr, nullptr, 0);
          SND_REQUIRE_EQ(count, ef.size());
          std::vector<std::uint64_t> sf(count), so(count);
          SND_REQUIRE_EQ(enc::BuildSeekTable(h, 0, sf.data(), so.data(), count), count);
          SND_CHECK(sf == ef);
          SND_CHECK(so == eo);
          snd::ICodec* c = snd::codec::BuiltinCodec(codec);
          Fenced mem(c->StateBytes(h));
          c->Init(mem.get(), h);
          Rng rng(ch * 1000u + bpc + preSkip);
          for (std::uint32_t e = 0; e < count; ++e) {
            snd::codec::PacketPosition pos{};
            SND_REQUIRE(snd::codec::PacketForFrame(h, sf[e], &pos));
            SND_CHECK_EQ(pos.frame, sf[e]);
            SND_CHECK_EQ(pos.byteOffset, so[e]);
            c->Seek(mem.get(), sf[e]);
            Driver d(c, mem.get(), ch, 512);
            SND_REQUIRE(d.FeedRandom(data.data() + so[e], data.size() - so[e], rng, 3u * pb + 1u, 512));
            SND_CHECK_MSG(d.tailOk && SameAudio(d.got, SliceOf(ref, sf[e], h.total_frames - sf[e])),
                          "codec %08x ch %u bpc %u pre %u entry %u frame %llu", codec, ch, bpc, preSkip, e,
                          static_cast<unsigned long long>(sf[e]));
          }
          SND_CHECK(mem.Intact());
        }
      }
    }
  }
}

// One state: Seek -> read a random part -> Seek again (no Init in between),
// with random pre_roll (also larger than the target) and targets past the end.
// Occasionally Seek(0) (pre_skip again) and a bad seek (non-boundary, error)
// that the next Seek must clear.
SND_TEST(IndepDecode, SeekPartialReadReseekRandomWalk) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 6u}) {
      CodecHeader h = Hdr(codec, ch, codec == SND_CODEC_ADPCM ? 9000u : 4000u, 36);
      const std::uint32_t fpb = snd::codec::PacketFrames(h);
      h.pre_skip = codec == SND_CODEC_ADPCM ? fpb + 7u : 11u;
      h.pre_roll = codec == SND_CODEC_ADPCM ? 2u * fpb : 40u;
      const auto data = EncodeOrDie(h, MakeSignal(ch, h.pre_skip + h.total_frames, 21));
      bool ok = false;
      const Planar ref = OneShot(h, data, &ok);
      SND_REQUIRE(ok);
      const std::uint32_t count = enc::BuildSeekTable(h, fpb * 3u, nullptr, nullptr, 0);
      std::vector<std::uint64_t> sf(count), so(count);
      enc::BuildSeekTable(h, fpb * 3u, sf.data(), so.data(), count);
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      Fenced mem(c->StateBytes(h));
      c->Init(mem.get(), h);
      Rng rng(codec + ch);
      for (int step = 0; step < 250; ++step) {
        const std::uint32_t kind = rng.Below(10);
        if (kind == 0 && codec == SND_CODEC_ADPCM) {
          c->Seek(mem.get(), fpb * 2u + 1u);  // not a block start: next Decode fails
          Driver bad(c, mem.get(), ch, 64);
          SND_CHECK_EQ(bad.Call(data.data(), 64, 64), snd::kDecodeError);
          continue;
        }
        const std::uint64_t target = kind == 1 ? 0 : rng.Below(static_cast<std::uint32_t>(h.total_frames + 300));
        const std::uint32_t preRoll = rng.Below(3) == 0 ? static_cast<std::uint32_t>(target + 1 + rng.Below(5000))
                                                        : h.pre_roll;
        const std::uint32_t e = snd::codec::FindSeekPoint(sf.data(), count, target, preRoll);
        SND_REQUIRE(e < count);
        const std::uint64_t back = target > preRoll ? target - preRoll : 0;
        SND_CHECK(sf[e] <= back);
        if (e + 1 < count) SND_CHECK(sf[e + 1] > back);
        c->Seek(mem.get(), sf[e]);
        Driver d(c, mem.get(), ch, 700);
        const std::size_t avail = data.size() - so[e];
        const std::size_t n = std::min<std::size_t>(avail, rng.Below(static_cast<std::uint32_t>(avail) + 1));
        SND_REQUIRE(d.FeedRandom(data.data() + so[e], n, rng, 600, 700));
        const std::uint64_t remaining = h.total_frames > sf[e] ? h.total_frames - sf[e] : 0;
        SND_CHECK(d.Frames() <= remaining);
        SND_CHECK_MSG(SameAudio(d.got, SliceOf(ref, sf[e], d.Frames())), "codec %08x ch %u step %d target %llu",
                      codec, ch, step, static_cast<unsigned long long>(target));
        if (n == avail) SND_CHECK_EQ(d.Frames(), remaining);
        SND_CHECK(d.tailOk);
      }
      SND_CHECK(mem.Intact());
    }
  }
}

// FindSeekPoint at its bounds (header comment: last entry <= target - preRoll,
// saturating at 0; kNoSeekPoint for count 0).
SND_TEST(IndepDecode, FindSeekPointBounds) {
  const std::uint64_t f[] = {0, 100, 200, 350, 1000};
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 0, 50, 0), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(nullptr, 0, 50, 0), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 1, 0, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 1, UINT64_MAX, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 0, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 99, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 100, 0), 1u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 999, 0), 3u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 1000, 0), 4u);  // last entry exactly
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, UINT64_MAX, 0), 4u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, UINT64_MAX, UINT32_MAX), 4u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 1000, 1), 3u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 300, 100), 2u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 300, 101), 1u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 300, 300), 0u);   // preRoll == target
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 300, 5000), 0u);  // preRoll > target
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 5, 0, UINT32_MAX), 0u);
  // Exhaustive against a linear scan.
  for (std::uint32_t n = 1; n <= 5; ++n) {
    for (std::uint64_t t = 0; t < 1200; t += 7) {
      for (std::uint32_t pr : {0u, 1u, 99u, 250u, 2000u}) {
        const std::uint64_t back = t > pr ? t - pr : 0;
        std::uint32_t lin = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
          if (f[i] <= back) lin = i;
        }
        SND_CHECK_EQ(snd::codec::FindSeekPoint(f, n, t, pr), lin);
      }
    }
  }
}

// PacketForFrame: {0,0} for targets inside the first packet / pre_skip, the
// containing packet otherwise, a consistent pair for the last frame, and false
// for targets at or past total_frames (acceptance decision 2).
SND_TEST(IndepDecode, PacketForFrameBounds) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t preSkip : {0u, 5u, 121u, 300u}) {
      CodecHeader h = Hdr(codec, 2, 5000, 64);
      h.pre_skip = preSkip;
      const std::uint32_t pf = snd::codec::PacketFrames(h);
      const std::uint32_t pb = snd::codec::PacketBytes(h);
      snd::codec::PacketPosition pos{};
      SND_CHECK(!snd::codec::PacketForFrame(h, 0, nullptr));
      SND_REQUIRE(snd::codec::PacketForFrame(h, 0, &pos));
      SND_CHECK_EQ(pos.frame, 0u);
      SND_CHECK_EQ(pos.byteOffset, 0u);
      for (std::uint64_t t : {std::uint64_t{5000}, std::uint64_t{123456}, UINT64_MAX}) {
        SND_CHECK(!snd::codec::PacketForFrame(h, t, &pos));
      }
      for (std::uint64_t t : {std::uint64_t{1}, std::uint64_t{pf}, std::uint64_t{4999}}) {
        SND_REQUIRE(snd::codec::PacketForFrame(h, t, &pos));
        SND_CHECK(pos.frame <= t);
        if (pos.frame == 0) {
          SND_CHECK_EQ(pos.byteOffset, 0u);
          // {0,0} only when the containing packet starts at or before pre_skip.
          SND_CHECK((t + preSkip) / pf * pf <= preSkip);
        } else {
          SND_CHECK_EQ((pos.frame + preSkip) % pf, 0u);
          SND_CHECK_EQ(pos.byteOffset, (pos.frame + preSkip) / pf * pb);
          SND_CHECK(t - pos.frame < pf);  // the containing packet, not an earlier one
        }
      }
      CodecHeader bad = h;
      bad.channels = 0;
      SND_CHECK(!snd::codec::PacketForFrame(bad, 10, &pos));
    }
  }
}

// PacketForFrame must not return an inconsistent {frame, byteOffset} pair when
// the byte offset of the containing packet does not fit in 64 bits (target far
// past any real stream). Expected: false, or a pair with byteOffset =
// (frame + pre_skip) / packetFrames * packetBytes.
SND_TEST(IndepDecode, PacketForFrameHugeTargetIsConsistent) {
  for (std::uint32_t codec : kCodecs) {
    CodecHeader h = Hdr(codec, 8, 5000, 64);
    const std::uint32_t pf = snd::codec::PacketFrames(h);
    const std::uint32_t pb = snd::codec::PacketBytes(h);
    for (std::uint64_t t : {UINT64_MAX, UINT64_MAX / 2, std::uint64_t{1} << 62}) {
      snd::codec::PacketPosition pos{};
      if (!snd::codec::PacketForFrame(h, t, &pos)) continue;
      const std::uint64_t q = (pos.frame + h.pre_skip) / pf;
      const bool representable = q <= UINT64_MAX / pb;
      SND_CHECK_MSG(representable && pos.byteOffset == q * pb,
                    "codec %08x target %llu -> frame %llu byteOffset %llu (does not match the frame)", h.codec,
                    static_cast<unsigned long long>(t), static_cast<unsigned long long>(pos.frame),
                    static_cast<unsigned long long>(pos.byteOffset));
    }
  }
}

// README 4: a caller that keeps the promise (feed <= 16 KiB per call, drain
// until < max_frames before the next feed) never sees an error, for every
// block size up to the 64 KiB maximum and tiny max_frames.
SND_TEST(IndepDecode, HoldPromiseKeptNeverFails) {
  struct Shape {
    std::uint32_t codec, ch, bpc;
  };
  const Shape shapes[] = {{SND_CODEC_PCM_F32, 1, 0},     {SND_CODEC_PCM_F32, 8, 0},    {SND_CODEC_PCM_S16, 3, 0},
                          {SND_CODEC_ADPCM, 1, 8},       {SND_CODEC_ADPCM, 2, 512},    {SND_CODEC_ADPCM, 8, 1024},
                          {SND_CODEC_ADPCM, 1, 65536},   {SND_CODEC_ADPCM, 8, 8192},   {SND_CODEC_ADPCM, 2, 32768},
                          {SND_CODEC_ADPCM, 3, 20000}};
  for (const Shape& s : shapes) {
    CodecHeader h = Hdr(s.codec, s.ch, 0, s.bpc);
    SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
    const std::uint32_t pf = snd::codec::PacketFrames(h);
    const std::uint32_t pb = snd::codec::PacketBytes(h);
    h.total_frames = s.codec == SND_CODEC_ADPCM ? 2ull * pf + pf / 3 : 70000ull / s.ch;
    h.pre_skip = pf > 1 ? pf / 2 : 100;
    const auto data = EncodeOrDie(h, MakeSignal(s.ch, h.pre_skip + h.total_frames, 8));
    bool ok = false;
    const Planar ref = OneShot(h, data, &ok);
    SND_REQUIRE(ok);
    for (std::uint32_t max : {1u, 2u, 17u, 1000u}) {
      if (max == 1u && pf > 20000u) continue;  // ~400k calls; covered by 2
      for (int mode = 0; mode < 2; ++mode) {
        snd::ICodec* c = snd::codec::BuiltinCodec(s.codec);
        Fenced mem(c->StateBytes(h));
        c->Init(mem.get(), h);
        Driver d(c, mem.get(), s.ch, 1000);
        Rng rng(max + mode);
        std::size_t off = 0;
        bool fine = true;
        while (off < data.size() && fine) {
          std::uint32_t n = mode == 0 ? snd::codec::kHoldInputBytes : 1u + rng.Below(snd::codec::kHoldInputBytes);
          if (n > data.size() - off) n = static_cast<std::uint32_t>(data.size() - off);
          fine = d.Feed(data.data() + off, n, max);
          off += n;
        }
        fine = fine && d.Feed(nullptr, 0, max);
        SND_CHECK_MSG(fine && d.tailOk && mem.Intact() && SameAudio(d.got, ref),
                      "codec %08x ch %u block %u max %u mode %d: fine %d frames %llu", s.codec, s.ch, pb, max, mode,
                      fine, static_cast<unsigned long long>(d.Frames()));
      }
    }
  }
}

// README 4: the exact limit. With max_frames = 1, PCM consumes one frame per
// call, ADPCM none of the block (fpb > 1): the bytes left in the state decide.
// Breaking the promise errors without touching memory outside the state, the
// error is sticky, and Seek / Init recover.
SND_TEST(IndepDecode, HoldLimitBoundaryAndRecovery) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 8u}) {
      CodecHeader h = Hdr(codec, ch, 0, 512);
      const std::uint32_t pb = snd::codec::PacketBytes(h);
      const std::uint32_t cap = snd::codec::HoldCapacityBytes(h);
      SND_CHECK_EQ(cap, snd::codec::kHoldInputBytes + pb);
      h.total_frames = (3ull * cap) / pb * snd::codec::PacketFrames(h);
      const auto data = EncodeOrDie(h, MakeSignal(ch, h.total_frames, 3));
      bool ok = false;
      const Planar ref = OneShot(h, data, &ok);
      SND_REQUIRE(ok);
      const std::uint32_t consumedByOneFrame = codec == SND_CODEC_ADPCM ? 0u : pb;
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      for (std::uint32_t extra : {0u, 1u}) {
        Fenced mem(c->StateBytes(h));
        c->Init(mem.get(), h);
        Driver d(c, mem.get(), ch, 8);
        const std::uint32_t n = cap + consumedByOneFrame + extra;
        const std::uint32_t r = d.Call(data.data(), n, 1);
        if (extra == 0u) {
          SND_CHECK_EQ(r, 1u);
          // Drain the rest: everything held must come out in order.
          while (d.Call(nullptr, 0, 8) == 8u) {
          }
          SND_CHECK(!d.error);
          const std::uint64_t got = d.Frames();
          SND_CHECK(SameAudio(d.got, SliceOf(ref, 0, got)));
        } else {
          SND_CHECK_EQ(r, snd::kDecodeError);
          SND_CHECK_EQ(d.Call(nullptr, 0, 8), snd::kDecodeError);             // sticky
          SND_CHECK_EQ(d.Call(data.data(), 16, 8), snd::kDecodeError);        // sticky
          c->Seek(mem.get(), 0);
          Driver again(c, mem.get(), ch, 256);
          Rng rng(ch);
          SND_CHECK(again.FeedRandom(data.data(), data.size(), rng, 4000, 256));
          SND_CHECK(SameAudio(again.got, ref));
          // Break it again by not draining, then recover with Init.
          Driver bad(c, mem.get(), ch, 8);
          c->Seek(mem.get(), 0);
          std::uint32_t off = 0, rr = 0;
          while (off + snd::codec::kHoldInputBytes <= data.size()) {
            rr = bad.Call(data.data() + off, snd::codec::kHoldInputBytes, 1);
            off += snd::codec::kHoldInputBytes;
            if (rr == snd::kDecodeError) break;
          }
          SND_CHECK_EQ(rr, snd::kDecodeError);
          SND_CHECK(bad.tailOk);
          SND_CHECK(SameAudio(bad.got, SliceOf(ref, 0, bad.Frames())));
          c->Init(mem.get(), h);
          Driver fresh(c, mem.get(), ch, 256);
          SND_CHECK(fresh.FeedRandom(data.data(), data.size(), rng, 16384, 256));
          SND_CHECK(SameAudio(fresh.got, ref));
        }
        SND_CHECK(d.tailOk);
        SND_CHECK(mem.Intact());
      }
    }
  }
}

// Each header field set to an extreme on its own. Expected classification per
// codec.hpp / README; the decoder must never touch memory outside the state
// or the output for any of them.
SND_TEST(IndepDecode, HeaderFieldExtremes) {
  using HE = snd::codec::HeaderError;
  std::uint8_t tinyExtra[4] = {1, 2, 3, 4};
  struct Case {
    const char* what;
    CodecHeader h;
    HE expect;
  };
  for (std::uint32_t codec : kCodecs) {
    const CodecHeader b = Hdr(codec, 2, 1000, 64);
    std::vector<Case> cases;
    auto add = [&](const char* w, CodecHeader h, HE e) { cases.push_back({w, h, e}); };
    CodecHeader t;
    t = b; t.codec = 0; add("codec 0", t, HE::UnknownCodec);
    t = b; t.codec = 0xFFFFFFFFu; add("codec max", t, HE::UnknownCodec);
    t = b; t.channels = 0; add("channels 0", t, HE::BadChannels);
    t = b; t.channels = 9; add("channels 9", t, HE::BadChannels);
    t = b; t.channels = UINT32_MAX; add("channels max", t, HE::BadChannels);
    t = b; t.sample_rate = 0; add("rate 0", t, HE::BadSampleRate);
    t = b; t.sample_rate = UINT32_MAX; add("rate max", t, HE::None);
    t = b; t.total_frames = 0; add("total 0", t, HE::None);
    t = b; t.total_frames = UINT64_MAX; add("total max", t, HE::None);
    t = b; t.total_frames = UINT64_MAX; t.pre_skip = 1; add("total max + pre_skip", t, HE::FrameCountOverflow);
    t = b; t.pre_skip = UINT32_MAX; add("pre_skip max", t, HE::None);
    t = b; t.pre_roll = UINT32_MAX; add("pre_roll max", t, HE::None);
    t = b; t.extra = tinyExtra; t.extra_bytes = UINT32_MAX; add("extra ignored", t, HE::None);
    t = b; t.extra = nullptr; t.extra_bytes = 100; add("extra null", t, HE::None);
    t = b; t.frames_per_packet = UINT32_MAX; add("fpp max", t, HE::BadFramesPerPacket);
    if (codec == SND_CODEC_ADPCM) {
      t = b; t.block_align = 0; add("block 0", t, HE::BadBlockAlign);
      t = b; t.block_align = UINT32_MAX; add("block max", t, HE::BadBlockAlign);
      t = b; t.block_align = 8; add("block header only", t, HE::BadBlockAlign);
      t = b; t.block_align = 16; t.frames_per_packet = 0; add("block min", t, HE::None);
      t = b; t.block_align = 65536 + 8; t.frames_per_packet = 0; add("block > 64 KiB", t, HE::BadBlockAlign);
      t = b; t.block_align = 65536; t.frames_per_packet = 0; add("block 64 KiB", t, HE::None);
      t = b; t.frames_per_packet = 0; add("fpp 0", t, HE::None);
      t = b; t.frames_per_packet = b.frames_per_packet + 1; add("fpp +1", t, HE::BadFramesPerPacket);
    } else {
      t = b; t.block_align = 0; add("block 0", t, HE::None);
      t = b; t.block_align = UINT32_MAX; add("block max", t, HE::BadBlockAlign);
      // Acceptance decision 4: a PCM packet is one frame (frames_per_packet 0 or 1, block_align 0 or the frame size).
      t = b; t.block_align = b.block_align * 4; t.frames_per_packet = 4; add("block x4", t, HE::BadFramesPerPacket);
      t = b; t.block_align = b.block_align * 4; t.frames_per_packet = 1; add("block x4 fpp 1", t, HE::BadBlockAlign);
      t = b; t.block_align = 0; t.frames_per_packet = 1; add("block 0 fpp 1", t, HE::None);
      t = b; t.block_align = 0; t.frames_per_packet = 2; add("block 0 fpp 2", t, HE::BadFramesPerPacket);
      t = b; t.block_align = b.block_align * 4; t.frames_per_packet = 3; add("block x4 fpp 3", t, HE::BadFramesPerPacket);
      t = b; t.frames_per_packet = 0; add("fpp 0", t, HE::None);
    }
    std::vector<std::uint8_t> bytes(6000);
    Rng rng(codec);
    for (auto& x : bytes) x = static_cast<std::uint8_t>(rng.Next());
    for (std::size_t i = 2; codec == SND_CODEC_ADPCM && i < bytes.size(); i += 4) bytes[i] %= 89;
    for (const Case& cs : cases) {
      SND_CHECK_MSG(snd::codec::ValidateHeader(cs.h) == cs.expect, "codec %08x %s: got %u want %u", codec, cs.what,
                    static_cast<unsigned>(snd::codec::ValidateHeader(cs.h)), static_cast<unsigned>(cs.expect));
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      const std::uint32_t sb = c->StateBytes(cs.h);
      SND_CHECK(sb >= snd::codec::kMinStateBytes);
      if (snd::codec::ValidateHeader(cs.h) != HE::None) SND_CHECK_EQ(sb, snd::codec::kMinStateBytes);
      Fenced mem(sb);
      c->Init(mem.get(), cs.h);
      Driver d(c, mem.get(), 8, 300);  // 8 output channels regardless of the header
      for (std::size_t off = 0; off < bytes.size();) {
        const std::uint32_t n = std::min<std::uint32_t>(1u + rng.Below(900), static_cast<std::uint32_t>(bytes.size() - off));
        if (!d.Feed(bytes.data() + off, n, 1 + rng.Below(300))) break;
        off += n;
      }
      c->Seek(mem.get(), 0);
      d.Feed(bytes.data(), 100, 300);
      SND_CHECK_MSG(mem.Intact() && d.tailOk && !d.overMax, "codec %08x %s", codec, cs.what);
      if (cs.expect != HE::None) SND_CHECK_MSG(d.error, "codec %08x %s: invalid header decoded", codec, cs.what);
      if (std::strcmp(cs.what, "pre_skip max") == 0) SND_CHECK_EQ(d.Frames(), 0u);  // all inside pre_skip
    }
  }
}

// ADPCM corruption: step index 89 and 255 in the first, a middle and the last
// (partial) block, inside pre_skip; reserved byte non-zero (ignored by WAV
// decoders); extreme predictors (clamping, UBSan); blocks past the end are not
// validated (README 3: consumed and ignored).
SND_TEST(IndepDecode, AdpcmCorruptBlocks) {
  for (std::uint32_t ch : {1u, 2u, 8u}) {
    CodecHeader h = Hdr(SND_CODEC_ADPCM, ch, 0, 36);
    const std::uint32_t fpb = snd::codec::PacketFrames(h);
    h.total_frames = 4ull * fpb + fpb / 2;  // 5 blocks, the last one partial
    const auto good = EncodeOrDie(h, MakeSignal(ch, h.total_frames, 9));
    bool ok = false;
    const Planar ref = OneShot(h, good, &ok);
    SND_REQUIRE(ok);
    const std::uint32_t blocks = static_cast<std::uint32_t>(good.size() / h.block_align);
    SND_REQUIRE_EQ(blocks, 5u);
    for (std::uint32_t blk : {0u, 2u, 4u}) {
      for (std::uint8_t bad : {std::uint8_t{89}, std::uint8_t{255}}) {
        auto data = good;
        data[blk * h.block_align + 4 * (ch - 1) + 2] = bad;  // last channel's step index
        snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
        Fenced mem(c->StateBytes(h));
        c->Init(mem.get(), h);
        Driver d(c, mem.get(), ch, 100);
        Rng rng(blk + bad);
        SND_CHECK(!d.FeedRandom(data.data(), data.size(), rng, 300, 100));
        SND_CHECK(d.Frames() <= static_cast<std::uint64_t>(blk + 1) * fpb);
        SND_CHECK(SameAudio(d.got, SliceOf(ref, 0, d.Frames())));
        // Sticky; Seek to a good block recovers.
        SND_CHECK_EQ(d.Call(nullptr, 0, 10), snd::kDecodeError);
        if (blk + 1 < blocks) {
          c->Seek(mem.get(), static_cast<std::uint64_t>(blk + 1) * fpb);
          Driver after(c, mem.get(), ch, 100);
          SND_CHECK(after.FeedRandom(data.data() + (blk + 1) * h.block_align,
                                     data.size() - (blk + 1) * h.block_align, rng, 300, 100));
          SND_CHECK(SameAudio(after.got, SliceOf(ref, (blk + 1) * fpb, h.total_frames - (blk + 1) * fpb)));
        }
        SND_CHECK(mem.Intact() && d.tailOk);
      }
    }
    // Reserved header byte set: still decodes identically.
    {
      auto data = good;
      for (std::uint32_t b = 0; b < blocks; ++b) {
        for (std::uint32_t k = 0; k < ch; ++k) data[b * h.block_align + 4 * k + 3] = 0xFF;
      }
      bool ok2 = false;
      SND_CHECK(SameAudio(OneShot(h, data, &ok2), ref));
      SND_CHECK(ok2);
    }
    // A corrupt block entirely past total_frames is never looked at.
    {
      CodecHeader shortH = h;
      shortH.total_frames = 2ull * fpb;  // blocks 2.. are past the end
      auto data = good;
      data[3 * h.block_align + 2] = 200;
      bool ok3 = false;
      const Planar p = OneShot(shortH, data, &ok3);
      SND_CHECK(ok3);
      SND_CHECK(SameAudio(p, SliceOf(ref, 0, 2ull * fpb)));
    }
  }
  // Extreme predictors with the largest steps: output stays in [-1, 1).
  for (std::uint32_t ch : {1u, 2u}) {
    const CodecHeader h = Hdr(SND_CODEC_ADPCM, ch, 0, 64);
    const std::uint32_t fpb = snd::codec::PacketFrames(h);
    CodecHeader hh = h;
    hh.total_frames = 40ull * fpb;
    std::vector<std::uint8_t> data(40 * h.block_align);
    Rng rng(ch + 40);
    for (std::uint32_t b = 0; b < 40; ++b) {
      std::uint8_t* blk = data.data() + b * h.block_align;
      for (std::uint32_t i = 0; i < h.block_align; ++i) blk[i] = (b & 1) ? 0x77 : static_cast<std::uint8_t>(rng.Next());
      for (std::uint32_t k = 0; k < ch; ++k) {
        const std::uint16_t pred = (b % 3 == 0) ? 0x7FFF : (b % 3 == 1) ? 0x8000 : static_cast<std::uint16_t>(rng.Next());
        blk[4 * k] = static_cast<std::uint8_t>(pred);
        blk[4 * k + 1] = static_cast<std::uint8_t>(pred >> 8);
        blk[4 * k + 2] = static_cast<std::uint8_t>(b % 2 ? 88 : rng.Below(89));
      }
    }
    bool ok = false;
    const Planar p = OneShot(hh, data, &ok);
    SND_CHECK(ok);
    SND_CHECK_EQ(p[0].size(), hh.total_frames);
    SND_CHECK(AllFinite(p, 1.0f));
    for (const auto& cc : p) {
      for (float v : cc) SND_CHECK(v >= -1.0f && v <= 32767.0f / 32768.0f);
    }
  }
}

// PCM_F32 decodes NaN / +-Inf bit patterns as 0 (acceptance decision 5: corrupt
// data must not reach feedback DSP); every other value passes bit-exactly.
// PCM_S16 and ADPCM never produce non-finite output or values outside [-1, 1).
SND_TEST(IndepDecode, NonFiniteOnlyFromF32Data) {
  const CodecHeader h = Hdr(SND_CODEC_PCM_F32, 2, 4);
  std::vector<std::uint8_t> data(32);
  const std::uint32_t bits[8] = {0x7FC00001u, 0x7F800000u, 0xFF800000u, 0x3F800000u,
                                 0x00000001u, 0x80000000u, 0x7F7FFFFFu, 0xFFFFFFFFu};
  const std::uint32_t want[8] = {0u, 0u, 0u, 0x3F800000u, 0x00000001u, 0x80000000u, 0x7F7FFFFFu, 0u};
  for (int i = 0; i < 8; ++i) std::memcpy(&data[static_cast<std::size_t>(i) * 4], &bits[i], 4);  // little-endian host
  bool ok = false;
  const Planar p = OneShot(h, data, &ok);
  SND_REQUIRE(ok);
  for (int i = 0; i < 8; ++i) {
    std::uint32_t got;
    std::memcpy(&got, &p[static_cast<std::size_t>(i % 2)][static_cast<std::size_t>(i / 2)], 4);
    SND_CHECK_EQ(got, want[i]);
  }
}

// The state holds no pointers to itself: a state copied (memcpy) to another
// address mid-stream, with the old copy poisoned, continues the same output.
SND_TEST(IndepDecode, StateSurvivesMemcpyRelocation) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 8u}) {
      CodecHeader h = Hdr(codec, ch, 7000, 64);
      h.pre_skip = 13;
      const auto data = EncodeOrDie(h, MakeSignal(ch, h.pre_skip + h.total_frames, 31));
      bool ok = false;
      const Planar ref = OneShot(h, data, &ok);
      SND_REQUIRE(ok);
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      const std::uint32_t sb = c->StateBytes(h);
      std::vector<Fenced*> mems;
      for (int i = 0; i < 6; ++i) mems.push_back(new Fenced(sb));
      c->Init(mems[0]->get(), h);
      Planar got(ch);
      Rng rng(codec + ch);
      std::size_t off = 0;
      int cur = 0;
      while (off < data.size()) {
        // Leave held bytes and a partial ADPCM block in the state: feed without
        // draining, with little room.
        const std::uint32_t n = std::min<std::uint32_t>(1 + rng.Below(3000), static_cast<std::uint32_t>(data.size() - off));
        Driver d(c, mems[static_cast<std::size_t>(cur)]->get(), ch, 64);
        std::uint32_t r = d.Call(data.data() + off, n, 1 + rng.Below(64));
        off += n;
        SND_REQUIRE(r != snd::kDecodeError);
        const int next = (cur + 1) % 6;
        std::memcpy(mems[static_cast<std::size_t>(next)]->get(), mems[static_cast<std::size_t>(cur)]->get(), sb);
        std::memset(mems[static_cast<std::size_t>(cur)]->get(), 0xEE, sb);
        d.state = mems[static_cast<std::size_t>(next)]->get();
        while (r != snd::kDecodeError) {
          const std::uint32_t max = 1 + rng.Below(64);
          r = d.Call(nullptr, 0, max);
          if (r < max) break;
        }
        SND_REQUIRE(!d.error);
        for (std::uint32_t k = 0; k < ch; ++k) got[k].insert(got[k].end(), d.got[k].begin(), d.got[k].end());
        cur = next;
      }
      SND_CHECK_MSG(SameAudio(got, ref), "codec %08x ch %u: relocated state diverged (%zu frames)", codec, ch,
                    got[0].size());
      for (Fenced* m : mems) {
        SND_CHECK(m->Intact());
        delete m;
      }
    }
  }
}

// A state initialised by one built-in object but decoded through another
// (undocumented misuse): whatever happens, no access outside the state.
SND_TEST(IndepDecode, CrossCodecMisuseStaysInBounds) {
  for (std::uint32_t a : kCodecs) {
    for (std::uint32_t b : kCodecs) {
      if (a == b) continue;
      const CodecHeader h = Hdr(a, 2, 3000, 64);
      const auto data = EncodeOrDie(h, MakeSignal(2, 3000, 1));
      snd::ICodec* ca = snd::codec::BuiltinCodec(a);
      snd::ICodec* cb = snd::codec::BuiltinCodec(b);
      Fenced mem(ca->StateBytes(h));
      ca->Init(mem.get(), h);
      Driver d(cb, mem.get(), 2, 128);
      Rng rng(a ^ b);
      (void)d.FeedRandom(data.data(), data.size(), rng, 5000, 128);
      cb->Seek(mem.get(), 0);
      (void)d.FeedRandom(data.data(), data.size(), rng, 5000, 128);
      SND_CHECK(mem.Intact() && d.tailOk && !d.overMax);
    }
  }
}

// README 2: a frame comes out as soon as all of its bytes are in, without
// waiting for the rest of the packet. Fed one byte at a time, the running
// frame count must equal the count derived from the layout after every byte.
SND_TEST(IndepDecode, FramesComeOutAsSoonAsTheirBytesArrive) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 3u, 8u}) {
      CodecHeader h = Hdr(codec, ch, 0, 36);
      const std::uint32_t fpb = snd::codec::PacketFrames(h);
      const std::uint32_t pb = snd::codec::PacketBytes(h);
      h.total_frames = 3ull * fpb + 2;
      const auto data = EncodeOrDie(h, MakeSignal(ch, h.total_frames, 17));
      // Frames whose bytes are all within the first `n` bytes (layout of README).
      auto expected = [&](std::size_t n) -> std::uint64_t {
        if (codec != SND_CODEC_ADPCM) return n / pb;
        std::uint64_t frames = 0;
        for (std::size_t blk = 0; blk * pb < n; ++blk) {
          const std::size_t have = std::min<std::size_t>(n - blk * pb, pb);
          if (have < 4 * ch) break;
          std::uint64_t f = 1;  // header sample
          for (std::uint32_t k = 0; k + 1 < fpb; ++k) {
            // sample k+1 of channel c is in byte 4ch + (k/8)*4ch + 4c + (k%8)/2
            const std::size_t lastByte = 4 * ch + (k / 8) * 4 * ch + 4 * (ch - 1) + (k % 8) / 2;
            if (lastByte >= have) break;
            ++f;
          }
          frames += f;
          if (have < pb) break;
        }
        return std::min<std::uint64_t>(frames, h.total_frames);
      };
      snd::ICodec* c = snd::codec::BuiltinCodec(codec);
      Fenced mem(c->StateBytes(h));
      c->Init(mem.get(), h);
      Driver d(c, mem.get(), ch, 4096);
      std::uint32_t mismatches = 0;
      for (std::size_t i = 0; i < data.size(); ++i) {
        d.Call(data.data() + i, 1, 4096);
        if (d.Frames() != expected(i + 1)) ++mismatches;
      }
      SND_CHECK_MSG(mismatches == 0 && !d.error, "codec %08x ch %u: %u byte positions with a late/early frame", codec,
                    ch, mismatches);
      SND_CHECK(mem.Intact());
    }
  }
}
