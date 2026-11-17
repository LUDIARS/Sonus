// Independent re-check of the second round of source fixes (4be2dd2 ..
// 8d38881): Seek / SetLoop composition in every form, the cross-fade of a
// seek that waited for a running decode job (shape and length), the claim of
// a posted job racing the job's own start (TSan), the per-block decode budget
// shared by parallel chunks, and a shortened stream loop whose start lies
// outside the preload. Defect_source_* tests fail on purpose.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

AssetSpec Spec(const char* name, std::uint64_t frames, const char* form, const char* codec, const char* extra = "") {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, &Ramp);  // never zero: ratios are meaningful
  a.loadForm = form;
  a.codec = codec;
  a.extra = extra;
  return a;
}
// n frames from p under loop [lb, le); *end = position after them.
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::size_t n,
                         std::uint64_t* end = nullptr) {
  std::vector<float> v;
  while (v.size() < n && p < x.size()) {
    v.push_back(x[static_cast<std::size_t>(p)]);
    if (++p == le && le != 0u) p = lb;
  }
  if (end != nullptr) *end = p;
  return v;
}
double MaxDiff(const std::vector<float>& got, std::size_t from, const std::vector<float>& want) {
  double m = 0;
  for (std::size_t i = 0; i < want.size(); ++i) {
    const double g = from + i < got.size() ? got[from + i] : 1e9;
    m = std::fmax(m, std::fabs(g - want[i]));
  }
  return m;
}
std::uint64_t WrapInto(std::uint64_t lb, std::uint64_t le, std::uint64_t p) { return p < le ? p : lb + (p - le) % (le - lb); }

// Gate in a codec: when armed, Decode waits until released (a running job that takes long).
struct GatedCodec final : snd::ICodec {
  snd::ICodec* inner = snd::codec::BuiltinCodec(SND_CODEC_ADPCM);
  std::atomic<int> armed{0}, entered{0}, open{0};
  std::uint32_t StateBytes(const snd::CodecHeader& h) const override { return inner->StateBytes(h); }
  void Init(void* s, const snd::CodecHeader& h) override { inner->Init(s, h); }
  std::uint32_t Decode(void* s, const std::uint8_t* src, std::uint32_t n, float* const* dst, std::uint32_t max) override {
    if (armed.load() != 0 && open.load() == 0) {
      entered.store(1);
      while (open.load() == 0) std::this_thread::yield();
    }
    return inner->Decode(s, src, n, dst, max);
  }
  void Seek(void* s, std::uint64_t f) override { inner->Seek(s, f); }
  static snd::ICodec* Find(void* ctx, std::uint32_t fourcc) noexcept {
    return fourcc == SND_CODEC_ADPCM ? static_cast<GatedCodec*>(ctx) : nullptr;
  }
};

// One RenderBatch of `vs` on the calling thread (source-rate items); appends the output.
void RenderOn(src::ISourceKind& k, std::vector<IndepDriver::Voice*> vs, std::vector<std::uint32_t>* frames) {
  std::vector<std::vector<float>> buf(vs.size(), std::vector<float>(256, 0.0f));
  std::vector<float*> ptr(vs.size());
  std::vector<src::RenderItem> items(vs.size());
  for (std::size_t i = 0; i < vs.size(); ++i) {
    ptr[i] = buf[i].data();
    items[i].sourceSlot = vs[i]->slot;
    items[i].realSlot = static_cast<snd::SlotIndex>(i);
    items[i].framesRequested = 256;
    items[i].channels = 1;
    items[i].pitch = snd::Ramp{1.0f, 1.0f};
    items[i].playhead = &vs[i]->ph;
    items[i].out = &ptr[i];
  }
  std::vector<snd::VoiceRenderStatus> st(vs.size());
  std::vector<std::uint8_t> scratch(1024);
  src::Scratch sc{scratch.data(), scratch.size(), 0};
  src::RenderBatch b{};
  b.kind = k.Kind();
  b.count = static_cast<std::uint32_t>(items.size());
  b.items = items.data();
  b.block = src::BlockInfo{0, 0, 256, 48000};
  b.scratch = &sc;
  k.RenderBatch(b, st.data());
  for (std::size_t i = 0; i < vs.size(); ++i) {
    vs[i]->out.resize(1);
    vs[i]->out[0].insert(vs[i]->out[0].end(), buf[i].begin(), buf[i].begin() + st[i].framesRendered);
    vs[i]->blocks.push_back(st[i].framesRendered);
    (*frames)[i] = st[i].framesRendered;
  }
}

constexpr std::uint64_t kN = 96000;

}  // namespace

// Composition (decision 1): Seek -> SetLoop -> Seek and SetLoop -> Seek ->
// SetLoop in one block, and a Seek / SetLoop / Seek over three consecutive
// blocks, in every form. Expected: the last Seek's target (wrapped by the loop
// in force when it came), then the last loop; the playhead after the blocks is
// the position of the next frame. Memory forms: no short block.
SND_TEST(IndepFix2, SeekSetLoopCompositionInEveryForm) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pm", kN, "pcm", "PC16"));
  as.push_back(Spec("ra", kN, "compressed", "ADPC"));
  as.push_back(Spec("cp", kN, "decompressCache", "PC16"));
  as.push_back(Spec("st", kN, "stream", "PC16"));
  Built b = Build("f2comp", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    const bool stream = a.loadForm == "stream";
    for (int seq = 0; seq < 3; ++seq) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      src::ISourceKind& k = stream ? d.Stream() : d.Sample();
      IndepDriver::Voice v;
      v.slot = d.Start(k, d.Asset(a.name), 0, 0);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      for (int i = 0; i < 30; ++i) d.Block(k, {&v}, 256);
      std::uint64_t target = 0, lb = 0, le = 0;
      std::size_t before = v.out[0].size(), b0 = v.blocks.size();
      if (seq == 0) {  // Seek -> SetLoop -> Seek
        d.Control(k, v.slot, src::ControlOp::Seek, 30000);
        d.Control(k, v.slot, src::ControlOp::SetLoop, 2000, 40001);
        d.Control(k, v.slot, src::ControlOp::Seek, 35000);
        target = 35000, lb = 2000, le = 40001;
      } else if (seq == 1) {  // SetLoop -> Seek -> SetLoop
        d.Control(k, v.slot, src::ControlOp::SetLoop, 2000, 20001);
        d.Control(k, v.slot, src::ControlOp::Seek, 30000);  // wrapped by (2000, 20001)
        d.Control(k, v.slot, src::ControlOp::SetLoop, 5000, 45001);
        target = WrapInto(2000, 20001, 30000), lb = 5000, le = 45001;
      } else {  // three blocks
        d.Control(k, v.slot, src::ControlOp::Seek, 10000);
        d.Block(k, {&v}, 256);
        d.Control(k, v.slot, src::ControlOp::SetLoop, 9000, 12001);
        d.Block(k, {&v}, 256);
        d.Control(k, v.slot, src::ControlOp::Seek, 11500);
        target = 11500, lb = 9000, le = 12001;
        // The frames of the two middle blocks: from 10000 under no loop, then (9000, 12001).
        if (!stream) {
          const double e = MaxDiff(v.out[0], before, Model(ref.ch[0], 10000, 9000, 12001, 512));
          SND_CHECK_MSG(e < 1e-6, "%s seq 2 middle blocks: err %g", a.name.c_str(), e);
        }
        before = v.out[0].size();
        b0 = v.blocks.size();
      }
      for (int i = 0; i < 80; ++i) d.Block(k, {&v}, 256);
      std::uint64_t end = 0;
      const std::size_t got = v.out[0].size() - before;
      const std::vector<float> want = Model(ref.ch[0], target, lb, le, got, &end);
      const double e = MaxDiff(v.out[0], before, want);
      std::uint32_t shortBlocks = 0;
      for (std::size_t i = b0; i < v.blocks.size(); ++i) shortBlocks += v.blocks[i] < 256u ? 1u : 0u;
      SND_CHECK_MSG(e < 1e-6, "%s seq %d: err %g (first %g, want %g)", a.name.c_str(), seq, e, v.out[0][before], want[0]);
      SND_CHECK_MSG(v.ph.frame == end, "%s seq %d: playhead %llu, next frame is %llu", a.name.c_str(), seq,
                    static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
      if (!stream) SND_CHECK_MSG(shortBlocks == 0u, "%s seq %d: %u short blocks", a.name.c_str(), seq, shortBlocks);
    }
  }
}

// Seek to a target past the end of a loop set right after it in the same
// block (decision 1: "the target stays"; decision 4: "a loop end behind the
// playhead goes to the loop start"). The forms must agree on what plays and
// the playhead must name the frame that plays. All forms play from the loop
// start (1000); today the per-voice ring and the stream report another
// playhead (the ring keeps the target 50000 and wraps it arithmetically:
// 1000 + (50000 + n - 4001) % 3001).
SND_TEST(IndepFix2, Defect_source_SeekThenLoopEndingBeforeTheTargetMisplacesThePlayhead) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pm", kN, "pcm", "PC16"));
  as.push_back(Spec("ra", kN, "compressed", "ADPC"));
  as.push_back(Spec("cp", kN, "decompressCache", "PC16"));
  as.push_back(Spec("st", kN, "stream", "PC16"));
  Built b = Build("f2behind", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::vector<std::uint64_t> starts;
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    const bool stream = a.loadForm == "stream";
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = stream ? d.Stream() : d.Sample();
    IndepDriver::Voice v;
    v.slot = d.Start(k, d.Asset(a.name), 0, 0);
    for (int i = 0; i < 30; ++i) d.Block(k, {&v}, 256);
    const std::size_t before = v.out[0].size();
    d.Control(k, v.slot, src::ControlOp::Seek, 50000);
    d.Control(k, v.slot, src::ControlOp::SetLoop, 1000, 4001);
    for (int i = 0; i < 60; ++i) d.Block(k, {&v}, 256);
    std::uint64_t at = ~std::uint64_t{0};
    for (std::uint64_t s : {std::uint64_t{50000}, std::uint64_t{1000}}) {
      if (MaxDiff(v.out[0], before, Model(ref.ch[0], s, 1000, 4001, 300)) < 1e-6) at = s;
    }
    if (at == ~std::uint64_t{0}) {
      for (std::uint64_t s = 1000; s < 4001; ++s) {
        if (MaxDiff(v.out[0], before, Model(ref.ch[0], s, 1000, 4001, 300)) < 1e-6) {
          at = s;
          break;
        }
      }
    }
    std::uint64_t end = 0;
    if (at != ~std::uint64_t{0}) Model(ref.ch[0], at, 1000, 4001, v.out[0].size() - before, &end);
    std::printf("  %s: Seek(50000) + SetLoop(1000, 4001) plays from %lld, playhead %llu (next frame %llu)\n",
                a.name.c_str(), at == ~std::uint64_t{0} ? -1LL : static_cast<long long>(at),
                static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
    SND_CHECK(at != ~std::uint64_t{0});
    SND_CHECK_MSG(v.ph.frame == end, "%s: playhead %llu, next frame %llu", a.name.c_str(),
                  static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
    starts.push_back(at);
  }
  for (std::size_t i = 1; i < starts.size(); ++i) {
    SND_CHECK_MSG(starts[i] == starts[0], "forms disagree: %s starts at %llu, %s at %llu", as[0].name.c_str(),
                  static_cast<unsigned long long>(starts[0]), as[i].name.c_str(), static_cast<unsigned long long>(starts[i]));
  }
}

// Decision 2: a seek while the voice's decode job is RUNNING plays on from the
// ring, and in the block after the job ends switches with a 5 ms (240-frame
// at 48 kHz) linear cross-fade: out[f] = new[f] g + old[f] (1 - g),
// g = (f + 1) / 241, then the target's material exactly.
SND_TEST(IndepFix2, CrossFadeAfterARunningJob) {
  Built b = Build("f2xf", {Spec("ra", kN, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  GatedCodec gc;
  IndepDriver::Opt o;
  o.manualJobs = true;
  o.findCodec = &GatedCodec::Find;
  o.codecCtx = &gc;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  for (int i = 0; i < 40 && d.queue.empty(); ++i) d.Block(d.Sample(), {&v}, 256);
  SND_REQUIRE(!d.queue.empty());
  const snd::JobDesc job = d.queue.front();
  d.queue.pop_front();
  gc.armed.store(1);
  std::thread t([&] { snd::RunJob(job); });
  while (gc.entered.load() == 0) std::this_thread::yield();  // the job runs (inside Decode)
  d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 50000);
  const std::uint64_t p0 = v.ph.frame;
  const std::size_t before = v.out[0].size();
  d.Block(d.Sample(), {&v}, 256);  // job running: plays on
  SND_CHECK_EQ(v.blocks.back(), 256u);
  SND_CHECK(MaxDiff(v.out[0], before, Model(ref.ch[0], p0, 0, 0, 256)) < 1e-6);
  gc.open.store(1);
  t.join();
  const std::uint64_t p1 = v.ph.frame;
  const std::size_t at = v.out[0].size();
  d.Block(d.Sample(), {&v}, 256);
  d.RunJobs();
  for (int i = 0; i < 4; ++i) {
    d.Block(d.Sample(), {&v}, 256);
    d.RunJobs();
  }
  SND_REQUIRE(v.out[0].size() >= at + 1024u);
  const std::vector<float> oldm = Model(ref.ch[0], p1, 0, 0, 256);
  const std::vector<float> newm = Model(ref.ch[0], 50000, 0, 0, 1024);
  double eFade = 0, eAfter = 0;
  std::uint32_t fadeLen = 0;
  for (std::size_t f = 0; f < 1024; ++f) {
    const double got = v.out[0][at + f];
    if (f < 240) {
      const double g = double(f + 1) / 241.0;
      eFade = std::fmax(eFade, std::fabs(got - (newm[f] * g + oldm[f] * (1.0 - g))));
    } else {
      eAfter = std::fmax(eAfter, std::fabs(got - newm[f]));
    }
    if (std::fabs(got - newm[f]) > 1e-6) fadeLen = static_cast<std::uint32_t>(f + 1);
  }
  std::printf("  cross-fade: %u frames differ from the target, max err to the linear model %.3g, after it %.3g\n", fadeLen,
              eFade, eAfter);
  SND_CHECK(eFade < 1e-5);
  SND_CHECK(eAfter < 1e-6);
  SND_CHECK(fadeLen <= 240u && fadeLen > 200u);
  SND_CHECK_EQ(v.ph.frame, 50000u + (v.out[0].size() - at));
}

// Decision 2 race (run under TSan): Render claims the posted job (CAS 1 -> 3)
// while a worker starts it (CAS 1 -> 2). Whichever wins, the voice stays
// consistent: a few blocks later its output is the material at its playhead.
SND_TEST(IndepFix2, ClaimRacesTheJobStart) {
  Built b = Build("f2claim", {Spec("ra", kN, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.manualJobs = true;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  std::uint32_t rng = 99, raced = 0;
  for (int round = 0; round < 150; ++round) {
    for (int i = 0; i < 40 && d.queue.empty(); ++i) {
      d.Block(d.Sample(), {&v}, 256);
    }
    if (d.queue.empty()) continue;
    const snd::JobDesc job = d.queue.front();
    d.queue.pop_front();
    rng = rng * 1664525u + 1013904223u;
    d.Control(d.Sample(), v.slot, src::ControlOp::Seek, (rng >> 8) % 80000u);
    std::atomic<int> go{0};
    std::thread t([&] {
      while (go.load() == 0) {
      }
      snd::RunJob(job);
    });
    go.store(1);
    d.Block(d.Sample(), {&v}, 256);  // Render may claim the job
    t.join();
    ++raced;
    for (int i = 0; i < 4; ++i) {
      d.Block(d.Sample(), {&v}, 256);
      d.RunJobs();
    }
    const std::size_t n = v.out[0].size();
    const std::uint64_t p = v.ph.frame;
    SND_REQUIRE(p >= 256u);
    const double e = MaxDiff(v.out[0], n - 256, Model(ref.ch[0], p - 256, 0, 0, 256));
    SND_CHECK_MSG(e < 1e-6, "round %d: the last block is not the material before the playhead %llu (err %g)", round,
                  static_cast<unsigned long long>(p), e);
    if (p > 90000u) d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 0);
  }
  SND_CHECK(raced > 50u);
}

// Decision 6: at most 4 seeks are decoded on the render side per block, shared
// by parallel chunks: 12 voices seek, 3 threads render 4 voices each at once.
// At most 4 voices have frames in that block; the others start in the next.
SND_TEST(IndepFix2, DecodeBudgetIsSharedByParallelChunks) {
  Built b = Build("f2budget", {Spec("ra", kN, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.maxDecoders = 12;
  o.maxVoices = 16;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  std::vector<IndepDriver::Voice> vs(12);
  std::vector<IndepDriver::Voice*> all;
  for (auto& v : vs) {
    v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    all.push_back(&v);
  }
  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 6; ++i) d.Block(d.Sample(), all, 256);
    std::vector<std::size_t> before;
    for (std::size_t i = 0; i < vs.size(); ++i) {
      d.Control(d.Sample(), vs[i].slot, src::ControlOp::Seek, 5000 + 6000 * i + 37 * round);
      before.push_back(vs[i].out[0].size());
    }
    std::vector<std::uint32_t> f0(4), f1(4), f2(4);
    std::vector<IndepDriver::Voice*> g0(all.begin(), all.begin() + 4), g1(all.begin() + 4, all.begin() + 8),
        g2(all.begin() + 8, all.end());
    std::thread t1([&] { RenderOn(d.Sample(), g1, &f1); });
    std::thread t2([&] { RenderOn(d.Sample(), g2, &f2); });
    RenderOn(d.Sample(), g0, &f0);
    t1.join();
    t2.join();
    d.Sample().ScheduleBackground(src::BlockInfo{0, 0, 256, 48000});
    d.RunJobs();
    std::uint32_t played = 0;
    for (auto* f : {&f0, &f1, &f2}) {
      for (std::uint32_t x : *f) played += x != 0u ? 1u : 0u;
    }
    SND_CHECK_MSG(played <= 4u, "round %d: %u voices decoded in one block", round, played);
    for (int i = 0; i < 4; ++i) d.Block(d.Sample(), all, 256);
    for (std::size_t i = 0; i < vs.size(); ++i) {
      const double e = MaxDiff(vs[i].out[0], before[i], Model(ref.ch[0], 5000 + 6000 * i + 37 * round, 0, 0, 512));
      SND_CHECK_MSG(e < 1e-6, "round %d voice %zu: err %g", round, i, e);
    }
  }
}

// Decision 5 with the loop start OUTSIDE the preload: shortening the loop keeps
// the decoded PCM up to the new loop end; the loop start must be read from
// the file. Exact; the number of short blocks is printed.
SND_TEST(IndepFix2, ShortenedStreamLoopWithItsStartOutsideThePreload) {
  Built b = Build("f2shorten", {Spec("st", kN, "stream", "PC16", "\"loop\": {\"start\": 1000, \"end\": 80001}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "st");
  SND_REQUIRE(ref.ok);
  for (std::uint64_t lb : {std::uint64_t{1000}, std::uint64_t{20000}}) {  // preload ~4800 frames
    for (std::uint64_t ahead : {std::uint64_t{300}, std::uint64_t{3000}, std::uint64_t{9000}}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset("st"), 0, src::kStartLoop);
      for (int i = 0; i < 120; ++i) d.Block(d.Stream(), {&v}, 256);
      const std::uint64_t p = v.ph.frame;  // 30720
      const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, lb, p + ahead);
      for (int i = 0; i < 120; ++i) d.Block(d.Stream(), {&v}, 256);
      std::uint64_t end = 0;
      const std::size_t got = v.out[0].size() - before;
      const double e = MaxDiff(v.out[0], before, Model(ref.ch[0], p, lb, p + ahead, got, &end));
      std::uint32_t sb = 0;
      for (std::size_t i = b0; i < v.blocks.size(); ++i) sb += v.blocks[i] < 256u ? 1u : 0u;
      if (e >= 1e-6) {  // where the output leaves the model, and what it plays there
        const std::vector<float> m = Model(ref.ch[0], p, lb, p + ahead, got);
        for (std::size_t i = 0; i < m.size(); ++i) {
          if (std::fabs(v.out[0][before + i] - m[i]) > 1e-6) {
            long long src = -1;
            for (std::size_t s2 = 0; s2 + 2 < ref.ch[0].size(); ++s2) {
              if (std::fabs(ref.ch[0][s2] - v.out[0][before + i]) < 1e-7 && std::fabs(ref.ch[0][s2 + 1] - v.out[0][before + i + 1]) < 1e-7 &&
                  std::fabs(ref.ch[0][s2 + 2] - v.out[0][before + i + 2]) < 1e-7) {
                src = static_cast<long long>(s2);
                break;
              }
            }
            std::printf("    leaves the model at output frame %zu (model frame %llu): plays material frame %lld\n", i,
                        static_cast<unsigned long long>(p + i < p + ahead ? p + i : lb + (i - ahead)), src);
            break;
          }
        }
      }
      std::printf("  SetLoop(%llu, p + %llu): err %.3g, %u short blocks, playhead %s\n", static_cast<unsigned long long>(lb),
                  static_cast<unsigned long long>(ahead), e, sb, v.ph.frame == end ? "ok" : "WRONG");
      if (lb == 1000u || ahead > 1000u) {
        SND_CHECK_MSG(e < 1e-6, "lb %llu ahead %llu: err %g", static_cast<unsigned long long>(lb),
                      static_cast<unsigned long long>(ahead), e);
        SND_CHECK(v.ph.frame == end);
      } else {
        // The loop start is not in memory and the loop end comes within a block: the
        // voice underruns (6.3: fade, stall, time priority skips). Exact up to the fade.
        SND_CHECK(MaxDiff(v.out[0], before, Model(ref.ch[0], p, lb, p + ahead, 256)) < 1e-6);
        SND_CHECK(sb != 0u);
      }
      if (lb == 1000u) SND_CHECK_MSG(sb == 0u, "loop start in the preload: %u short blocks", sb);
    }
  }
}
