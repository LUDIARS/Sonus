// The Sample and Stream kinds driven directly (kind_driver.hpp): source frames
// compared exactly with the codec's decode. Seek (D-23 pre-roll), loop count,
// virtualisation, release while a decode job runs (epoch, A1-15), RT safety
// (every call in SND_RT_SCOPE) and no allocation after Init.
#include <cmath>
#include <cstdio>
#include <vector>

#include "kind_driver.hpp"
#include "reference.hpp"

using namespace srctest;
namespace src = snd::source;

namespace {
AssetSpec Spec(const char* name, std::uint64_t frames, const char* form, const char* codec, const char* extra = "") {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, &Sine1k);
  a.loadForm = form;
  a.codec = codec;
  a.extra = extra;
  return a;
}
double MaxDiff(const std::vector<float>& got, const std::vector<float>& want, std::size_t from, std::size_t n) {
  double m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double g = from + i < got.size() ? got[from + i] : 1e9;
    const double w = i < want.size() ? want[i] : 0.0;
    m = std::fmax(m, std::fabs(g - w));
  }
  return m;
}
std::vector<float> Slice(const std::vector<float>& x, std::size_t from, std::size_t n) {
  std::vector<float> v;
  for (std::size_t i = from; i < from + n && i < x.size(); ++i) v.push_back(x[i]);
  return v;
}
// Renders until `frames` source frames were produced (or `limit` blocks).
void RenderUntil(KindDriver& d, src::ISourceKind& k, KindDriver::Voice& v, std::size_t frames, int limit = 4000) {
  for (int i = 0; i < limit && (v.out.empty() || v.out[0].size() < frames) && v.ended == 0u; ++i) d.Block(k, {&v}, 256);
}

std::vector<AssetSpec> AllForms() {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pcm", 40000, "pcm", "PC16"));
  as.push_back(Spec("rad", 40000, "compressed", "ADPC"));
  as.push_back(Spec("cad", 40000, "decompressCache", "ADPC"));
  as.push_back(Spec("spc", 40000, "stream", "PC16"));
  as.push_back(Spec("sad", 40000, "stream", "ADPC"));
  if (snd::codec::OpusAvailable()) {
    as.push_back(Spec("rop", 40000, "compressed", "OPUS"));
    as.push_back(Spec("sop", 40000, "stream", "OPUS"));
  }
  return as;
}
double Tol(const AssetSpec& a) { return a.codec == "OPUS" ? 1e-4 : 1e-6; }
}  // namespace

SND_TEST(SourceKind, StartOffsetAndSeekAreSampleAccurate) {
  const std::vector<AssetSpec> as = AllForms();
  Built b = Build("kseek", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    KindDriver d(b, KindDriver::Opt{});
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
    // Start at an arbitrary frame (not a packet boundary).
    KindDriver::Voice v;
    v.ph.frame = 12345;
    v.slot = d.Start(k, d.Asset(a.name), 12345, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    RenderUntil(d, k, v, 6000);
    const double e1 = MaxDiff(v.out[0], Slice(ref.ch[0], 12345, 6000), 0, 6000);
    SND_CHECK_MSG(e1 < Tol(a), "%s start: %g", a.name.c_str(), e1);
    // Seek backwards and forwards.
    for (std::uint64_t target : {std::uint64_t{777}, std::uint64_t{31000}}) {
      d.Control(k, v.slot, src::ControlOp::Seek, target);
      const std::size_t before = v.out[0].size();
      RenderUntil(d, k, v, before + 4000);
      const double e = MaxDiff(v.out[0], Slice(ref.ch[0], target, 4000), before, 4000);
      SND_CHECK_MSG(e < Tol(a), "%s seek %llu: %g", a.name.c_str(), static_cast<unsigned long long>(target), e);
      if (e >= Tol(a)) {
        for (std::size_t i = 0; i < 4000; i += 500) std::printf("  %zu got %g want %g\n", i, v.out[0][before + i], ref.ch[0][target + i]);
        for (std::size_t s2 = 0; s2 < 40000; ++s2) {
          if (std::fabs(ref.ch[0][s2] - v.out[0][before]) < 1e-6 && std::fabs(ref.ch[0][s2 + 1] - v.out[0][before + 1]) < 1e-6 && std::fabs(ref.ch[0][s2 + 7] - v.out[0][before + 7]) < 1e-6) { std::printf("  matches material at %zu\n", s2); break; }
        }
      }
      SND_CHECK_EQ(v.ph.frame, target + (v.out[0].size() - before));
    }
    // To the end: exactly total frames, then ended.
    RenderUntil(d, k, v, ~std::size_t{0}, 2000);
    SND_CHECK(v.ended != 0u);
    SND_CHECK_EQ(v.ph.frame, ref.h.total_frames);
    d.Release(k, v.slot);
  }
}

// Loop count (proposed flags bits 16..31): count 3 plays [0, le) + [lb, le) + [lb, end).
SND_TEST(SourceKind, LoopCountPlaysThenRunsOut) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pcm", 20000, "pcm", "PC16", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  as.push_back(Spec("rad", 20000, "compressed", "ADPC", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  as.push_back(Spec("spc", 20000, "stream", "PC16", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  as.push_back(Spec("sad", 20000, "stream", "ADPC", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  Built b = Build("kcount", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    std::vector<float> model;
    for (std::size_t i = 0; i < 9001; ++i) model.push_back(ref.ch[0][i]);
    for (std::size_t i = 4000; i < 9001; ++i) model.push_back(ref.ch[0][i]);
    for (std::size_t i = 4000; i < 20000; ++i) model.push_back(ref.ch[0][i]);
    KindDriver d(b, KindDriver::Opt{});
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
    KindDriver::Voice v;
    v.slot = d.Start(k, d.Asset(a.name), 0, src::kStartLoop | (3u << 16));
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    RenderUntil(d, k, v, ~std::size_t{0}, 3000);
    SND_CHECK(v.ended != 0u);
    SND_CHECK_EQ(v.out[0].size(), model.size());
    SND_CHECK_MSG(MaxDiff(v.out[0], model, 0, model.size()) < 1e-6, "%s", a.name.c_str());
    SND_CHECK_EQ(v.ph.loopCount, 2u);
    // Infinite (count 0) keeps looping.
    KindDriver::Voice w;
    w.slot = d.Start(k, d.Asset(a.name), 0, src::kStartLoop);
    RenderUntil(d, k, w, 60000);
    SND_CHECK(w.ended == 0u);
    SND_CHECK_MSG(MaxDiff(w.out[0], LoopModel(ref.ch[0], 4000, 9001, 60000), 0, 60000) < 1e-6, "%s inf", a.name.c_str());
  }
}

SND_TEST(SourceKind, VirtualiseAndReturn) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("rad", 96000, "compressed", "ADPC"));
  as.push_back(Spec("sad", 96000, "stream", "ADPC"));
  Built b = Build("kvirt", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    KindDriver d(b, KindDriver::Opt{});
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
    KindDriver::Voice v;
    v.slot = d.Start(k, d.Asset(a.name), 0, 0);
    RenderUntil(d, k, v, 5000);
    d.Control(k, v.slot, src::ControlOp::Virtualize);
    // 40 blocks virtual at pitch 1.5: playhead advances 15360 frames.
    for (int i = 0; i < 40; ++i) {
      src::AdvanceItem it{0, v.slot, 256, 1.5f, &v.ph};
      snd::VoiceRenderStatus st{};
      k.AdvanceVirtual(&it, 1, &st);
      d.Block(k, {}, 256);
    }
    const std::uint64_t pos = v.ph.frame;
    SND_CHECK_EQ(pos, v.out[0].size() + 15360u);
    d.Control(k, v.slot, src::ControlOp::Devirtualize, pos);
    const std::size_t before = v.out[0].size();
    RenderUntil(d, k, v, before + 3000);
    SND_CHECK_MSG(MaxDiff(v.out[0], Slice(ref.ch[0], pos, 3000), before, 3000) < 1e-6, "%s", a.name.c_str());
    if (a.loadForm == "stream") SND_CHECK_EQ(d.pool.Used() <= 4u, true);
  }
}

// A voice released while its decode job is still queued keeps its decoder slot
// until the job ran (epoch, 3.3); the slot then returns with its generation
// (A1-15: no stale release reaches a new owner).
SND_TEST(SourceKind, ReleaseWhileTheJobRuns) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("rad", 96000, "compressed", "ADPC"));
  as.push_back(Spec("sad", 96000, "stream", "ADPC"));
  Built b = Build("krel", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    KindDriver::Opt o;
    o.manualJobs = true;
    o.maxDecoders = 1;
    KindDriver d(b, o);
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
    KindDriver::Voice v;
    v.slot = d.Start(k, d.Asset(a.name), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    for (int i = 0; i < 30 && d.queue.empty(); ++i) d.Block(k, {&v}, 256);
    SND_REQUIRE(!d.queue.empty());  // a decode job is posted and not run yet
    d.Release(k, v.slot);
    // The only decoder is still held: a new voice is refused.
    std::uint32_t detail = 0;
    SND_CHECK_EQ(d.Start(k, d.Asset(a.name), 0, 0, &detail), snd::kInvalidSlot);
    SND_CHECK_EQ(detail, SND_END_DETAIL_DECODER_POOL_FULL);
    SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 1u);
    d.RunJobs();  // the job sees the cancel flag (or runs) and finishes
    for (int i = 0; i < 10; ++i) d.Block(k, {}, 256);
    SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 0u);
    KindDriver::Voice w;
    w.slot = d.Start(k, d.Asset(a.name), 0, 0);
    SND_CHECK(w.slot != snd::kInvalidSlot);
    for (int i = 0; i < 10; ++i) {
      d.Block(k, {&w}, 256);
      d.RunJobs();
    }
    SND_CHECK(w.out[0].size() > 1000u);
  }
}

// Steady state (start, render, background, seek, release) allocates nothing.
SND_TEST(SourceKind, NoAllocationAfterInit) {
  std::vector<AssetSpec> as = AllForms();
  Built b = Build("kalloc", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  struct Counting final : snd::IAllocator {
    void* Allocate(std::size_t n, std::size_t al, snd::MemTag t) override {
      ++count;
      return snd::base::DefaultAllocator().Allocate(n, al, t);
    }
    void Free(void* p, snd::MemTag t) override { snd::base::DefaultAllocator().Free(p, t); }
    std::uint64_t count = 0;
  } alloc;
  KindDriver::Opt o;
  o.allocator = &alloc;
  o.maxStreams = 4;
  KindDriver d(b, o);
  SND_REQUIRE(d.Ok());
  const std::uint64_t afterInit = alloc.count;
  for (int round = 0; round < 2; ++round) {
    for (const AssetSpec& a : as) {
      src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
      KindDriver::Voice v;
      v.slot = d.Start(k, d.Asset(a.name), 100, src::kStartLoop);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      for (int i = 0; i < 20; ++i) d.Block(k, {&v}, 256);
      d.Control(k, v.slot, src::ControlOp::Seek, 20000);
      for (int i = 0; i < 20; ++i) d.Block(k, {&v}, 256);
      d.Release(k, v.slot);
      for (int i = 0; i < 4; ++i) d.Block(k, {}, 256);
    }
  }
  SND_CHECK_EQ(alloc.count, afterInit);
}

// SetLoop (Control): loop off while playing a loop -> continues to the end.
SND_TEST(SourceKind, SetLoopOffRunsOutFromThePosition) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pcm", 20000, "pcm", "PC16", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  as.push_back(Spec("rad", 20000, "compressed", "ADPC", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  as.push_back(Spec("sad", 20000, "stream", "ADPC", "\"loop\": {\"start\": 4000, \"end\": 9001}"));
  Built b = Build("ksetloop", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    KindDriver d(b, KindDriver::Opt{});
    SND_REQUIRE(d.Ok());
    src::ISourceKind& k = a.loadForm == "stream" ? d.Stream() : d.Sample();
    KindDriver::Voice v;
    v.slot = d.Start(k, d.Asset(a.name), 0, src::kStartLoop);
    RenderUntil(d, k, v, 15000);
    const std::size_t at = v.out[0].size();
    const std::uint64_t pos = v.ph.frame;
    d.Control(k, v.slot, src::ControlOp::SetLoop, 0, 0);
    RenderUntil(d, k, v, ~std::size_t{0}, 2000);
    SND_CHECK(v.ended != 0u);
    SND_CHECK_EQ(v.out[0].size() - at, ref.h.total_frames - pos);
    SND_CHECK_MSG(MaxDiff(v.out[0], Slice(ref.ch[0], pos, ref.h.total_frames - pos), at, ref.h.total_frames - pos) < 1e-6,
                  "%s", a.name.c_str());
  }
}
