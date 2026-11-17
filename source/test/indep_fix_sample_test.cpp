// Independent re-check of the Sample fixes (728ea97 / 18013cb): the request
// hand-over (RingCtx::req) when requests meet in one block, the bounded decode
// in RenderBatch (4 voices per batch), the boundaries of SetLoop in place,
// multichannel rings and the memory the new ring sizing takes.
// Model of every check: Control ops apply in order; the frames rendered after
// them continue from the position they leave, following the loop they leave.
// Defect_source_* tests fail on purpose.
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
// n frames played from p under loop [lb, le) (le == 0: no loop), stopping at total.
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::size_t n) {
  std::vector<float> v;
  while (v.size() < n && p < x.size()) {
    v.push_back(x[static_cast<std::size_t>(p)]);
    if (++p == le && le != 0u) p = lb;
  }
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
std::uint32_t ShortBlocks(const IndepDriver::Voice& v, std::size_t from) {
  std::uint32_t n = 0;
  for (std::size_t i = from; i < v.blocks.size(); ++i) n += v.blocks[i] < 256u ? 1u : 0u;
  return n;
}
void Render(IndepDriver& d, IndepDriver::Voice& v, int blocks) {
  for (int i = 0; i < blocks; ++i) d.Block(d.Sample(), {&v}, 256);
}

double PerChannel(std::uint32_t c, std::uint64_t i) {
  return 0.08 * (c + 1) * std::sin(6.283185307179586 * (300.0 + 70.0 * c) * double(i) / 48000.0);
}

constexpr const char* kLoop = "\"loop\": {\"start\": 4000, \"end\": 60001}";

}  // namespace

// Seek then SetLoop in the same block (one Control call each, as core sends
// them in RunEventLogic): the voice must play from the seek target under the
// new loop. Today the SetLoop request (req = 2) overwrites the pending seek
// (req = 1) in the single hand-over slot: the seek is lost.
SND_TEST(IndepFixSample, SeekThenSetLoopInOneBlockKeepsTheSeek) {
  Built b = Build("fsl1", {Spec("ra", 96000, "compressed", "ADPC", kLoop)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, src::kStartLoop);
  Render(d, v, 20);
  const std::size_t before = v.out[0].size();
  d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 10000);
  d.Control(d.Sample(), v.slot, src::ControlOp::SetLoop, 0, 0);  // loop off
  Render(d, v, 60);
  const std::vector<float> want = Model(ref.ch[0], 10000, 0, 0, 12000);
  const double e = MaxDiff(v.out[0], before, want);
  SND_CHECK_MSG(e < 1e-6, "seek + loop off: err %g (first frame %g, want %g)", e, v.out[0][before], want[0]);
}

// SetLoop that does not fit in place (the new loop end is behind the decoder),
// then Seek in the same block: the seek must follow the NEW loop. Today the
// seek request overwrites the loop request and the old loop stays.
SND_TEST(IndepFixSample, SetLoopThenSeekInOneBlockKeepsTheLoop) {
  Built b = Build("fsl2", {Spec("ra", 96000, "compressed", "ADPC", kLoop)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, src::kStartLoop);
  Render(d, v, 20);  // playhead 5120, the ring decoded ahead
  const std::size_t before = v.out[0].size();
  d.Control(d.Sample(), v.slot, src::ControlOp::SetLoop, 3000, 6001);
  d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 5500);
  Render(d, v, 30);
  const std::vector<float> want = Model(ref.ch[0], 5500, 3000, 6001, 6000);
  const double e = MaxDiff(v.out[0], before, want);
  SND_CHECK_MSG(e < 1e-6, "loop (3000, 6001) + seek 5500: err %g", e);
}

// Two seeks in one block: the last one wins; the output has no hole (data in memory).
SND_TEST(IndepFixSample, TwoSeeksInOneBlockLastWinsWithoutAHole) {
  Built b = Build("fsl3", {Spec("ra", 96000, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  Render(d, v, 10);
  for (std::uint64_t t : {std::uint64_t{70000}, std::uint64_t{33333}}) d.Control(d.Sample(), v.slot, src::ControlOp::Seek, t);
  const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
  Render(d, v, 10);
  SND_CHECK_EQ(ShortBlocks(v, b0), 0u);
  SND_CHECK(MaxDiff(v.out[0], before, Model(ref.ch[0], 33333, 0, 0, 2560)) < 1e-6);
  // A seek in each of three consecutive blocks: each block starts at its target.
  for (std::uint64_t t : {std::uint64_t{1000}, std::uint64_t{50000}, std::uint64_t{20}}) {
    d.Control(d.Sample(), v.slot, src::ControlOp::Seek, t);
    const std::size_t at = v.out[0].size(), bb = v.blocks.size();
    Render(d, v, 1);
    SND_CHECK_EQ(ShortBlocks(v, bb), 0u);
    SND_CHECK(MaxDiff(v.out[0], at, Model(ref.ch[0], t, 0, 0, 256)) < 1e-6);
  }
}

// Bounded decode: 7 compressed voices seek in one block (one RenderBatch).
// 4 are decoded on the render thread; the others go to the next block. Every
// voice plays exactly the material from its target; the deferred ones lose at
// most one block (silence, position kept), never play wrong data.
SND_TEST(IndepFixSample, BoundedDecodeDefersSeeksBeyondFourVoices) {
  Built b = Build("fsl4", {Spec("ra", 96000, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.maxDecoders = 8;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  std::vector<IndepDriver::Voice> vs(7);
  std::vector<IndepDriver::Voice*> ptr;
  for (auto& v : vs) {
    v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    ptr.push_back(&v);
  }
  for (int i = 0; i < 10; ++i) d.Block(d.Sample(), ptr, 256);
  std::vector<std::size_t> before, b0;
  for (std::size_t i = 0; i < vs.size(); ++i) {
    d.Control(d.Sample(), vs[i].slot, src::ControlOp::Seek, 10000 + 7000 * i);
    before.push_back(vs[i].out[0].size());
    b0.push_back(vs[i].blocks.size());
  }
  for (int i = 0; i < 12; ++i) d.Block(d.Sample(), ptr, 256);
  std::uint32_t deferred = 0;
  for (std::size_t i = 0; i < vs.size(); ++i) {
    const std::uint32_t sb = ShortBlocks(vs[i], b0[i]);
    deferred += sb != 0u ? 1u : 0u;
    SND_CHECK_MSG(sb <= 1u, "voice %zu: %u short blocks", i, sb);
    if (sb == 1u) SND_CHECK_EQ(vs[i].blocks[b0[i]], 0u);  // the deferred block is silent, not partial
    const double e = MaxDiff(vs[i].out[0], before[i], Model(ref.ch[0], 10000 + 7000 * i, 0, 0, 2000));
    SND_CHECK_MSG(e < 1e-6, "voice %zu: err %g", i, e);
  }
  std::printf("  7 seeks in one batch: %u voices deferred by a block\n", deferred);
  SND_CHECK(deferred <= 3u);
}

// SetLoop on a playing per-voice-ring voice at the boundaries of the in-place
// change: the new loop end ahead of the decoder (fits), between the playhead
// and the decoder (keep the ring up to it), a new loop start only, loop off,
// and a loop end 1 frame after the playhead. All: exact, no hole.
SND_TEST(IndepFixSample, SetLoopInPlaceBoundariesAreExactWithoutAHole) {
  Built b = Build("fsl5", {Spec("ra", 96000, "compressed", "ADPC", kLoop)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  struct Case {
    std::int64_t lbOff;  // relative to the playhead p (absolute when absolute == true)
    std::int64_t leOff;
    bool absolute;
    const char* what;
  };
  const Case cases[] = {
      {4000, 60001, true, "same loop"},
      {4000, 70001, true, "end moved later (ahead of the decoder)"},
      {-1000, 1, false, "end 1 frame after the playhead"},
      {-1000, 50, false, "end 50 frames after the playhead"},
      {-1000, 3000, false, "end inside the decoded ring"},
      {-1000, 4095, false, "end at the edge of the ring"},
      {-1000, 9000, false, "end past the decoded ring"},
      {1000, 60001, true, "start only"},
      {0, 0, true, "loop off"},
  };
  for (const Case& c : cases) {
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, src::kStartLoop);
    Render(d, v, 20);
    const std::uint64_t p = v.ph.frame;
    const std::uint64_t lb = c.absolute ? static_cast<std::uint64_t>(c.lbOff) : p + c.lbOff;
    const std::uint64_t le = c.absolute ? static_cast<std::uint64_t>(c.leOff) : p + c.leOff;
    const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
    d.Control(d.Sample(), v.slot, src::ControlOp::SetLoop, lb, le);
    Render(d, v, 60);
    const std::vector<float> want = Model(ref.ch[0], p, lb, le, 14000);
    const double e = MaxDiff(v.out[0], before, want);
    SND_CHECK_MSG(e < 1e-6, "%s: err %g", c.what, e);
    SND_CHECK_MSG(ShortBlocks(v, b0) == 0u, "%s: %u short blocks", c.what, ShortBlocks(v, b0));
  }
}

// SetLoop to a loop end BEHIND the playhead: the forms disagree on where the
// voice goes (spec gap, printed): PCM resident (read in place) jumps to the
// loop start; the per-voice ring maps the position into the loop (WrapInto).
SND_TEST(IndepFixSample, SetLoopBehindThePlayheadPerForm) {
  Built b = Build("fsl6", {Spec("pc", 96000, "pcm", "PC16", kLoop), Spec("ra", 96000, "compressed", "ADPC", kLoop)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const char* name : {"pc", "ra"}) {
    const Decoded ref = Reference(b, name);
    SND_REQUIRE(ref.ok);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), d.Asset(name), 0, src::kStartLoop);
    Render(d, v, 20);  // p = 5120
    const std::size_t before = v.out[0].size();
    d.Control(d.Sample(), v.slot, src::ControlOp::SetLoop, 1000, 4001);
    Render(d, v, 4);
    std::uint64_t at = ~std::uint64_t{0};
    for (std::uint64_t s = 1000; s < 4001; ++s) {
      if (MaxDiff(v.out[0], before, Model(ref.ch[0], s, 1000, 4001, 256)) < 1e-6) {
        at = s;
        break;
      }
    }
    std::printf("  %s: playhead 5120, SetLoop(1000, 4001) -> continues at %llu\n", name, static_cast<unsigned long long>(at));
    SND_CHECK(at != ~std::uint64_t{0});
  }
}

// 7 (rings for maxVoiceChannels): 8-channel compressed voices up to
// maxDecoders at once, every channel its own material.
SND_TEST(IndepFixSample, EightChannelRingsUpToMaxDecoders) {
  Built b = Build("fsl7", {Spec("r8", 20000, "compressed", "ADPC", "", 8, &PerChannel)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "r8");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.maxDecoders = 4;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  std::vector<IndepDriver::Voice> vs(4);
  std::vector<IndepDriver::Voice*> ptr;
  for (auto& v : vs) {
    v.channels = 8;
    v.slot = d.Start(d.Sample(), d.Asset("r8"), 0, 0);
    SND_CHECK(v.slot != snd::kInvalidSlot);
    ptr.push_back(&v);
  }
  for (int i = 0; i < 20; ++i) d.Block(d.Sample(), ptr, 256);
  double e = 0;
  for (auto& v : vs) {
    for (std::uint32_t c = 0; c < 8; ++c) e = std::fmax(e, MaxDiff(v.out[c], 0, Model(ref.ch[c], 0, 0, 0, 4000)));
  }
  SND_CHECK_MSG(e < 1e-6, "err %g", e);
  // Memory of the kinds for the default-like limits (printed: the new ring sizing).
  src::SourceLimits l{};
  l.maxVoices = 256;
  l.maxRealVoices = 160;
  l.maxStreams = 32;
  l.streamChunkBytes = 64u << 10;
  l.maxDecoders = 64;
  l.decoderSlotBytes = 64u << 10;
  l.voiceRingFrames = 4096;
  l.pcmCacheBytes = 0;
  l.pcmCachePageBytes = 64u << 10;
  l.blockFrames = 256;
  l.sampleRate = 48000;
  l.streamTargetAheadMs = 200;
  std::printf("  pools (maxDecoders 64, maxStreams 32, maxVoiceChannels 8): Sample %.1f MiB, Stream %.1f MiB\n",
              double(d.Sample().PoolBytes(l)) / 1048576.0, double(d.Stream().PoolBytes(l)) / 1048576.0);
}

// A seek that arrives while the voice's decode job is posted but not finished
// (a busy prefetch lane): the request waits for the job (the decoder is
// shared), and the voice renders nothing meanwhile. Decision 1 asks for no hole
// on data in memory; with a slow job the hole lasts until the job ends.
SND_TEST(IndepFixSample, SeekWhileTheRingJobIsQueuedLeavesNoHole) {
  Built b = Build("fsl8", {Spec("ra", 96000, "compressed", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "ra");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.manualJobs = true;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Sample(), d.Asset("ra"), 0, 0);
  for (int i = 0; i < 40 && d.queue.empty(); ++i) d.Block(d.Sample(), {&v}, 256);
  SND_REQUIRE(!d.queue.empty());  // a decode job is posted, not run yet
  d.Control(d.Sample(), v.slot, src::ControlOp::Seek, 50000);
  const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
  d.Block(d.Sample(), {&v}, 256);  // the job is still queued
  d.RunJobs();
  for (int i = 0; i < 6; ++i) {
    d.Block(d.Sample(), {&v}, 256);
    d.RunJobs();
  }
  SND_CHECK_MSG(ShortBlocks(v, b0) == 0u, "%u short blocks (first %u frames)", ShortBlocks(v, b0), v.blocks[b0]);
  SND_CHECK(MaxDiff(v.out[0], before, Model(ref.ch[0], 50000, 0, 0, 1000)) < 1e-6);
}
