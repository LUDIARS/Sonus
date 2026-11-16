// Decoder tests: round trip, chunking invariance (D-23), seek, pre-skip, hold
// limit, malformed input, RT safety.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "codec_test_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/golden.hpp"
#include "snd/test/test.hpp"

using namespace codec_test;
namespace enc = snd::codec::encode;
using snd::CodecHeader;

namespace {

CodecHeader HeaderFor(std::uint32_t codec, std::uint32_t ch, std::uint64_t frames, std::uint32_t adpcmBlock = 0) {
  if (codec == SND_CODEC_ADPCM) {
    return enc::MakeAdpcmHeader(ch, 48000, frames,
                                adpcmBlock ? adpcmBlock : enc::kDefaultAdpcmBlockBytesPerChannel);
  }
  return enc::MakePcmHeader(codec, ch, 48000, frames);
}

FeedResult Reference(const CodecHeader& h, const std::vector<std::uint8_t>& data) {
  const std::uint64_t frames = h.total_frames + 16;
  return DecodeAll(h, data.data(), data.size(), {static_cast<std::uint32_t>(data.size() ? data.size() : 1)},
                   static_cast<std::uint32_t>(frames));
}

Planar Slice(const Planar& p, std::uint64_t from, std::uint64_t n) {
  Planar o(p.size());
  for (std::size_t c = 0; c < p.size(); ++c) {
    o[c].assign(p[c].begin() + static_cast<std::ptrdiff_t>(from), p[c].begin() + static_cast<std::ptrdiff_t>(from + n));
  }
  return o;
}

const std::uint32_t kCodecs[] = {SND_CODEC_PCM_F32, SND_CODEC_PCM_S16, SND_CODEC_ADPCM};

}  // namespace

SND_TEST(Decode, PcmF32IsBitExact) {
  Planar src = MakeSignal(2, 1000);
  src[0][3] = -0.0f;
  src[0][4] = 1.0f;
  src[1][5] = -1.0f;
  src[1][6] = std::numeric_limits<float>::denorm_min();
  src[0][7] = 3.5f;  // f32 PCM keeps values above full scale
  const CodecHeader h = HeaderFor(SND_CODEC_PCM_F32, 2, 1000);
  auto data = EncodeOrDie(h, src);
  SND_CHECK_EQ(data.size(), 1000u * 8u);
  FeedResult r = Reference(h, data);
  SND_REQUIRE(!r.error);
  SND_CHECK(SameAudio(r.out, src));
}

SND_TEST(Decode, PcmS16RoundTripWithinHalfLsb) {
  Planar src = MakeSignal(3, 2000);
  src[0][10] = 1.0f;   // clamps to 32767
  src[1][11] = -1.0f;  // exactly -32768
  const CodecHeader h = HeaderFor(SND_CODEC_PCM_S16, 3, 2000);
  auto data = EncodeOrDie(h, src);
  FeedResult r = Reference(h, data);
  SND_REQUIRE(!r.error);
  SND_REQUIRE_EQ(Frames(r.out), 2000u);
  double maxErr = 0;
  for (std::uint32_t c = 0; c < 3; ++c) {
    for (std::size_t i = 0; i < 2000; ++i) {
      if (c == 0 && i == 10) continue;
      maxErr = std::fmax(maxErr, std::fabs(static_cast<double>(r.out[c][i]) - src[c][i]));
    }
  }
  SND_CHECK(maxErr <= 0.5 / 32768.0 + 1e-9);
  SND_CHECK_EQ(r.out[0][10], 32767.0f / 32768.0f);
  SND_CHECK_EQ(r.out[1][11], -1.0f);
}

SND_TEST(Decode, AdpcmRoundTripQuality) {
  for (std::uint32_t ch : {1u, 2u, 6u, 8u}) {
    for (std::uint32_t block : {64u, 256u, 512u, 1024u}) {
      Planar src = MakeSignal(ch, 12000, ch);
      const CodecHeader h = HeaderFor(SND_CODEC_ADPCM, ch, 12000, block);
      SND_REQUIRE(snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None);
      auto data = EncodeOrDie(h, src);
      FeedResult r = Reference(h, data);
      SND_REQUIRE(!r.error);
      SND_REQUIRE_EQ(Frames(r.out), 12000u);
      const double snr = SnrDb(src, r.out);
      SND_CHECK_MSG(snr > 25.0, "ch %u block %u: SNR %.1f dB", ch, block, snr);
      if (ch == 2 && block == 512) snd::test::Note("ADPCM stereo 512 B/ch: SNR %.1f dB", snr);
    }
  }
}

// D-23: any chunking / max_frames gives the same output as one big call.
SND_TEST(Decode, ChunkingDoesNotChangeOutput) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 3u, 8u}) {
      const std::uint64_t frames = codec == SND_CODEC_ADPCM ? 5000 : 3001;
      const CodecHeader h = HeaderFor(codec, ch, frames, 64);
      Planar src = MakeSignal(ch, frames, 3);
      auto data = EncodeOrDie(h, src);
      FeedResult ref = Reference(h, data);
      SND_REQUIRE(!ref.error);
      SND_REQUIRE_EQ(Frames(ref.out), frames);
      const std::uint32_t pb = snd::codec::PacketBytes(h);
      const std::vector<std::vector<std::uint32_t>> patterns = {
          {1}, {2, 3, 5}, {pb - 1}, {pb + 1}, {pb}, {pb * 3 + 7}, {7000}, {}};
      for (std::uint32_t maxFrames : {1u, 7u, 256u, 4096u}) {
        for (std::size_t pi = 0; pi < patterns.size(); ++pi) {
          if (maxFrames == 1u && patterns[pi].size() == 1 && patterns[pi][0] == 1u && ch == 8u) continue;  // slow, covered by ch 1-3
          FeedResult r = DecodeAll(h, data.data(), data.size(), patterns[pi], maxFrames,
                                   static_cast<std::uint32_t>(pi + maxFrames));
          SND_CHECK_MSG(!r.error && r.guardsOk && SameAudio(r.out, ref.out),
                        "codec %08x ch %u max %u pattern %zu: error %d guards %d frames %llu", codec, ch,
                        maxFrames, pi, r.error, r.guardsOk, static_cast<unsigned long long>(Frames(r.out)));
        }
      }
    }
  }
}

// Feeding new input without draining is allowed up to the hold capacity.
SND_TEST(Decode, UndrainedInputIsHeldAndReturnedLater) {
  for (std::uint32_t codec : kCodecs) {
    const std::uint64_t frames = 1500;
    const CodecHeader h = HeaderFor(codec, 2, frames, 256);
    auto data = EncodeOrDie(h, MakeSignal(2, frames));
    FeedResult ref = Reference(h, data);
    snd::ICodec* c = snd::codec::BuiltinCodec(codec);
    StateMem mem(c->StateBytes(h));
    c->Init(mem.get(), h);
    OutBuf ob(2, 100);
    Planar out(2);
    // Two chunks with only 100 frames of room each: the rest is held.
    const std::uint32_t half = static_cast<std::uint32_t>(data.size() / 2);
    SND_REQUIRE(data.size() <= snd::codec::HoldCapacityBytes(h));
    std::uint32_t n = c->Decode(mem.get(), data.data(), half, ob.dst(), 100);
    SND_REQUIRE_EQ(n, 100u);
    ob.AppendTo(out, n);
    n = c->Decode(mem.get(), data.data() + half, static_cast<std::uint32_t>(data.size() - half), ob.dst(), 100);
    SND_REQUIRE_EQ(n, 100u);
    ob.AppendTo(out, n);
    for (;;) {
      n = c->Decode(mem.get(), nullptr, 0, ob.dst(), 100);
      SND_REQUIRE(n != snd::kDecodeError);
      ob.AppendTo(out, n);
      if (n < 100u) break;
    }
    SND_CHECK(SameAudio(out, ref.out));
    SND_CHECK(mem.GuardIntact());
    SND_CHECK(ob.GuardIntact());
  }
}

SND_TEST(Decode, HoldOverflowFailsAndSeekRecovers) {
  for (std::uint32_t codec : kCodecs) {
    const CodecHeader h = HeaderFor(codec, 1, 200000);
    auto data = EncodeOrDie(h, MakeSignal(1, 200000));
    const std::uint32_t cap = snd::codec::HoldCapacityBytes(h);
    SND_REQUIRE(cap >= snd::codec::kHoldInputBytes);
    SND_REQUIRE(data.size() > cap + 1u);
    snd::ICodec* c = snd::codec::BuiltinCodec(codec);
    StateMem mem(c->StateBytes(h));
    c->Init(mem.get(), h);
    OutBuf ob(1, 16);
    // max_frames = 0: everything is held. Exactly the capacity fits.
    SND_CHECK_EQ(c->Decode(mem.get(), data.data(), cap, ob.dst(), 0), 0u);
    SND_CHECK_EQ(c->Decode(mem.get(), data.data() + cap, 1, ob.dst(), 0), snd::kDecodeError);
    // Sticky until Seek.
    SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 0, ob.dst(), 16), snd::kDecodeError);
    c->Seek(mem.get(), 0);
    SND_CHECK_EQ(c->Decode(mem.get(), data.data(), snd::codec::PacketBytes(h), ob.dst(), 16),
                 codec == SND_CODEC_ADPCM ? 16u : 1u);
    SND_CHECK(mem.GuardIntact());
  }
}

// Seek(frame) + discarding (target - frame) equals the continuous decode.
SND_TEST(Decode, SeekMatchesContinuousDecode) {
  for (std::uint32_t codec : kCodecs) {
    for (std::uint32_t ch : {1u, 2u, 5u}) {
      for (std::uint32_t preRoll : {0u, 300u}) {
        const std::uint64_t frames = 20000;
        CodecHeader h = HeaderFor(codec, ch, frames, 128);
        h.pre_roll = preRoll;  // the codec ignores it; the caller backs off by it
        auto data = EncodeOrDie(h, MakeSignal(ch, frames, 11));
        FeedResult ref = Reference(h, data);
        SND_REQUIRE_EQ(Frames(ref.out), frames);
        const std::uint32_t count = enc::BuildSeekTable(h, 1000, nullptr, nullptr, 0);
        std::vector<std::uint64_t> sf(count), so(count);
        SND_REQUIRE_EQ(enc::BuildSeekTable(h, 1000, sf.data(), so.data(), count), count);
        snd::ICodec* c = snd::codec::BuiltinCodec(codec);
        StateMem mem(c->StateBytes(h));
        c->Init(mem.get(), h);
        Rng rng(ch * 31 + preRoll);
        std::vector<std::uint64_t> targets = {0, 1, 999, 1000, 1017, frames - 1, frames - 300};
        for (int i = 0; i < 12; ++i) targets.push_back(rng.Below(static_cast<std::uint32_t>(frames)));
        for (std::uint64_t target : targets) {
          const std::uint32_t e = snd::codec::FindSeekPoint(sf.data(), count, target, h.pre_roll);
          SND_REQUIRE(e != snd::codec::kNoSeekPoint);
          SND_REQUIRE(sf[e] + h.pre_roll <= target || sf[e] == 0u);
          c->Seek(mem.get(), sf[e]);
          FeedResult r;
          Feed(*c, mem.get(), ch, data.data() + so[e], data.size() - so[e], {}, 333, &r,
               static_cast<std::uint32_t>(target), 900);
          SND_REQUIRE(!r.error);
          const std::uint64_t skip = target - sf[e];
          SND_REQUIRE_EQ(Frames(r.out), frames - sf[e]);
          Planar got = Slice(r.out, skip, frames - target);
          SND_CHECK_MSG(SameAudio(got, Slice(ref.out, target, frames - target)),
                        "codec %08x ch %u target %llu entry %llu", codec, ch,
                        static_cast<unsigned long long>(target), static_cast<unsigned long long>(sf[e]));
        }
        // Without a seek table (fixed-size codecs).
        for (std::uint64_t target : targets) {
          snd::codec::PacketPosition pos{};
          const std::uint64_t back = target > h.pre_roll ? target - h.pre_roll : 0;
          SND_REQUIRE(snd::codec::PacketForFrame(h, back, &pos));
          SND_REQUIRE(pos.frame <= back);
          c->Seek(mem.get(), pos.frame);
          FeedResult r;
          Feed(*c, mem.get(), ch, data.data() + pos.byteOffset, data.size() - pos.byteOffset, {4096}, 4096, &r);
          SND_REQUIRE(!r.error);
          Planar got = Slice(r.out, target - pos.frame, frames - target);
          SND_CHECK(SameAudio(got, Slice(ref.out, target, frames - target)));
        }
        SND_CHECK(mem.GuardIntact());
      }
    }
  }
}

// pre_skip frames are dropped after Init / Seek(0); total_frames clamps the end.
SND_TEST(Decode, PreSkipAndTotalFramesClamp) {
  for (std::uint32_t codec : kCodecs) {
    const std::uint32_t preSkip = codec == SND_CODEC_ADPCM ? 1500u : 37u;
    const std::uint64_t total = 7000;
    CodecHeader h = HeaderFor(codec, 2, total, 100);
    h.pre_skip = preSkip;
    Planar src = MakeSignal(2, preSkip + total, 5);
    auto data = EncodeOrDie(h, src);
    FeedResult ref = Reference(h, data);
    SND_REQUIRE(!ref.error);
    SND_REQUIRE_EQ(Frames(ref.out), total);
    if (codec == SND_CODEC_PCM_F32) {
      SND_CHECK(SameAudio(ref.out, Slice(src, preSkip, total)));
    }
    // Same through odd chunking.
    FeedResult r = DecodeAll(h, data.data(), data.size(), {3, 1, 250}, 13);
    SND_CHECK(SameAudio(r.out, ref.out));
    // Seek into the middle via the seek table (entries skip packets inside pre_skip).
    const std::uint32_t count = enc::BuildSeekTable(h, 0, nullptr, nullptr, 0);
    std::vector<std::uint64_t> sf(count), so(count);
    enc::BuildSeekTable(h, 0, sf.data(), so.data(), count);
    SND_REQUIRE(count >= 2u);
    SND_CHECK_EQ(sf[0], 0u);
    SND_CHECK_EQ(so[0], 0u);
    snd::ICodec* c = snd::codec::BuiltinCodec(codec);
    StateMem mem(c->StateBytes(h));
    c->Init(mem.get(), h);
    for (std::uint64_t target : {0ull, 5ull, 2000ull, 6999ull}) {
      const std::uint32_t e = snd::codec::FindSeekPoint(sf.data(), count, target, 0);
      c->Seek(mem.get(), sf[e]);
      FeedResult s;
      Feed(*c, mem.get(), 2, data.data() + so[e], data.size() - so[e], {}, 64, &s, 3, 700);
      SND_REQUIRE(!s.error);
      SND_REQUIRE_EQ(Frames(s.out), total - sf[e]);
      SND_CHECK(SameAudio(Slice(s.out, target - sf[e], total - target), Slice(ref.out, target, total - target)));
    }
    // A header with fewer frames than the data: output stops at total_frames.
    CodecHeader shorter = h;
    shorter.total_frames = 1234;
    FeedResult t = DecodeAll(shorter, data.data(), data.size(), {}, 500);
    SND_REQUIRE(!t.error);
    SND_CHECK_EQ(Frames(t.out), 1234u);
    SND_CHECK(SameAudio(t.out, Slice(ref.out, 0, 1234)));
  }
}

// A stream cut inside a packet decodes every frame whose bytes arrived.
SND_TEST(Decode, TruncatedInputGivesPrefix) {
  for (std::uint32_t codec : kCodecs) {
    const CodecHeader h = HeaderFor(codec, 3, 3000, 64);
    auto data = EncodeOrDie(h, MakeSignal(3, 3000));
    FeedResult ref = Reference(h, data);
    for (std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{11}, std::size_t{13},
                            data.size() / 3, data.size() - 1}) {
      FeedResult r = DecodeAll(h, data.data(), cut, {}, 77, static_cast<std::uint32_t>(cut));
      SND_REQUIRE(!r.error);
      SND_CHECK(r.guardsOk);
      const std::uint64_t n = Frames(r.out);
      SND_CHECK(n <= 3000u);
      SND_CHECK(SameAudio(r.out, Slice(ref.out, 0, n)));
      if (codec != SND_CODEC_ADPCM) SND_CHECK_EQ(n, cut / snd::codec::PacketBytes(h));
    }
  }
}

SND_TEST(Decode, InvalidHeadersFailCleanly) {
  struct Case {
    CodecHeader h;
    snd::codec::HeaderError expect;
  };
  std::vector<Case> cases;
  CodecHeader base = enc::MakePcmHeader(SND_CODEC_PCM_S16, 2, 48000, 100);
  CodecHeader adp = enc::MakeAdpcmHeader(2, 48000, 100);
  auto add = [&](CodecHeader h, snd::codec::HeaderError e) { cases.push_back({h, e}); };
  CodecHeader t = base; t.channels = 0; add(t, snd::codec::HeaderError::BadChannels);
  t = base; t.channels = 9; add(t, snd::codec::HeaderError::BadChannels);
  t = base; t.sample_rate = 0; add(t, snd::codec::HeaderError::BadSampleRate);
  t = base; t.block_align = 3; add(t, snd::codec::HeaderError::BadBlockAlign);
  t = base; t.block_align = 8; t.frames_per_packet = 3; add(t, snd::codec::HeaderError::BadFramesPerPacket);
  t = base; t.total_frames = UINT64_MAX; t.pre_skip = 1; add(t, snd::codec::HeaderError::FrameCountOverflow);
  // Opus (D-56): built in unless SND_CODEC_OPUS=OFF; a PCM-shaped header is not a valid Opus header.
  t = base; t.codec = SND_CODEC_OPUS;
  add(t, snd::codec::OpusAvailable() ? snd::codec::HeaderError::BadBlockAlign : snd::codec::HeaderError::CodecUnavailable);
  t = base; t.codec = snd::Fourcc('X', 'Y', 'Z', 'W'); add(t, snd::codec::HeaderError::UnknownCodec);
  t = adp; t.block_align = 0; add(t, snd::codec::HeaderError::BadBlockAlign);
  t = adp; t.block_align = 8; add(t, snd::codec::HeaderError::BadBlockAlign);           // header only
  t = adp; t.block_align = 8 + 7; add(t, snd::codec::HeaderError::BadBlockAlign);       // not 4*ch groups
  t = adp; t.block_align = 2 * 40000; add(t, snd::codec::HeaderError::BadBlockAlign);   // > 64 KiB
  t = adp; t.frames_per_packet = 5; add(t, snd::codec::HeaderError::BadFramesPerPacket);
  t = adp; t.channels = 0; add(t, snd::codec::HeaderError::BadChannels);
  std::vector<std::uint8_t> junk(4096, 0x11);
  for (const Case& cs : cases) {
    SND_CHECK_EQ(snd::codec::ValidateHeader(cs.h), cs.expect);
    SND_CHECK_EQ(snd::codec::PacketBytes(cs.h), 0u);
    // Every built-in object must take the invalid header without crashing.
    for (std::uint32_t fourcc : kCodecs) {
      snd::ICodec* c = snd::codec::BuiltinCodec(fourcc);
      const std::uint32_t sb = c->StateBytes(cs.h);
      SND_CHECK_EQ(sb, snd::codec::kMinStateBytes);
      StateMem mem(sb);
      c->Init(mem.get(), cs.h);
      OutBuf ob(8, 64);
      SND_CHECK_EQ(c->Decode(mem.get(), junk.data(), static_cast<std::uint32_t>(junk.size()), ob.dst(), 64),
                   snd::kDecodeError);
      c->Seek(mem.get(), 0);  // stays failed
      SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 0, ob.dst(), 64), snd::kDecodeError);
      SND_CHECK(mem.GuardIntact());
      SND_CHECK(ob.GuardIntact());
    }
  }
  // A header for another codec than the object is rejected too.
  snd::ICodec* pcm = snd::codec::BuiltinCodec(SND_CODEC_PCM_F32);
  SND_CHECK_EQ(pcm->StateBytes(adp), snd::codec::kMinStateBytes);
}

SND_TEST(Decode, MisuseIsRejected) {
  const CodecHeader h = enc::MakeAdpcmHeader(2, 48000, 5000, 64);
  auto data = EncodeOrDie(h, MakeSignal(2, 5000));
  snd::ICodec* c = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
  StateMem mem(c->StateBytes(h));
  OutBuf ob(2, 64);
  // Not initialised (the 0x5A fill is not a valid state).
  SND_CHECK_EQ(c->Decode(mem.get(), data.data(), 64, ob.dst(), 64), snd::kDecodeError);
  c->Init(mem.get(), h);
  SND_CHECK_EQ(c->Decode(mem.get(), nullptr, 5, ob.dst(), 64), snd::kDecodeError);
  SND_CHECK_EQ(c->Decode(mem.get(), data.data(), 64, nullptr, 64), snd::kDecodeError);
  float* oneNull[2] = {ob.dst()[0], nullptr};
  SND_CHECK_EQ(c->Decode(mem.get(), data.data(), 64, oneNull, 64), snd::kDecodeError);
  // dst may be null when max_frames == 0.
  SND_CHECK_EQ(c->Decode(mem.get(), data.data(), 10, nullptr, 0), 0u);
  // Seek to a frame that is not a block start: the next Decode fails.
  const std::uint32_t fpb = snd::codec::PacketFrames(h);
  c->Seek(mem.get(), fpb + 1);
  SND_CHECK_EQ(c->Decode(mem.get(), data.data(), 64, ob.dst(), 64), snd::kDecodeError);
  c->Seek(mem.get(), fpb);
  SND_CHECK(c->Decode(mem.get(), data.data() + h.block_align, 64, ob.dst(), 64) != snd::kDecodeError);
  // Null state.
  SND_CHECK_EQ(c->Decode(nullptr, data.data(), 10, ob.dst(), 64), snd::kDecodeError);
  c->Seek(nullptr, 0);
  c->Init(nullptr, h);
  SND_CHECK(mem.GuardIntact());
}

SND_TEST(Decode, CorruptAdpcmStepIndexIsAnError) {
  const CodecHeader h = enc::MakeAdpcmHeader(2, 48000, 5000, 64);
  auto data = EncodeOrDie(h, MakeSignal(2, 5000));
  data[h.block_align * 2 + 4 + 2] = 89;  // block 2, channel 1 step index
  FeedResult r = DecodeAll(h, data.data(), data.size(), {}, 100);
  SND_CHECK(r.error);
  SND_CHECK(r.guardsOk);
  const std::uint32_t fpb = snd::codec::PacketFrames(h);
  SND_CHECK(Frames(r.out) <= 2u * fpb);  // frames before the corrupt block may have come out
}

// Random headers and random bytes: never crash, never write out of bounds
// (ASan), never return more than max_frames.
SND_TEST(Decode, FuzzRandomInput) {
  Rng rng(2024);
  for (int iter = 0; iter < 300; ++iter) {
    CodecHeader h{};
    h.codec = kCodecs[rng.Below(3)];
    h.channels = rng.Below(10);
    h.sample_rate = rng.Below(4) ? 48000u : 0u;
    h.total_frames = rng.Below(3) ? rng.Below(20000) : UINT64_MAX - rng.Below(3);
    h.pre_skip = rng.Below(4) == 0 ? rng.Below(3000) : 0;
    h.block_align = rng.Below(3) == 0 ? rng.Below(5000) : h.channels * 4u * (1u + rng.Below(64));
    h.frames_per_packet = rng.Below(4) == 0 ? rng.Below(2000) : 0;
    std::vector<std::uint8_t> bytes(rng.Below(20000));
    for (auto& b : bytes) b = static_cast<std::uint8_t>(rng.Next());
    // Mostly-valid step indices for ADPCM so that decoding goes deep.
    if (h.codec == SND_CODEC_ADPCM && rng.Below(2) && h.block_align > 0) {
      for (std::size_t i = 2; i < bytes.size(); i += 4) bytes[i] = static_cast<std::uint8_t>(bytes[i] % 89);
    }
    const std::uint32_t max = 1u + rng.Below(600);
    FeedResult r = DecodeAll(h, bytes.data(), bytes.size(), {}, max, static_cast<std::uint32_t>(iter), 3000);
    SND_CHECK(r.guardsOk);
    if (!r.error && h.channels > 0) {
      for (const auto& ch : r.out) SND_CHECK(ch.size() <= bytes.size() * 2u + 1u);
    }
  }
}

// Everything the decoders do is allocation/lock free: run it inside an RT scope.
SND_TEST(Decode, NoRtViolationsInDecodePath) {
  for (std::uint32_t codec : kCodecs) {
    const CodecHeader h = HeaderFor(codec, 2, 6000, 256);
    auto data = EncodeOrDie(h, MakeSignal(2, 6000));
    snd::ICodec* c = snd::codec::BuiltinCodec(codec);
    StateMem mem(c->StateBytes(h));
    OutBuf ob(2, 512);
    std::uint64_t total = 0;
    {
      SND_RT_SCOPE();
      (void)c->StateBytes(h);
      (void)snd::codec::ValidateHeader(h);
      c->Init(mem.get(), h);
      std::uint32_t off = 0;
      while (off < data.size()) {
        std::uint32_t n = static_cast<std::uint32_t>(data.size() - off);
        if (n > 700) n = 700;
        std::uint32_t got = c->Decode(mem.get(), data.data() + off, n, ob.dst(), 512);
        off += n;
        while (got != snd::kDecodeError) {
          total += got;
          if (got < 512) break;
          got = c->Decode(mem.get(), nullptr, 0, ob.dst(), 512);
        }
      }
      c->Seek(mem.get(), 0);
    }
    SND_CHECK_EQ(total, 6000u);
  }
}

// Golden: the ADPCM decoder output for a fixed encoded signal must not change
// (bit-exact; catches accidental changes of the decoder or the block layout).
SND_TEST(Decode, AdpcmGolden) {
  const CodecHeader h = enc::MakeAdpcmHeader(2, 48000, 4800, 256);
  auto data = EncodeOrDie(h, MakeSignal(2, 4800, 99));
  FeedResult r = Reference(h, data);
  SND_REQUIRE(!r.error);
  snd::test::AudioBuffer buf(2, 4800, 48000);
  for (std::uint32_t c = 0; c < 2; ++c) std::memcpy(buf.Channel(c), r.out[c].data(), 4800 * sizeof(float));
  snd::test::GoldenTolerance tol;
  tol.maxAbs = 0.0f;
  SND_CHECK_GOLDEN("adpcm_stereo_256", buf, tol);
}

// Acceptance decision 5: PCM_F32 NaN / +-Inf decode as 0, everything else bit-exact.
SND_TEST(Decode, PcmF32NonFiniteBecomesZero) {
  const CodecHeader h = enc::MakePcmHeader(SND_CODEC_PCM_F32, 1, 48000, 6);
  const std::uint32_t bits[6] = {0x7FC00000u, 0xFFC00001u, 0x7F800000u, 0xFF800000u, 0x7F7FFFFFu, 0x80000001u};
  const std::uint32_t want[6] = {0u, 0u, 0u, 0u, 0x7F7FFFFFu, 0x80000001u};
  std::vector<std::uint8_t> data(24);
  for (int i = 0; i < 6; ++i) {
    for (int k = 0; k < 4; ++k) data[static_cast<std::size_t>(i * 4 + k)] = static_cast<std::uint8_t>(bits[i] >> (8 * k));
  }
  FeedResult r = Reference(h, data);
  SND_REQUIRE(!r.error);
  SND_REQUIRE_EQ(Frames(r.out), 6u);
  for (int i = 0; i < 6; ++i) {
    std::uint32_t got;
    std::memcpy(&got, &r.out[0][static_cast<std::size_t>(i)], 4);
    SND_CHECK_EQ(got, want[i]);
  }
}

// Acceptance decision 6: a state belongs to the codec that initialised it.
SND_TEST(Decode, ForeignCodecStateIsRejected) {
  const CodecHeader h = enc::MakePcmHeader(SND_CODEC_PCM_S16, 1, 48000, 100);
  auto data = EncodeOrDie(h, MakeSignal(1, 100));
  snd::ICodec* s16 = snd::codec::BuiltinCodec(SND_CODEC_PCM_S16);
  snd::ICodec* f32 = snd::codec::BuiltinCodec(SND_CODEC_PCM_F32);
  snd::ICodec* adp = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
  StateMem mem(snd::codec::kMinStateBytes > s16->StateBytes(h) ? snd::codec::kMinStateBytes : s16->StateBytes(h));
  OutBuf ob(1, 64);
  s16->Init(mem.get(), h);
  SND_CHECK_EQ(f32->Decode(mem.get(), data.data(), 8, ob.dst(), 64), snd::kDecodeError);
  SND_CHECK_EQ(adp->Decode(mem.get(), nullptr, 0, ob.dst(), 64), snd::kDecodeError);
  // The rightful codec still works after a foreign Decode.
  SND_CHECK_EQ(s16->Decode(mem.get(), data.data(), 8, ob.dst(), 64), 4u);
  // A foreign Seek leaves the state failed for the next Decode.
  adp->Seek(mem.get(), 0);
  SND_CHECK_EQ(s16->Decode(mem.get(), data.data() + 8, 8, ob.dst(), 64), snd::kDecodeError);
  // Until the owner seeks (or Init).
  s16->Seek(mem.get(), 0);
  SND_CHECK_EQ(s16->Decode(mem.get(), data.data(), 8, ob.dst(), 64), 4u);
  // Init by a codec object with another codec's header is an invalid header.
  f32->Init(mem.get(), h);
  SND_CHECK_EQ(f32->Decode(mem.get(), data.data(), 8, ob.dst(), 64), snd::kDecodeError);
  SND_CHECK_EQ(s16->Decode(mem.get(), data.data(), 8, ob.dst(), 64), snd::kDecodeError);
  SND_CHECK(mem.GuardIntact());
}

// Acceptance decisions 1, 2, 4: range and overflow checks of the seek helpers, PCM packet = 1 frame.
SND_TEST(Decode, SeekHelpersRejectOutOfRange) {
  for (std::uint32_t codec : kCodecs) {
    CodecHeader h = HeaderFor(codec, 2, 5000, 64);
    snd::codec::PacketPosition pos{};
    SND_CHECK(snd::codec::PacketForFrame(h, 4999, &pos));
    for (std::uint64_t t : {std::uint64_t{5000}, std::uint64_t{1} << 40, UINT64_MAX}) {
      SND_CHECK(!snd::codec::PacketForFrame(h, t, &pos));
    }
    // A header whose byte offsets overflow 64 bits near the end.
    CodecHeader huge = h;
    huge.total_frames = UINT64_MAX;
    SND_CHECK(!snd::codec::PacketForFrame(huge, UINT64_MAX - 1, &pos));
    SND_CHECK(snd::codec::PacketForFrame(huge, 1000, &pos));
  }
  const std::uint64_t f[] = {0, 1000, 2000};
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 3, 2500, 0, 3000), 2u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 3, 2999, 0, 3000), 2u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 3, 3000, 0, 3000), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 3, UINT64_MAX, 0, 3000), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 3, 0, 0, 0), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 0, 10, 0, 3000), snd::codec::kNoSeekPoint);
  CodecHeader pcm = enc::MakePcmHeader(SND_CODEC_PCM_S16, 2, 48000, 10);
  for (std::uint32_t fpp : {0u, 1u}) {
    for (std::uint32_t ba : {0u, 4u}) {
      pcm.frames_per_packet = fpp;
      pcm.block_align = ba;
      SND_CHECK(snd::codec::ValidateHeader(pcm) == snd::codec::HeaderError::None);
    }
  }
  pcm.block_align = 0;
  pcm.frames_per_packet = 2;
  SND_CHECK(snd::codec::ValidateHeader(pcm) == snd::codec::HeaderError::BadFramesPerPacket);
  pcm.frames_per_packet = 1;
  pcm.block_align = 8;
  SND_CHECK(snd::codec::ValidateHeader(pcm) == snd::codec::HeaderError::BadBlockAlign);
}
