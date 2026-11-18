// Stream loops of any length (fifth round, decision 4): a loop no longer than
// the PCM ring is decoded once and repeated from memory; a longer one wraps in
// the IO. Exact frames and playhead at the ring boundary, across ADPCM blocks
// and Opus packets, with finite loop counts (the material goes on after the
// last pass) and with the IO stopped while an infinite short loop repeats.
// Drives the Stream kind directly (indep_driver.hpp: ring of 16384 frames).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

constexpr std::uint64_t kRing = 16384;  // NextPow2(200 ms at 48 kHz + 4 blocks of 256)

AssetSpec Spec(const char* name, std::uint64_t frames, const char* codec, const char* extra = "",
               double (*f)(std::uint32_t, std::uint64_t) = &Ramp) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, f);
  a.loadForm = "stream";
  a.codec = codec;
  a.extra = extra;
  return a;
}
// Material frames from p under loop [lb, le) played `count` times in total (0 = infinite).
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::uint32_t count,
                         std::size_t n, std::uint64_t* end = nullptr, std::uint32_t* loops = nullptr) {
  std::vector<float> v;
  std::uint32_t done = 0;
  while (v.size() < n && p < x.size()) {
    v.push_back(x[static_cast<std::size_t>(p)]);
    if (++p == le && le > lb && (count == 0u || done + 1u < count)) {
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
double Tol(const std::string& codec) { return codec == "OPUS" ? 2e-3 : 1e-6; }

}  // namespace

// SetLoop behind the playhead (the voice goes to the loop start) with lengths
// around the ring size and around an ADPCM block (1017 frames), loop starts on
// and off block / packet boundaries: every frame and the playhead exact.
SND_TEST(SourceStreamLoop, AnyLengthAroundTheRingAndCodecBlocks) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pc", 120000, "PC16"));
  as.push_back(Spec("ad", 120000, "ADPC"));
  if (snd::codec::OpusAvailable()) as.push_back(Spec("op", 120000, "OPUS", "", &Sine1k));
  Built b = Build("sloops", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  struct Case {
    std::uint64_t lb, len;
  };
  const std::vector<Case> cases = {
      {9000, 1},        {9000, 7},          {1017 * 9 - 3, 7},  {1017 * 9, 1017},   {1017 * 9 - 1, 1018},
      {9100, 1016},     {9000, kRing - 1},  {9000, kRing},      {9000, kRing + 1},  {9000, kRing + 1017},
      {9000, 40000},    {960 * 10 - 5, 10},  // across an Opus packet (960 frames)
  };
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    for (const Case& c : cases) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      for (int i = 0; i < 120; ++i) d.Block(d.Stream(), {&v}, 256);  // playhead 30720: past every loop end below 30720
      const std::size_t before = v.out[0].size();
      const std::uint64_t le = c.lb + c.len;
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, c.lb, le);
      const int blocks = c.len > 30000 ? 400 : 260;
      for (int i = 0; i < blocks; ++i) d.Block(d.Stream(), {&v}, 256);
      const std::uint64_t from = before < le ? before : c.lb;
      const std::size_t got = v.out[0].size() - before;
      std::uint64_t end = 0;
      std::uint32_t loops = 0;
      const std::vector<float> m = Model(ref.ch[0], from, c.lb, le, 0, got, &end, &loops);
      const double e = MaxDiff(v.out[0], before, m);
      SND_CHECK_MSG(e < Tol(a.codec), "%s loop %llu+%llu: err %g", a.name.c_str(), static_cast<unsigned long long>(c.lb),
                    static_cast<unsigned long long>(c.len), e);
      SND_CHECK_MSG(got > 50000u, "%s loop %llu+%llu: %zu frames", a.name.c_str(), static_cast<unsigned long long>(c.lb),
                    static_cast<unsigned long long>(c.len), got);
      SND_CHECK_MSG(v.ph.frame == end, "%s loop %llu+%llu: playhead %llu, next frame %llu", a.name.c_str(),
                    static_cast<unsigned long long>(c.lb), static_cast<unsigned long long>(c.len),
                    static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
      SND_CHECK_EQ(v.ended, 0u);
    }
  }
}

// A short loop from the asset (Start with kStartLoop) and a finite loop count:
// the passes, then the material after the loop end to the end, exactly; the
// voice ends. Loops short enough to be repeated from memory and longer ones.
SND_TEST(SourceStreamLoop, FiniteCountsGoOnAfterTheLastPass) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("p1", 30000, "PC16", "\"loop\": {\"start\": 2000, \"end\": 2003}"));
  as.push_back(Spec("a1", 30000, "ADPC", "\"loop\": {\"start\": 4000, \"end\": 5100}"));
  as.push_back(Spec("p2", 60000, "PC16", "\"loop\": {\"start\": 2000, \"end\": 22000}"));
  as.push_back(Spec("p3", 30000, "PC16", "\"loop\": {\"start\": 29000, \"end\": 30000}"));  // ends at the end
  Built b = Build("sloopn", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    std::uint64_t lb = 0, le = 0;
    SND_REQUIRE(std::sscanf(a.extra.c_str(), "\"loop\": {\"start\": %llu, \"end\": %llu}",
                            reinterpret_cast<unsigned long long*>(&lb), reinterpret_cast<unsigned long long*>(&le)) == 2);
    for (std::uint32_t count : {2u, 5u, le - lb < 5000u ? 300u : 12u}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, src::kStartLoop | (count << 16));
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      std::uint64_t end = 0;
      std::uint32_t loops = 0;
      const std::vector<float> m = Model(ref.ch[0], 0, lb, le, count, SIZE_MAX, &end, &loops);
      const std::size_t blocks = m.size() / 256u + 200u;
      for (std::size_t i = 0; i < blocks && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
      const double e = MaxDiff(v.out[0], 0, m);
      std::printf("  %s x%u: %zu frames (model %zu), err %g, ended %u\n", a.name.c_str(), count, v.out[0].size(), m.size(), e,
                  v.ended);
      SND_CHECK_MSG(v.out[0].size() == m.size() && e < 1e-6, "%s x%u: %zu frames (model %zu), err %g", a.name.c_str(), count,
                    v.out[0].size(), m.size(), e);
      SND_CHECK_EQ(v.ended, 1u);
      SND_CHECK_EQ(v.ph.loopCount, loops);
    }
  }
}

// An infinite loop repeated from memory reads nothing more: the IO posts stop
// once the pass is in the ring. A seek (its target wrapped into the loop) reads
// the loop again; a longer loop set while it repeats, and the loop switched
// off, go on from the position exactly.
SND_TEST(SourceStreamLoop, InfiniteShortLoopStopsTheIoAndASeekLeavesIt) {
  Built b = Build("sloopio", {Spec("s", 96000, "PC16", "\"loop\": {\"start\": 3000, \"end\": 3100}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "s");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("s"), 0, src::kStartLoop);
  for (int i = 0; i < 100; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::uint64_t posted = d.runtime.Stats().ioPosted;
  for (int i = 0; i < 400; ++i) d.Block(d.Stream(), {&v}, 256);
  SND_CHECK_MSG(d.runtime.Stats().ioPosted == posted, "reads posted while the loop repeats: %llu",
                static_cast<unsigned long long>(d.runtime.Stats().ioPosted - posted));
  std::uint64_t end = 0;
  SND_CHECK(MaxDiff(v.out[0], 0, Model(ref.ch[0], 0, 3000, 3100, 0, v.out[0].size(), &end)) < 1e-6);
  SND_CHECK_EQ(v.ph.frame, end);
  // Seek past the loop end: the target is wrapped into the loop (3000 + 46900 % 100).
  const std::size_t before = v.out[0].size();
  d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 50037);
  for (int i = 0; i < 300; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::size_t got = v.out[0].size() - before;
  SND_CHECK_MSG(MaxDiff(v.out[0], before, Model(ref.ch[0], 3037, 3000, 3100, 0, got, &end)) < 1e-6, "after the seek");
  SND_CHECK_EQ(v.ph.frame, end);
  // Seek back into the loop, and a new (longer) loop set while it repeats.
  const std::size_t b2 = v.out[0].size();
  d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 3050);
  for (int i = 0; i < 100; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::size_t b3 = v.out[0].size();
  SND_CHECK(MaxDiff(v.out[0], b2, Model(ref.ch[0], 3050, 3000, 3100, 0, b3 - b2, &end)) < 1e-6);
  d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 3000, 3333);
  for (int i = 0; i < 200; ++i) d.Block(d.Stream(), {&v}, 256);
  // The new loop starts from the position the voice had (inside both loops).
  const std::size_t got3 = v.out[0].size() - b3;
  SND_CHECK_MSG(MaxDiff(v.out[0], b3, Model(ref.ch[0], end, 3000, 3333, 0, got3, &end)) < 1e-6, "after SetLoop");
  SND_CHECK_EQ(v.ph.frame, end);
  SND_CHECK_EQ(v.ended, 0u);
  // Loop off while it repeats: from the position to the material end, then ended.
  const std::size_t b4 = v.out[0].size();
  d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 0, 0);
  for (int i = 0; i < 600 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::vector<float> rest = Model(ref.ch[0], end, 0, 0, 0, SIZE_MAX);
  SND_CHECK_MSG(v.out[0].size() - b4 == rest.size() && MaxDiff(v.out[0], b4, rest) < 1e-6, "loop off: %zu frames (model %zu)",
                v.out[0].size() - b4, rest.size());
  SND_CHECK_EQ(v.ended, 1u);
}

// Loops inside the preload, across its end and after it (and without a
// preload), every start (before, at, inside, past the loop end: wrapped into
// it), loop counts 2, 3 and infinite, with the stream file answering at once
// or held for a few blocks (the header comes after the decoder took its wraps
// in the preload). Exact frames, no stall, the voice ends after the last pass.
SND_TEST(SourceStreamLoop, PreloadLoopsStartsAndCounts) {
  struct L {
    const char* name;
    const char* codec;
    std::uint64_t lb, le;
    bool preload;
  };
  const std::vector<L> loops = {
      {"a", "PC16", 0, 700, true},       {"b", "PC16", 5000, 5003, true},    {"c", "PC16", 9000, 12000, true},
      {"d", "PC16", 20000, 20700, true}, {"e", "ADPC", 0, 1500, true},       {"f", "ADPC", 4000, 4001, true},
      {"g", "PC16", 0, 700, false},      {"h", "PC16", 20000, 22000, false}, {"i", "PC16", 3000, 40000, true},
  };
  std::vector<AssetSpec> as;
  std::vector<std::string> extras;
  for (const L& l : loops) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "\"loop\": {\"start\": %llu, \"end\": %llu}%s", static_cast<unsigned long long>(l.lb),
                  static_cast<unsigned long long>(l.le), l.preload ? "" : ", \"preloadMs\": 0");
    extras.emplace_back(buf);
  }
  for (std::size_t i = 0; i < loops.size(); ++i) as.push_back(Spec(loops[i].name, 40000, loops[i].codec, extras[i].c_str()));
  Built b = Build("sloopp", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::uint32_t runs = 0;
  for (const L& l : loops) {
    const Decoded ref = Reference(b, l.name);
    SND_REQUIRE(ref.ok);
    std::string path;
    for (const auto& f : b.files) {
      if (f.first.find(std::string("/") + l.name + "_") != std::string::npos) path = f.first;
    }
    SND_REQUIRE(!path.empty());
    const std::uint64_t len = l.le - l.lb;
    const std::vector<std::uint64_t> starts = {0, l.lb, l.lb + len / 2, l.le - 1, l.le + 37, l.lb >= 1000 ? l.lb - 1000 : 1};
    for (std::uint64_t start : starts) {
      if (start >= 40000) continue;
      for (std::uint32_t count : {2u, 3u, 0u}) {
        for (int hold : {0, 4}) {
          IndepDriver d(b, IndepDriver::Opt{});
          SND_REQUIRE(d.Ok());
          if (hold != 0) d.fs.Hold(path);
          IndepDriver::Voice v;
          const std::uint64_t from = start >= l.le ? l.lb + (start - l.le) % len : start;
          v.ph.frame = start;
          v.slot = d.Start(d.Stream(), d.Asset(l.name), start, src::kStartLoop | (count << 16));
          SND_REQUIRE(v.slot != snd::kInvalidSlot);
          std::uint64_t end = 0;
          std::uint32_t done = 0;
          const std::size_t cap = count == 0u ? 60000u : SIZE_MAX;
          const std::vector<float> m = Model(ref.ch[0], from, l.lb, l.le, count, cap, &end, &done);
          const std::size_t blocks = m.size() / 256u + 200u;
          for (std::size_t i = 0; i < blocks && v.ended == 0u && (v.out.empty() || v.out[0].size() < cap); ++i) {
            if (hold != 0 && i == static_cast<std::size_t>(hold)) d.fs.Release(path);
            d.Block(d.Stream(), {&v}, 256);
          }
          SND_REQUIRE(!v.out.empty());
          const std::size_t got = count == 0u && v.out[0].size() > cap ? cap : v.out[0].size();
          const double e = MaxDiff(v.out[0], 0, m);
          const std::uint32_t stalls = d.Tables().Stream.col.stallCount[v.slot];
          ++runs;
          SND_CHECK_MSG(got == m.size() && e < 1e-6 && stalls == 0u,
                        "%s loop %llu..%llu start %llu count %u hold %d: %zu of %zu frames, err %g, %u stalls", l.name,
                        static_cast<unsigned long long>(l.lb), static_cast<unsigned long long>(l.le),
                        static_cast<unsigned long long>(start), count, hold, got, m.size(), e, stalls);
          if (count != 0u) {
            SND_CHECK_EQ(v.ended, 1u);
            SND_CHECK_EQ(v.ph.loopCount, done);
          }
        }
      }
    }
  }
  std::printf("  %u runs\n", runs);
}
