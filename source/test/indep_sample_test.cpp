// Independent tests of the Sample kind (written from the spec, not from the
// implementation): analytic levels / phase / start offsets / loop seams / end
// frame counts per load form, seeks and SetLoop without dropouts, virtual
// advance in 32.32 fixed point over long runs, the PCM cache (LRU, pinning,
// fallback, refusal) and the work done on the audio thread at Start.
// Driven directly through ISourceKind (indep_driver.hpp): RenderBatch gets
// source-rate items, so the output is the source frames themselves.
// Tests named Defect_source_* fail on purpose: they show a defect (reported, not fixed).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

AssetSpec Spec(const char* name, std::uint64_t frames, const char* form, const char* codec, const char* extra = "",
               std::uint32_t channels = 1, double (*f)(std::uint32_t, std::uint64_t) = &Sine1k) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(channels, 48000, frames, f);
  a.loadForm = form;
  a.codec = codec;
  a.extra = extra;
  return a;
}
// Channel-dependent, never zero, exactly representable (Q16) analytic signal.
double Analytic(std::uint32_t c, std::uint64_t i) {
  return 0.3 * std::sin(6.283185307179586 * (440.0 + 110.0 * c) * double(i) / 48000.0 + 0.1 * c) + 0.05 * (c + 1);
}
double Expect(std::uint32_t c, std::uint64_t i) { return Q16(Analytic(c, i)); }

double MaxDiff(const std::vector<float>& got, std::size_t from, const std::vector<float>& want) {
  double m = 0;
  for (std::size_t i = 0; i < want.size(); ++i) {
    const double g = from + i < got.size() ? got[from + i] : 1e9;
    m = std::fmax(m, std::fabs(g - want[i]));
  }
  return m;
}
std::vector<float> Slice(const std::vector<float>& x, std::size_t from, std::size_t n) {
  std::vector<float> v;
  for (std::size_t i = from; i < from + n && i < x.size(); ++i) v.push_back(x[i]);
  return v;
}
void RenderUntil(IndepDriver& d, src::ISourceKind& k, IndepDriver::Voice& v, std::size_t frames, int limit = 4000) {
  for (int i = 0; i < limit && (v.out.empty() || v.out[0].size() < frames) && v.ended == 0u; ++i) d.Block(k, {&v}, 256);
}

// Wraps a built-in codec and counts the frames it decodes (codec hook of the services).
struct CountingCodec final : snd::ICodec {
  snd::ICodec* inner = nullptr;
  std::uint32_t fourcc = 0;
  std::atomic<std::uint64_t> frames{0};
  std::uint32_t StateBytes(const snd::CodecHeader& h) const override { return inner->StateBytes(h); }
  void Init(void* s, const snd::CodecHeader& h) override { inner->Init(s, h); }
  std::uint32_t Decode(void* s, const std::uint8_t* src, std::uint32_t n, float* const* dst, std::uint32_t max) override {
    const std::uint32_t got = inner->Decode(s, src, n, dst, max);
    if (got != SND_DECODE_ERROR) frames.fetch_add(got, std::memory_order_relaxed);
    return got;
  }
  void Seek(void* s, std::uint64_t f) override { inner->Seek(s, f); }
  static snd::ICodec* Find(void* ctx, std::uint32_t fourcc) noexcept {
    CountingCodec* c = static_cast<CountingCodec*>(ctx);
    return fourcc == c->fourcc ? c : nullptr;
  }
};

}  // namespace

// A1, A2, A5, A8: PCM resident (PCMF and PC16, 1/2/8 channels) plays exactly
// the analytic material from any start frame, the channels stay apart, the end
// comes after exactly total - start frames (the last block partial, then one
// empty block with `ended`, so core's resampler look-ahead is played out).
SND_TEST(IndepSample, PcmResidentAnalyticLevelsPhaseAndEnd) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("f1", 10007, "pcm", "PCMF", "", 1, &Analytic));
  as.push_back(Spec("s2", 10007, "pcm", "PC16", "", 2, &Analytic));
  as.push_back(Spec("s8", 5003, "pcm", "PC16", "", 8, &Analytic));
  Built b = Build("iana", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    for (std::uint64_t start : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{4093}}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.channels = a.pcm.channels;
      v.ph.frame = start;
      std::uint32_t priming = 9;
      v.slot = d.Start(d.Sample(), d.Asset(a.name), start, 0, nullptr, &priming);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      SND_CHECK_EQ(priming, 0u);  // resident PCM needs no priming (B1)
      SND_CHECK(d.Ready(d.Sample(), v.slot));
      RenderUntil(d, d.Sample(), v, ~std::size_t{0});
      const std::uint64_t want = a.pcm.frames - start;
      SND_CHECK(v.ended != 0u);
      SND_CHECK_EQ(v.out[0].size(), want);
      SND_CHECK_EQ(v.ph.frame, a.pcm.frames);
      // Every block full except the last one with frames, then exactly one empty block (ended).
      SND_REQUIRE(v.blocks.size() >= 2u);
      SND_CHECK_EQ(v.blocks.back(), 0u);
      SND_CHECK_EQ(v.blocks[v.blocks.size() - 2], static_cast<std::uint32_t>(want % 256u == 0u ? 256u : want % 256u));
      double err = 0;
      for (std::uint32_t c = 0; c < v.channels; ++c) {
        for (std::uint64_t i = 0; i < want && i < v.out[c].size(); ++i) {
          err = std::fmax(err, std::fabs(v.out[c][i] - Expect(c, start + i)));
        }
      }
      SND_CHECK_MSG(err == 0.0, "%s start %llu: max err %g", a.name.c_str(), static_cast<unsigned long long>(start), err);
    }
  }
}

// A4: loop seams of every resident form are sample exact (odd loop points, not
// on packet boundaries), many loops in a row, loopCount counts them.
SND_TEST(IndepSample, LoopSeamsExactInEveryForm) {
  const char* lp = "\"loop\": {\"start\": 1234, \"end\": 7777}";
  std::vector<AssetSpec> as;
  as.push_back(Spec("pf", 12000, "pcm", "PCMF", lp, 1, &Analytic));
  as.push_back(Spec("ra", 12000, "compressed", "ADPC", lp, 1, &Analytic));
  as.push_back(Spec("ca", 12000, "decompressCache", "ADPC", lp, 1, &Analytic));
  as.push_back(Spec("cp", 12000, "decompressCache", "PC16", lp, 1, &Analytic));
  if (snd::codec::OpusAvailable()) {
    as.push_back(Spec("ro", 12000, "compressed", "OPUS", lp, 1, &Analytic));
    as.push_back(Spec("co", 12000, "decompressCache", "OPUS", lp, 1, &Analytic));
  }
  Built b = Build("iloop", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    SND_REQUIRE_EQ(ref.loopStart, 1234u);
    SND_REQUIRE_EQ(ref.loopEnd, 7777u);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset(a.name), 0, src::kStartLoop);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    const std::size_t n = 7777 + 6543 * 7 + 100;
    RenderUntil(d, d.Sample(), v, n);
    const std::vector<float> model = LoopModel(ref.ch[0], 1234, 7777, n);
    const double e = MaxDiff(v.out[0], 0, model);
    SND_CHECK_MSG(e < (a.codec == "OPUS" ? 1e-4 : 1e-7), "%s: loop err %g", a.name.c_str(), e);
    SND_CHECK_EQ(v.ph.loopCount, static_cast<std::uint32_t>((v.out[0].size() - 7777) / 6543 + 1));
    SND_CHECK(v.ended == 0u);
    // A start past the loop end maps into the loop (WrapInto): start 9000 plays 1234 + (9000-7777).
    IndepDriver::Voice w;
    w.slot = d.Start(d.Sample(), d.Asset(a.name), 9000, src::kStartLoop);
    SND_REQUIRE(w.slot != snd::kInvalidSlot);
    RenderUntil(d, d.Sample(), w, 3000);
    const double e2 = MaxDiff(w.out[0], 0, Slice(ref.ch[0], 1234 + (9000 - 7777), 3000));
    SND_CHECK_MSG(e2 < (a.codec == "OPUS" ? 1e-4 : 1e-7), "%s: start past loop end err %g", a.name.c_str(), e2);
  }
}

// A3 + continuity: a seek of a compressed-resident voice (data in bank memory,
// like the start, which is decoded on the audio thread so the first block
// plays) must not leave a block without frames. Today the block that follows
// the Control renders 0 frames (pendingSeek) and the position stands still for
// a block: a 256-frame hole in the middle of the sound.
SND_TEST(IndepSample, Defect_source_SeekOfCompressedResidentLeavesAnEmptyBlock) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("ra", 96000, "compressed", "ADPC"));
  if (snd::codec::OpusAvailable()) as.push_back(Spec("ro", 96000, "compressed", "OPUS"));
  Built b = Build("iseekgap", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset(a.name), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    for (int i = 0; i < 20; ++i) d.Block(d.Sample(), {&v}, 256);
    SND_CHECK_EQ(v.silentBlocks, 0u);
    d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 50001);
    const std::size_t before = v.out[0].size();
    const std::size_t b0 = v.blocks.size();
    for (int i = 0; i < 10; ++i) d.Block(d.Sample(), {&v}, 256);
    std::uint32_t empty = 0;
    for (std::size_t i = b0; i < v.blocks.size(); ++i) empty += v.blocks[i] < 256u ? 1u : 0u;
    SND_CHECK_MSG(empty == 0u, "%s: %u short blocks after a seek (first %u frames)", a.name.c_str(), empty, v.blocks[b0]);
    // The frames after the seek are exact anyway.
    const double e = MaxDiff(v.out[0], before, Slice(ref.ch[0], 50001, 2000));
    SND_CHECK_MSG(e < (a.codec == "OPUS" ? 1e-4 : 1e-6), "%s seek err %g", a.name.c_str(), e);
  }
}

// SetLoop on a playing compressed-resident voice (loop end moved, loop turned
// off to let a loop ring out): the output must go on without a hole. Today
// SetLoop re-seeks the per-voice ring to the current position: one block of 0
// frames, the playhead stands still.
SND_TEST(IndepSample, Defect_source_SetLoopOnCompressedResidentLeavesAnEmptyBlock) {
  Built b = Build("isetloopgap", {Spec("ra", 48000, "compressed", "ADPC", "\"loop\": {\"start\": 4000, \"end\": 30001}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, src::kStartLoop);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 30; ++i) d.Block(d.Sample(), {&v}, 256);
  const std::uint64_t pos = v.ph.frame;  // 7680
  const std::size_t before = v.out[0].size();
  const std::size_t b0 = v.blocks.size();
  d.Control(d.Sample(), v.slot, src::ControlOp::SetLoop, 0, 0);  // loop off: run out to the end
  for (int i = 0; i < 20; ++i) d.Block(d.Sample(), {&v}, 256);
  std::uint32_t empty = 0;
  for (std::size_t i = b0; i < v.blocks.size(); ++i) empty += v.blocks[i] < 256u ? 1u : 0u;
  SND_CHECK_MSG(empty == 0u, "%u short blocks after SetLoop", empty);
  SND_CHECK(MaxDiff(v.out[0], before, Slice(ref.ch[0], pos, 4000)) < 1e-6);
}

// Core's order (sys_event.cpp: Devirtualize is sent by RunEventLogic one block
// after SelectVoices gave the voice a real slot, while Render already renders
// it in that block): a per-voice-ring voice is rendered once while the source
// still has it virtual. It must render either nothing or the material at its
// playhead. Today RenderRing ignores `virt` and plays the stale ring (the
// material from where it was virtualised).
SND_TEST(IndepSample, Defect_source_RingRendersStaleDataBeforeDevirtualize) {
  Built b = Build("istale", {Spec("ra", 96000, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  for (int i = 0; i < 20; ++i) d.Block(d.Sample(), {&v}, 256);
  d.Control(d.Sample(), v.slot, src::ControlOp::Virtualize);
  for (int i = 0; i < 40; ++i) {
    src::AdvanceItem it{0, v.slot, 256, 1.0f, &v.ph};
    snd::VoiceRenderStatus st{};
    d.Sample().AdvanceVirtual(&it, 1, &st);
    d.Block(d.Sample(), {}, 256);
  }
  const std::uint64_t pos = v.ph.frame;
  SND_CHECK_EQ(pos, 20u * 256u + 40u * 256u);
  const std::size_t before = v.out[0].size();
  d.Block(d.Sample(), {&v}, 256);  // real again; Devirtualize comes in the next block
  const std::size_t got = v.out[0].size() - before;
  if (got != 0u) {
    const double e = MaxDiff(v.out[0], before, Slice(ref.ch[0], pos, got));
    SND_CHECK_MSG(e < 1e-6, "rendered %zu frames before Devirtualize that are not the material at the playhead %llu (err %g)",
                  got, static_cast<unsigned long long>(pos), e);
  }
  // After Devirtualize (frame = the playhead core then holds) the voice is exact again.
  const std::uint64_t pos2 = v.ph.frame;
  d.Control(d.Sample(), v.slot, src::ControlOp::Devirtualize, pos2);
  const std::size_t b2 = v.out[0].size();
  RenderUntil(d, d.Sample(), v, b2 + 3000);
  SND_CHECK(MaxDiff(v.out[0], b2, Slice(ref.ch[0], pos2, 3000)) < 1e-6);
}

// A7: virtual advance in 32.32 fixed point. With a float pitch the per-block
// advance outFrames * pitch is exact in 32.32 (pitch has a 24-bit mantissa), so
// after millions of blocks the position must be exact (no drift), wrapping over
// the loop modulo its length and counting loops; a non-looping voice ends in the
// block that crosses the end.
SND_TEST(IndepSample, AdvanceVirtualExactOverLongRunsAndLoops) {
  Built b = Build("iadv", {Spec("lp", 100000, "pcm", "PC16", "\"loop\": {\"start\": 777, \"end\": 54321}"),
                           Spec("nl", 100000, "pcm", "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  for (float pitch : {44100.0f / 48000.0f, 1.0f, 1.7320508f, 0.013f}) {
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset("lp"), 0, src::kStartLoop | src::kStartVirtual);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    const std::uint64_t blocks = 2000000;  // ~3 hours at 256 frames / 48 kHz
    const std::uint32_t outFrames = 256;
    src::AdvanceItem it{0, v.slot, outFrames, pitch, &v.ph};
    snd::VoiceRenderStatus st{};
    for (std::uint64_t i = 0; i < blocks; ++i) {
      d.Sample().AdvanceVirtual(&it, 1, &st);
      if (st.ended != 0u) break;
    }
    SND_CHECK(st.ended == 0u);
    // Exact model: per block advance A = outFrames * pitch in 2^-32 units (exact), sum, wrap.
    // outFrames * pitch * 2^32 is exact in a double (24-bit mantissa x 2^8 x 2^32) and the sum fits 64 bits.
    const std::uint64_t per = static_cast<std::uint64_t>(double(outFrames) * double(pitch) * 4294967296.0);
    const std::uint64_t total = per * blocks;
    const std::uint64_t whole = static_cast<std::uint64_t>(total >> 32);
    const std::uint32_t frac = static_cast<std::uint32_t>(total);
    const std::uint64_t le = 54321, lb = 777, len = le - lb;
    std::uint64_t pos = whole;
    std::uint64_t loops = 0;
    if (pos >= le) {
      loops = 1 + (pos - le) / len;
      pos = lb + (pos - le) % len;
    }
    SND_CHECK_MSG(v.ph.frame == pos && v.ph.fraction == frac,
                  "pitch %.9g: got %llu + %u/2^32, want %llu + %u/2^32", double(pitch),
                  static_cast<unsigned long long>(v.ph.frame), v.ph.fraction, static_cast<unsigned long long>(pos), frac);
    SND_CHECK_EQ(static_cast<std::uint64_t>(v.ph.loopCount), loops);
    // Devirtualise there: the material at that position, exactly.
    d.Control(d.Sample(), v.slot, src::ControlOp::Devirtualize, v.ph.frame);
    const std::uint64_t at = v.ph.frame;
    d.Block(d.Sample(), {&v}, 256);
    const Decoded ref = Reference(b, "lp");
    SND_REQUIRE(ref.ok);
    std::vector<float> want;
    for (std::uint64_t i = 0, p = at; i < 256; ++i) {
      want.push_back(ref.ch[0][static_cast<std::size_t>(p)]);
      if (++p == le) p = lb;
    }
    SND_CHECK(MaxDiff(v.out[0], 0, want) < 1e-7);
    d.Release(d.Sample(), v.slot);
  }
  // Non-looping: ends in the block that reaches total_frames, not before.
  IndepDriver::Voice n;
  n.slot = d.Start(d.Sample(), d.Asset("nl"), 0, src::kStartVirtual);
  src::AdvanceItem it{0, n.slot, 256, 1.5f, &n.ph};
  snd::VoiceRenderStatus st{};
  std::uint64_t blk = 0;
  while (st.ended == 0u && blk < 10000) {
    d.Sample().AdvanceVirtual(&it, 1, &st);
    ++blk;
  }
  SND_CHECK_EQ(blk, (100000u + 383u) / 384u);  // 384 frames per block
}

// B3: PCM cache LRU and pinning. 3 pages, assets of one page each.
SND_TEST(IndepSample, CacheLruPinningFallbackAndRefusal) {
  std::vector<AssetSpec> as;
  for (const char* n : {"c1", "c2", "c3", "c4"}) as.push_back(Spec(n, 16000, "decompressCache", "ADPC", "", 1, &Analytic));
  as.push_back(Spec("rad", 50000, "compressed", "ADPC"));
  Built b = Build("ilru", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded r4 = Reference(b, "c4");
  SND_REQUIRE(r4.ok);
  IndepDriver::Opt o;
  o.pcmCacheBytes = 3u * (64u << 10);
  o.maxDecoders = 4;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  auto playOnce = [&](const char* name) {
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset(name), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    RenderUntil(d, d.Sample(), v, ~std::size_t{0});
    SND_CHECK(v.ended != 0u);
    SND_CHECK_EQ(v.out[0].size(), 16000u);
    d.Release(d.Sample(), v.slot);
    for (int i = 0; i < 3; ++i) d.Block(d.Sample(), {}, 256);
  };
  playOnce("c1");
  playOnce("c2");
  playOnce("c3");
  auto s = d.runtime.Stats();
  SND_CHECK_EQ(s.cacheMisses, 3u);
  SND_CHECK_EQ(s.cacheHits, 0u);
  SND_CHECK_EQ(s.cachePagesUsed, 3u);
  playOnce("c1");  // hit, c1 becomes the most recent
  s = d.runtime.Stats();
  SND_CHECK_EQ(s.cacheHits, 1u);
  playOnce("c4");  // miss, evicts the least recently used: c2
  s = d.runtime.Stats();
  SND_CHECK_EQ(s.cacheMisses, 4u);
  SND_CHECK_EQ(s.cacheEvictions, 1u);
  playOnce("c3");  // still cached
  playOnce("c1");
  s = d.runtime.Stats();
  SND_CHECK_EQ(s.cacheHits, 3u);
  playOnce("c2");  // was evicted
  s = d.runtime.Stats();
  SND_CHECK_EQ(s.cacheMisses, 5u);
  SND_CHECK_EQ(s.decodersInUse, 0u);
  // All pages pinned by playing voices: the next cache asset plays through a per-voice ring, exactly.
  IndepDriver::Voice p1, p2, p3, f;
  p1.slot = d.Start(d.Sample(), d.Asset("c1"), 0, 0);
  p2.slot = d.Start(d.Sample(), d.Asset("c2"), 0, 0);
  p3.slot = d.Start(d.Sample(), d.Asset("c3"), 0, 0);
  SND_REQUIRE(p1.slot != snd::kInvalidSlot && p2.slot != snd::kInvalidSlot && p3.slot != snd::kInvalidSlot);
  d.Block(d.Sample(), {&p1, &p2, &p3}, 256);
  const std::uint64_t fb0 = d.runtime.Stats().cacheFallbacks;
  f.slot = d.Start(d.Sample(), d.Asset("c4"), 0, 0);
  SND_REQUIRE(f.slot != snd::kInvalidSlot);
  SND_CHECK_EQ(d.runtime.Stats().cacheFallbacks, fb0 + 1u);
  for (int i = 0; i < 70 && f.ended == 0u; ++i) d.Block(d.Sample(), {&p1, &p2, &p3, &f}, 256);
  SND_CHECK(f.ended != 0u);
  SND_CHECK_EQ(f.out[0].size(), 16000u);
  SND_CHECK(MaxDiff(f.out[0], 0, r4.ch[0]) < 1e-6);
  SND_CHECK(MaxDiff(p2.out[0], 0, Slice(Reference(b, "c2").ch[0], 0, p2.out[0].size())) < 1e-6);
  // Pages pinned AND no decoder: refused with a reason.
  IndepDriver::Voice q1, q2, q3, q4, x;
  q1.slot = d.Start(d.Sample(), d.Asset("c1"), 0, 0);
  q2.slot = d.Start(d.Sample(), d.Asset("c2"), 0, 0);
  q3.slot = d.Start(d.Sample(), d.Asset("c3"), 0, 0);
  std::vector<IndepDriver::Voice> rings(4);
  std::uint32_t held = 0;
  for (auto& r : rings) {
    r.slot = d.Start(d.Sample(), d.Asset("rad"), 0, 0);
    held += r.slot != snd::kInvalidSlot ? 1u : 0u;
  }
  SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 4u);
  std::uint32_t detail = 0;
  x.slot = d.Start(d.Sample(), d.Asset("c4"), 0, 0, &detail);
  SND_CHECK_EQ(x.slot, snd::kInvalidSlot);
  SND_CHECK_MSG(detail == SND_END_DETAIL_PCM_CACHE_FULL || detail == SND_END_DETAIL_DECODER_POOL_FULL, "detail %u", detail);
  std::printf("  cache full + no decoder -> detail %u (held rings %u)\n", detail, held);
  for (snd::SlotIndex sl : {p1.slot, p2.slot, p3.slot, f.slot, q1.slot, q2.slot, q3.slot}) d.Release(d.Sample(), sl);
  for (auto& r : rings) {
    if (r.slot != snd::kInvalidSlot) d.Release(d.Sample(), r.slot);
  }
  for (int i = 0; i < 4; ++i) d.Block(d.Sample(), {}, 256);
  SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 0u);
  SND_CHECK_EQ(d.runtime.Stats().sampleInstances, 0u);
}

// Design 2.4 / principle 1: work on the audio thread at Start is bounded. A
// start offset deep into a DecompressCache asset must not make Start decode the
// whole asset up to the offset on the audio thread. Today FillHead decodes
// [0, start + ready) inline (here ~450 000 frames for ADPCM; ~9 s of Opus).
SND_TEST(IndepSample, Defect_source_CacheStartDecodesUpToTheOffsetOnTheAudioThread) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("cad", 480000, "decompressCache", "ADPC"));
  as.push_back(Spec("rad", 480000, "compressed", "ADPC"));
  Built b = Build("iinline", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const char* name : {"rad", "cad"}) {
    CountingCodec cc;
    cc.fourcc = SND_CODEC_ADPCM;
    cc.inner = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
    SND_REQUIRE(cc.inner != nullptr);
    IndepDriver::Opt o;
    o.findCodec = &CountingCodec::Find;
    o.codecCtx = &cc;
    o.manualJobs = true;
    o.pcmCacheBytes = 4u << 20;
    IndepDriver d(b, o);
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    const auto t0 = std::chrono::steady_clock::now();
    v.slot = d.Start(d.Sample(), d.Asset(name), 450000, 0);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    const std::uint64_t inl = cc.frames.load();
    std::printf("  %s: Start decoded %llu frames on the audio thread (%.2f ms)\n", name,
                static_cast<unsigned long long>(inl), ms);
    SND_CHECK_MSG(inl < 20000u, "%s: Start decoded %llu frames inline", name, static_cast<unsigned long long>(inl));
  }
}
