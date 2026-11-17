// Independent re-check of the fifth round (67ff330): a playback binds its
// stream file (ReopenFile at the start, CloseFile at the end), and loops no
// longer than the PCM ring are repeated from the ring. Properties checked:
//   - many overlapping playbacks of one path, started and released in turns,
//     leave no file open; while they play, at most one open file each;
//   - short loops with a loop count, the voice started inside or before the
//     loop: every pass, the material after the last pass, the end, loopCount;
//   - Seek, SetLoop, Virtualize / Devirtualize while a loop is repeated;
//   - an IO error after the repeated passes (finite count): the voice plays
//     what it has and ends; the file is closed.
// Defect_source_* tests fail on purpose.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

AssetSpec Spec(const char* name, std::uint64_t frames, const char* codec, const char* extra = "") {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, &Ramp);
  a.loadForm = "stream";
  a.codec = codec;
  a.extra = extra;
  return a;
}
// Frames from p under [lb, le) played `count` times in total (0 = infinite), to the end or n frames.
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::uint32_t count,
                         std::size_t n, std::uint64_t* end = nullptr, std::uint32_t* loops = nullptr) {
  std::vector<float> v;
  std::uint32_t done = 0;
  while (v.size() < n && p < x.size()) {
    v.push_back(x[static_cast<std::size_t>(p)]);
    ++p;
    if (le != 0u && p == le && (count == 0u || done + 1u < count)) {
      p = lb;
      ++done;
    }
  }
  if (end != nullptr) *end = p;
  if (loops != nullptr) *loops = done;
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
std::string PathOf(const Built& b, const std::string& name) {
  for (const auto& f : b.files) {
    if (f.first.find("/" + name + "_") != std::string::npos) return f.first;
  }
  return std::string();
}

}  // namespace

// Up to maxStreams playbacks of one path at once, started and released in
// turns for many rounds: never more files than slots; when all are
// released, no file is open.
SND_TEST(IndepFix4, OverlappingPlaybacksLeaveNoFileOpen) {
  Built b = Build("f4leak", {Spec("s", 96000, "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver::Opt o;
  o.maxStreams = 6;
  o.maxDecoders = 8;
  o.poolBytes = 512u << 10;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  std::vector<IndepDriver::Voice> vs(6);
  std::uint32_t worst = 0, over = 0, rng = 7;
  for (int round = 0; round < 300; ++round) {
    rng = rng * 1664525u + 1013904223u;
    IndepDriver::Voice& v = vs[(rng >> 8) % vs.size()];
    if (v.slot == snd::kInvalidSlot || v.ended != 0u) {
      if (v.slot != snd::kInvalidSlot) d.Release(d.Stream(), v.slot);
      v = IndepDriver::Voice{};
      v.slot = d.Start(d.Stream(), d.Asset("s"), (rng >> 4) % 80000u, 0);
    } else if ((rng & 3u) == 0u) {
      d.Release(d.Stream(), v.slot);
      v = IndepDriver::Voice{};
    } else if ((rng & 7u) == 1u) {
      d.Control(d.Stream(), v.slot, src::ControlOp::Seek, (rng >> 6) % 90000u);
    }
    std::vector<IndepDriver::Voice*> live;
    for (auto& w : vs) {
      if (w.slot != snd::kInvalidSlot) live.push_back(&w);
    }
    d.Block(d.Stream(), live, 256);
    const std::uint32_t open = d.fs.OpenFiles();
    if (open > worst) worst = open;
    if (open > live.size()) ++over;
  }
  for (auto& w : vs) {
    if (w.slot != snd::kInvalidSlot) d.Release(d.Stream(), w.slot);
  }
  for (int i = 0; i < 10; ++i) d.Block(d.Stream(), {}, 256);
  std::printf("  most files open at once %u; blocks with more files than playbacks %u; after the releases %u\n", worst, over,
              d.fs.OpenFiles());
  // A released playback's file is closed once its last read is done and the IO
  // side has seen CloseFile (the next IO pass): briefly one more file than playbacks.
  SND_CHECK(worst <= vs.size());
  SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
  SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
  SND_CHECK_EQ(d.pool.Used(), 0u);
}

// Short loops with a loop count (flags bits 16..31), started before the loop,
// inside it and past its end, PC16 and ADPCM: every pass, the material after
// the last pass to the end, `ended`, the final playhead and loopCount.
SND_TEST(IndepFix4, ShortLoopCountsFromEveryStart) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("p", 40000, "PC16", "\"loop\": {\"start\": 10000, \"end\": 10003}"));
  as.push_back(Spec("a", 40000, "ADPC", "\"loop\": {\"start\": 10000, \"end\": 10003}"));
  as.push_back(Spec("q", 40000, "PC16", "\"loop\": {\"start\": 0, \"end\": 700}"));
  as.push_back(Spec("e", 40000, "PC16", "\"loop\": {\"start\": 39000, \"end\": 40000}"));  // ends at the material end
  Built b = Build("f4count", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    for (std::uint32_t count : {2u, 3u, 7u}) {
      for (std::uint64_t start : {std::uint64_t{0}, ref.loopStart + 1u, std::uint64_t{ref.loopEnd + 5u}}) {
        if (start >= ref.h.total_frames) continue;
        if (a.name == "q" && start != 0u) continue;  // Defect_source_FiniteLoopAtTheFileStartStallsWhenStartedInsideIt
        IndepDriver d(b, IndepDriver::Opt{});
        SND_REQUIRE(d.Ok());
        IndepDriver::Voice v;
        v.ph.frame = start;
        v.slot = d.Start(d.Stream(), d.Asset(a.name), start, src::kStartLoop | (count << 16));
        SND_REQUIRE(v.slot != snd::kInvalidSlot);
        for (int i = 0; i < 1200 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
        std::uint64_t end = 0;
        std::uint32_t loops = 0;
        // A start past the loop end is mapped into the loop (WrapInto) and counts as in it.
        const std::uint64_t s0 = start >= ref.loopEnd ? ref.loopStart + (start - ref.loopEnd) % (ref.loopEnd - ref.loopStart)
                                                      : start;
        const std::vector<float> want = Model(ref.ch[0], s0, ref.loopStart, ref.loopEnd, count, SIZE_MAX, &end, &loops);
        const double e = MaxDiff(v.out[0], 0, want);
        const bool ok = v.ended != 0u && v.out[0].size() == want.size() && e < 1e-6 && v.ph.frame == end &&
                        v.ph.loopCount == loops;
        SND_CHECK_MSG(ok, "%s count %u start %llu: ended %u, %zu of %zu frames, err %g, playhead %llu (want %llu), loops %u (want %u)",
                      a.name.c_str(), count, static_cast<unsigned long long>(start), v.ended, v.out[0].size(), want.size(), e,
                      static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end), v.ph.loopCount, loops);
      }
    }
  }
}

// While a short loop is repeated from the ring (infinite): Seek inside the
// loop, Seek outside it, SetLoop to another short loop, SetLoop off, and a
// Virtualize / Devirtualize round trip. The concatenated output follows the
// model after each (holes allowed: re-reads), and the playhead names the
// next frame.
SND_TEST(IndepFix4, ControlWhileALoopIsRepeated) {
  Built b = Build("f4ctl", {Spec("s", 96000, "PC16", "\"loop\": {\"start\": 20000, \"end\": 20500}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "s");
  SND_REQUIRE(ref.ok);
  struct Case {
    const char* what;
    int op;  // 0 seek, 1 set loop, 2 virtualise + devirtualise
    std::uint64_t a, b;
  };
  const Case cases[] = {{"seek inside the loop", 0, 20123, 0},  {"seek before the loop", 0, 5000, 0},
                        {"seek past the loop", 0, 60000, 0},    {"another short loop", 1, 30000, 30017},
                        {"loop off", 1, 0, 0},                  {"loop start only", 1, 20100, 20500},
                        {"virtualise and return", 2, 0, 0}};
  for (const Case& c : cases) {
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset("s"), 0, src::kStartLoop);
    for (int i = 0; i < 160; ++i) d.Block(d.Stream(), {&v}, 256);  // 40960 frames: well into the repeat
    SND_REQUIRE(MaxDiff(v.out[0], 0, Model(ref.ch[0], 0, 20000, 20500, 0, v.out[0].size())) < 1e-6);
    const std::uint64_t p = v.ph.frame;
    const std::size_t before = v.out[0].size();
    std::uint64_t from = p, lb = 20000, le = 20500;
    if (c.op == 0) {
      d.Control(d.Stream(), v.slot, src::ControlOp::Seek, c.a);
      from = c.a >= 20500 ? 20000 + (c.a - 20500) % 500 : c.a;
    } else if (c.op == 1) {
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, c.a, c.b);
      lb = c.a;
      le = c.b;
      if (le != 0u && p >= le) from = lb;
    } else {
      d.Control(d.Stream(), v.slot, src::ControlOp::Virtualize);
      for (int i = 0; i < 20; ++i) {
        src::AdvanceItem it{0, v.slot, 256, 1.0f, &v.ph};
        snd::VoiceRenderStatus st{};
        d.Stream().AdvanceVirtual(&it, 1, &st);
        d.Block(d.Stream(), {}, 256);
      }
      from = v.ph.frame;
      d.Control(d.Stream(), v.slot, src::ControlOp::Devirtualize, from);
    }
    for (int i = 0; i < 160; ++i) d.Block(d.Stream(), {&v}, 256);
    const std::size_t got = v.out[0].size() - before;
    std::uint64_t end = 0;
    const double e = MaxDiff(v.out[0], before, Model(ref.ch[0], from, lb, le, 0, got, &end));
    SND_CHECK_MSG(e < 1e-6, "%s: err %g (%zu frames)", c.what, e, got);
    SND_CHECK_MSG(v.ph.frame == end, "%s: playhead %llu, next frame %llu", c.what, static_cast<unsigned long long>(v.ph.frame),
                  static_cast<unsigned long long>(end));
    SND_CHECK_MSG(got > 20000u, "%s: only %zu frames", c.what, got);
  }
}

// A finite short loop repeated from the ring, then the reads after the loop
// end fail: the voice plays every pass and what was read, then ends (no
// garbage), the IO error is counted, the file is closed once released.
SND_TEST(IndepFix4, IoErrorAfterARepeatedLoop) {
  Built b = Build("f4err", {Spec("s", 96000, "PC16", "\"loop\": {\"start\": 10000, \"end\": 10300}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "s");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  const std::string path = PathOf(b, "s");
  SND_REQUIRE(!path.empty());
  d.fs.FailReads(path, 2048u + 2u * 30000u, UINT64_MAX);  // data from frame ~30000 on fails
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("s"), 0, src::kStartLoop | (20u << 16));
  for (int i = 0; i < 1000 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::vector<float> want = Model(ref.ch[0], 0, 10000, 10300, 20, SIZE_MAX);
  const std::size_t got = v.out[0].size();
  const double e = MaxDiff(v.out[0], 0, std::vector<float>(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(
                                                                                 got < want.size() ? got : want.size())));
  // The last frames before the end may be faded (D-15): compare all but the last 240.
  const double eHead = MaxDiff(v.out[0], 0, std::vector<float>(want.begin(), want.begin() + static_cast<std::ptrdiff_t>(
                                                                                     got > 240u ? got - 240u : 0u)));
  std::printf("  played %zu frames (passes end at %llu, the error at ~%llu); err %.3g (%.3g before the fade); ioErrors %llu\n", got,
              static_cast<unsigned long long>(10000u + 20u * 300u), static_cast<unsigned long long>(10000u + 20u * 300u + 20000u), e,
              eHead, static_cast<unsigned long long>(d.runtime.Stats().streamIoErrors));
  SND_CHECK(v.ended != 0u);
  SND_CHECK(eHead < 1e-6);
  SND_CHECK(got >= 16000u && got < want.size());
  SND_CHECK(d.runtime.Stats().streamIoErrors >= 1u);
  d.Release(d.Stream(), v.slot);
  for (int i = 0; i < 6; ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
  SND_CHECK_EQ(d.pool.Used(), 0u);
}

// A finite loop at the start of the file (its start lies in the preload),
// the voice started inside the loop: after the one wrap the stream must go on
// without a stall. Today, with a loop count of 2 (one wrap), it stalls for
// ~66 blocks and time priority then skips ~16 700 frames; started at frame 0,
// or with a count of 3, it plays exactly.
SND_TEST(IndepFix4, Defect_source_FiniteLoopAtTheFileStartStallsWhenStartedInsideIt) {
  Built b = Build("f4pre", {Spec("q", 40000, "PC16", "\"loop\": {\"start\": 0, \"end\": 700}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "q");
  SND_REQUIRE(ref.ok);
  for (std::uint32_t count : {2u, 3u}) {
    for (std::uint64_t start : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{300}}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.ph.frame = start;
      v.slot = d.Start(d.Stream(), d.Asset("q"), start, src::kStartLoop | (count << 16));
      for (int i = 0; i < 1200 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
      const std::vector<float> want = Model(ref.ch[0], start, 0, 700, count, SIZE_MAX);
      const double e = MaxDiff(v.out[0], 0, want);
      const std::uint32_t stalls = d.Tables().Stream.col.stallCount[v.slot];
      std::printf("  count %u start %llu: %zu of %zu frames, %u stalls\n", count, static_cast<unsigned long long>(start),
                  v.out[0].size(), want.size(), stalls);
      SND_CHECK_MSG(v.out[0].size() == want.size() && e < 1e-6 && stalls == 0u, "count %u start %llu: %zu of %zu frames, err %g, %u stalls",
                    count, static_cast<unsigned long long>(start), v.out[0].size(), want.size(), e, stalls);
    }
  }
}
