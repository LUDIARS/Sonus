// Encoder, header and seek-table helpers (buildtool side), and the A0-2
// measurement of StateBytes against Config.memory.decoderSlotBytes.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "codec_test_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/config.h"
#include "snd/test/test.hpp"

using namespace codec_test;
namespace enc = snd::codec::encode;
using enc::EncodeStatus;

SND_TEST(Encode, HeadersAreValid) {
  for (std::uint32_t ch = 1; ch <= SND_MAX_CHANNELS; ++ch) {
    const snd::CodecHeader f = enc::MakePcmHeader(SND_CODEC_PCM_F32, ch, 48000, 100);
    const snd::CodecHeader s = enc::MakePcmHeader(SND_CODEC_PCM_S16, ch, 44100, 100);
    const snd::CodecHeader a = enc::MakeAdpcmHeader(ch, 32000, 100);
    SND_CHECK(snd::codec::ValidateHeader(f) == snd::codec::HeaderError::None);
    SND_CHECK(snd::codec::ValidateHeader(s) == snd::codec::HeaderError::None);
    SND_CHECK(snd::codec::ValidateHeader(a) == snd::codec::HeaderError::None);
    SND_CHECK_EQ(f.block_align, 4u * ch);
    SND_CHECK_EQ(s.block_align, 2u * ch);
    SND_CHECK_EQ(a.block_align, 512u * ch);
    SND_CHECK_EQ(a.frames_per_packet, 1017u);
    SND_CHECK_EQ(a.pre_skip, 0u);
    SND_CHECK_EQ(a.pre_roll, 0u);
    SND_CHECK_EQ(enc::EncodedBytes(f), 400u * ch);
    SND_CHECK_EQ(enc::EncodedBytes(a), 512u * ch);  // one padded block
  }
  SND_CHECK_EQ(snd::codec::AdpcmFramesPerBlock(1, 36), 65u);   // classic 36-byte mono block
  SND_CHECK_EQ(snd::codec::AdpcmFramesPerBlock(2, 1024), 1017u);
  SND_CHECK_EQ(snd::codec::AdpcmFramesPerBlock(2, 2048), 2041u);
  SND_CHECK_EQ(snd::codec::AdpcmFramesPerBlock(2, 2047), 0u);
  snd::CodecHeader a = enc::MakeAdpcmHeader(2, 48000, 1017 * 3);
  SND_CHECK_EQ(enc::EncodedBytes(a), 3u * 1024u);
  a.total_frames = 1017 * 3 + 1;
  SND_CHECK_EQ(enc::EncodedBytes(a), 4u * 1024u);
  a.total_frames = 0;
  SND_CHECK_EQ(enc::EncodedBytes(a), 0u);
}

SND_TEST(Encode, RejectsBadInput) {
  Planar src = MakeSignal(2, 100);
  auto ptrs = Ptrs(src);
  std::vector<std::uint8_t> out(4096);
  std::uint64_t w = 99;
  snd::CodecHeader h = enc::MakePcmHeader(SND_CODEC_PCM_S16, 2, 48000, 100);
  SND_CHECK(enc::Encode(h, ptrs.data(), 99, out.data(), out.size(), &w) == EncodeStatus::BadArguments);
  SND_CHECK_EQ(w, 0u);
  SND_CHECK(enc::Encode(h, ptrs.data(), 100, out.data(), 399, &w) == EncodeStatus::BufferTooSmall);
  SND_CHECK(enc::Encode(h, nullptr, 100, out.data(), out.size(), &w) == EncodeStatus::BadArguments);
  src[1][50] = std::numeric_limits<float>::quiet_NaN();
  SND_CHECK(enc::Encode(h, ptrs.data(), 100, out.data(), out.size(), &w) == EncodeStatus::NonFiniteInput);
  src[1][50] = std::numeric_limits<float>::infinity();
  SND_CHECK(enc::Encode(h, ptrs.data(), 100, out.data(), out.size(), &w) == EncodeStatus::NonFiniteInput);
  src[1][50] = 0.0f;
  SND_CHECK(enc::Encode(h, ptrs.data(), 100, out.data(), out.size(), &w) == EncodeStatus::Ok);
  SND_CHECK_EQ(w, 400u);
  snd::CodecHeader opus = h;
  opus.codec = SND_CODEC_OPUS;
  SND_CHECK(enc::Encode(opus, ptrs.data(), 100, out.data(), out.size(), &w) == EncodeStatus::BadHeader);
  snd::CodecHeader empty = enc::MakeAdpcmHeader(1, 48000, 0);
  SND_CHECK(enc::Encode(empty, nullptr, 0, nullptr, 0, &w) == EncodeStatus::Ok);
  SND_CHECK_EQ(w, 0u);
  SND_CHECK(enc::EncodeStatusName(EncodeStatus::NonFiniteInput)[0] != '?');
}

SND_TEST(Encode, AdpcmLayoutIsWavIma) {
  // Block header = first sample as s16 + step index; the decoder returns it exactly.
  Planar src(1, std::vector<float>(65, 0.0f));
  src[0][0] = 0.25f;
  const snd::CodecHeader h = enc::MakeAdpcmHeader(1, 48000, 65, 36);
  auto data = EncodeOrDie(h, src);
  SND_REQUIRE_EQ(data.size(), 36u);
  SND_CHECK_EQ(data[0] | (data[1] << 8), 8192);
  SND_CHECK(data[2] <= 88u);
  SND_CHECK_EQ(data[3], 0u);
}

SND_TEST(Encode, SeekTables) {
  for (std::uint32_t codec : {SND_CODEC_PCM_F32, SND_CODEC_PCM_S16, SND_CODEC_ADPCM}) {
    snd::CodecHeader h = codec == SND_CODEC_ADPCM ? enc::MakeAdpcmHeader(2, 48000, 100000)
                                                  : enc::MakePcmHeader(codec, 2, 48000, 100000);
    for (std::uint32_t pre : {0u, 2500u}) {
      h.pre_skip = pre;
      for (std::uint64_t interval : {0ull, 1ull, 4800ull, 1000000ull}) {
        if (interval <= 1 && codec != SND_CODEC_ADPCM) continue;  // one entry per frame: skip the big ones
        const std::uint32_t n = enc::BuildSeekTable(h, interval, nullptr, nullptr, 0);
        SND_REQUIRE(n >= 1u);
        std::vector<std::uint64_t> f(n), o(n);
        SND_REQUIRE_EQ(enc::BuildSeekTable(h, interval, f.data(), o.data(), n), n);
        SND_CHECK_EQ(f[0], 0u);
        SND_CHECK_EQ(o[0], 0u);
        for (std::uint32_t i = 1; i < n; ++i) {
          SND_CHECK(f[i] > f[i - 1]);
          SND_CHECK(o[i] > o[i - 1]);
          SND_CHECK(f[i] < h.total_frames);
          if (interval > 1) SND_CHECK(f[i] - f[i - 1] >= interval);
          // Every entry is a packet boundary consistent with PacketForFrame.
          snd::codec::PacketPosition p{};
          SND_REQUIRE(snd::codec::PacketForFrame(h, f[i], &p));
          SND_CHECK_EQ(p.frame, f[i]);
          SND_CHECK_EQ(p.byteOffset, o[i]);
        }
        if (interval == 4800) {
          // Gaps are no larger than interval + one packet.
          for (std::uint32_t i = 1; i < n; ++i) SND_CHECK(f[i] - f[i - 1] < interval + snd::codec::PacketFrames(h));
        }
        // Writing into a smaller array is bounded.
        std::vector<std::uint64_t> f1(1), o1(1);
        SND_CHECK_EQ(enc::BuildSeekTable(h, interval, f1.data(), o1.data(), 1), n);
      }
    }
  }
  snd::CodecHeader bad{};
  SND_CHECK_EQ(enc::BuildSeekTable(bad, 0, nullptr, nullptr, 0), 0u);
}

SND_TEST(Encode, FindSeekPoint) {
  const std::uint64_t f[] = {0, 1000, 2000, 3000};
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 0, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 999, 0), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 1000, 0), 1u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 1000, 1), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 2500, 400), 2u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 99999, 0), 3u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 4, 10, 100), 0u);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(f, 0, 10, 0), snd::codec::kNoSeekPoint);
  SND_CHECK_EQ(snd::codec::FindSeekPoint(nullptr, 4, 10, 0), snd::codec::kNoSeekPoint);
}

// A0-2: StateBytes of the built-in codecs vs Config.memory.decoderSlotBytes.
// The state keeps undecoded input in compressed form, so it does not depend on
// max_frames; the test checks that by decoding with several max_frames inside
// a slot of exactly StateBytes (guard bytes after it).
SND_TEST(Encode, StateBytesFitDefaultDecoderSlot) {
  snd_config cfg;
  snd_config_init(&cfg);
  const std::uint32_t slot = cfg.memory.decoder_slot_bytes;
  SND_CHECK_EQ(slot, 32768u);
  snd::test::Note("StateBytes (bytes) vs decoderSlotBytes %u:", slot);
  snd::test::Note("  codec      ch  block_align  StateBytes  hold capacity");
  for (std::uint32_t ch : {1u, 2u, 6u, 8u}) {
    struct Row {
      const char* name;
      snd::CodecHeader h;
    };
    const Row rows[] = {
        {"PCM_F32", enc::MakePcmHeader(SND_CODEC_PCM_F32, ch, 48000, 48000)},
        {"PCM_S16", enc::MakePcmHeader(SND_CODEC_PCM_S16, ch, 48000, 48000)},
        {"ADPCM256", enc::MakeAdpcmHeader(ch, 48000, 48000, 256)},
        {"ADPCM512", enc::MakeAdpcmHeader(ch, 48000, 48000, 512)},
        {"ADPCM1024", enc::MakeAdpcmHeader(ch, 48000, 48000, 1024)},
        {"ADPCM2048", enc::MakeAdpcmHeader(ch, 48000, 48000, 2048)},
    };
    for (const Row& r : rows) {
      snd::ICodec* c = snd::codec::BuiltinCodec(r.h.codec);
      const std::uint32_t sb = c->StateBytes(r.h);
      snd::test::Note("  %-9s  %2u  %11u  %10u  %13u%s", r.name, ch, r.h.block_align, sb,
                      snd::codec::HoldCapacityBytes(r.h), sb > slot ? "  (exceeds slot)" : "");
      SND_CHECK(sb % 64u == 0u);
      const bool defaultBlock = r.h.codec != SND_CODEC_ADPCM || r.h.block_align <= 1024u * ch;
      if (defaultBlock) SND_CHECK_MSG(sb <= slot, "%s ch %u: %u > %u", r.name, ch, sb, slot);
      // Independent of max_frames: decode inside exactly sb bytes with several max_frames.
      auto data = EncodeOrDie(r.h, MakeSignal(ch, 48000));
      for (std::uint32_t maxFrames : {1u, 256u, 4096u, 65536u}) {
        if (maxFrames == 1u && ch > 2u) continue;
        FeedResult fr = DecodeAll(r.h, data.data(), data.size(), {8192}, maxFrames);
        SND_CHECK(!fr.error && fr.guardsOk);
        SND_CHECK_EQ(Frames(fr.out), 48000u);
      }
    }
  }
}
