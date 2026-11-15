// Independent fuzz test of the built-in decoders (fixed seed). Random headers
// (mostly valid), random or mutated encoded bytes, random sequences of Decode
// (random sizes, random max_frames incl. 0, drains), Seek (boundaries, non
// boundaries, 0, huge) and Init. Run under ASan/UBSan for out-of-bounds and UB.
// Checked invariants: nothing written outside the state (fences on both sides)
// or past dst[c][max_frames]; never more than max_frames; S16/ADPCM output is
// finite and within [-1, 1); an error is sticky until Seek/Init; an invalid
// header fails every Decode; output after Init/Seek(f) never exceeds
// total_frames - f.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "codec_test_util.hpp"
#include "indep_util.hpp"
#include "snd/codec/codec.hpp"
#include "snd/codec/encode.hpp"
#include "snd/test/test.hpp"

using namespace indep;
namespace enc = snd::codec::encode;

namespace {

CodecHeader RandomHeader(Rng& rng) {
  CodecHeader h{};
  h.codec = rng.Below(20) == 0 ? rng.Next() : kCodecs[rng.Below(3)];
  if (rng.Below(30) == 0) h.codec = SND_CODEC_OPUS;
  h.channels = rng.Below(15) == 0 ? rng.Next() % 12 : 1 + rng.Below(8);
  h.sample_rate = rng.Below(20) == 0 ? 0 : 48000;
  switch (rng.Below(6)) {
    case 0: h.total_frames = 0; break;
    case 1: h.total_frames = UINT64_MAX - rng.Below(4); break;
    default: h.total_frames = rng.Below(6000); break;
  }
  h.pre_skip = rng.Below(4) == 0 ? rng.Below(3000) : 0;
  if (rng.Below(40) == 0) h.pre_skip = UINT32_MAX - rng.Below(3);
  h.pre_roll = rng.Next();
  if (h.codec == SND_CODEC_ADPCM) {
    h.block_align = rng.Below(8) == 0 ? rng.Next() % 70000 : h.channels * 4u * (2u + rng.Below(40));
    h.frames_per_packet = rng.Below(6) == 0 ? rng.Next() % 400 : 0;
  } else {
    const std::uint32_t fb = h.channels * (h.codec == SND_CODEC_PCM_F32 ? 4u : 2u);
    h.block_align = rng.Below(6) == 0 ? rng.Next() % 100 : fb * rng.Below(3);
    h.frames_per_packet = rng.Below(6) == 0 ? rng.Next() % 5 : 0;
  }
  return h;
}

}  // namespace

SND_TEST(IndepFuzz, DecodersSurviveRandomUse) {
  Rng rng(0x5EED2026u);
  const int kIters = 30000;
  std::uint64_t errors = 0, framesOut = 0, valid = 0;
  for (int iter = 0; iter < kIters; ++iter) {
    const CodecHeader h = RandomHeader(rng);
    const bool headerOk = snd::codec::ValidateHeader(h) == snd::codec::HeaderError::None;
    valid += headerOk;
    // Bytes: a real encoding (then mutated) or pure noise.
    std::vector<std::uint8_t> bytes;
    if (headerOk && h.total_frames < 6000 && h.pre_skip < 4000 && rng.Below(2) == 0) {
      const std::uint64_t all = h.pre_skip + h.total_frames;
      bytes = codec_test::EncodeOrDie(h, codec_test::MakeSignal(h.channels, all, static_cast<std::uint32_t>(iter)));
      const std::uint32_t flips = rng.Below(4);
      for (std::uint32_t i = 0; i < flips && !bytes.empty(); ++i) bytes[rng.Below(static_cast<std::uint32_t>(bytes.size()))] ^= static_cast<std::uint8_t>(1u << rng.Below(8));
      if (rng.Below(3) == 0 && !bytes.empty()) bytes.resize(rng.Below(static_cast<std::uint32_t>(bytes.size())));
    } else {
      bytes.resize(rng.Below(3000));
      for (auto& b : bytes) b = static_cast<std::uint8_t>(rng.Next());
      if (h.codec == SND_CODEC_ADPCM && rng.Below(2)) {
        for (std::size_t i = 2; i < bytes.size(); i += 4) bytes[i] %= 89;
      }
    }
    // Pick the object: usually the header's codec, sometimes another built-in.
    snd::ICodec* c = snd::codec::BuiltinCodec(rng.Below(8) == 0 ? kCodecs[rng.Below(3)] : h.codec);
    if (c == nullptr) c = snd::codec::BuiltinCodec(kCodecs[rng.Below(3)]);
    const bool objectMatches = c == snd::codec::BuiltinCodec(h.codec);
    const std::uint32_t sb = c->StateBytes(h);
    if (!headerOk || !objectMatches) SND_CHECK_EQ(sb, snd::codec::kMinStateBytes);
    Fenced mem(sb);
    c->Init(mem.get(), h);
    Driver d(c, mem.get(), 8, 400);
    const bool pcmF32 = h.codec == SND_CODEC_PCM_F32 && objectMatches;
    const bool usable = headerOk && objectMatches;
    std::uint64_t limit = h.total_frames, since = 0;
    bool failed = false;
    std::size_t off = 0;
    const int ops = 1 + static_cast<int>(rng.Below(40));
    for (int op = 0; op < ops; ++op) {
      const std::uint32_t kind = rng.Below(16);
      if (kind == 0) {
        std::uint64_t f;
        switch (rng.Below(4)) {
          case 0: f = 0; break;
          case 1: f = UINT64_MAX - rng.Below(3); break;
          case 2: {
            const std::uint32_t pf = snd::codec::PacketFrames(h);
            f = pf ? static_cast<std::uint64_t>(pf) * rng.Below(20) : rng.Below(100);
            f = f > h.pre_skip ? f - h.pre_skip : 0;
            break;
          }
          default: f = rng.Below(10000); break;
        }
        c->Seek(mem.get(), f);
        limit = f == 0 ? h.total_frames : (h.total_frames > f ? h.total_frames - f : 0);
        since = 0;
        failed = false;
        off = rng.Below(static_cast<std::uint32_t>(bytes.size()) + 1);
        continue;
      }
      if (kind == 1) {
        c->Init(mem.get(), h);
        limit = h.total_frames;
        since = 0;
        failed = false;
        off = 0;
        continue;
      }
      const std::uint32_t max = rng.Below(10) == 0 ? 0u : 1u + rng.Below(400);
      std::uint32_t n = 0;
      const std::uint8_t* src = nullptr;
      if (kind > 4 && off < bytes.size()) {
        n = std::min<std::uint32_t>(1u + rng.Below(rng.Below(4) == 0 ? 20000u : 900u),
                                    static_cast<std::uint32_t>(bytes.size() - off));
        src = bytes.data() + off;
        off += n;
      }
      const std::uint64_t before = d.Frames();
      const std::uint32_t r = d.Call(src, n, max);
      if (r == snd::kDecodeError) {
        ++errors;
        failed = true;
        continue;
      }
      SND_CHECK_MSG(usable, "iter %d: decode succeeded on an invalid header / foreign object", iter);
      SND_CHECK_MSG(!failed, "iter %d: error was not sticky", iter);
      since += d.Frames() - before;
      framesOut += d.Frames() - before;
      SND_CHECK_MSG(since <= limit, "iter %d: %llu frames after reset, limit %llu", iter,
                    static_cast<unsigned long long>(since), static_cast<unsigned long long>(limit));
      if (!pcmF32) {
        for (std::uint32_t ch = 0; ch < h.channels && ch < 8; ++ch) {
          const float* p = d.out.Channel(ch);
          for (std::uint32_t i = 0; i < r; ++i) {
            if (!(std::isfinite(p[i]) && p[i] >= -1.0f && p[i] < 1.0f)) {
              SND_CHECK_MSG(false, "iter %d: sample %g out of range", iter, static_cast<double>(p[i]));
              break;
            }
          }
        }
      }
      // Bound memory of the collected output.
      if (d.Frames() > 100000) d.got.assign(8, {});
    }
    if (!(mem.Intact() && d.tailOk && !d.overMax)) {
      SND_CHECK_MSG(false, "iter %d codec %08x ch %u block %u: fence %d tail %d overMax %d", iter, h.codec,
                    h.channels, h.block_align, mem.Intact(), d.tailOk, d.overMax);
      break;
    }
  }
  snd::test::Note("fuzz: %d iterations, %llu valid headers, %llu errors, %llu frames", kIters,
                  static_cast<unsigned long long>(valid), static_cast<unsigned long long>(errors),
                  static_cast<unsigned long long>(framesOut));
}
