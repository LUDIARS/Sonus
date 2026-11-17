// Independent re-check of the sixth round (f30f790): a finite stream loop
// whose last wrap the decoder takes inside the preload before the header (and
// the read plan) arrive. Around it: every read of the file answering late by
// various amounts (latency in IO polls, not a hold), a Seek right after the
// start (same block / next block) into, before and past the loop, Opus (pre-
// skip, pre-roll at the loop start), both stall policies, and a header that
// never comes (open failure) after the preload already played.
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

AssetSpec Spec(const char* name, std::uint64_t frames, const char* codec, const char* extra,
               double (*f)(std::uint32_t, std::uint64_t) = &Ramp) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, f);
  a.loadForm = "stream";
  a.codec = codec;
  a.extra = extra;
  return a;
}
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::uint32_t count,
                         std::size_t n, std::uint64_t* end = nullptr) {
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
std::uint64_t Wrap(std::uint64_t lb, std::uint64_t le, std::uint64_t p) { return p < le ? p : lb + (p - le) % (le - lb); }

struct L {
  const char* name;
  const char* codec;
  std::uint64_t lb, le;
  const char* more;
};
const std::vector<L>& Loops() {
  static const std::vector<L> v = {
      {"a", "PC16", 0, 700, ""},
      {"b", "PC16", 3000, 3001, ""},
      {"c", "ADPC", 100, 4000, ""},                               // across the preload end (~4800)? inside
      {"d", "PC16", 4000, 6000, ""},                              // across the preload end
      {"s", "PC16", 0, 700, ", \"stallPolicy\": \"sound\""},     // sound priority
      {"o", "OPUS", 0, 960, ""},                                  // Opus: pre-skip, pre-roll at the loop start
      {"p", "OPUS", 2000, 2961, ""},
  };
  return v;
}
Built BuildAll(const char* tag) {
  std::vector<AssetSpec> as;
  for (const L& l : Loops()) {
    if (std::string(l.codec) == "OPUS" && !snd::codec::OpusAvailable()) continue;
    char buf[160];
    std::snprintf(buf, sizeof buf, "\"loop\": {\"start\": %llu, \"end\": %llu}%s", static_cast<unsigned long long>(l.lb),
                  static_cast<unsigned long long>(l.le), l.more);
    as.push_back(Spec(l.name, 30000, l.codec, buf, std::string(l.codec) == "OPUS" ? &Sine1k : &Ramp));
  }
  return Build(tag, as);
}
double Tol(const L& l) { return std::string(l.codec) == "OPUS" ? 1e-4 : 1e-6; }

}  // namespace

// Every read answering after `lat` IO polls (0 .. 40), counts 2 and 3, starts
// before, inside and past the loop: exact, no stall, ends after the last pass.
SND_TEST(IndepFix5, LateFileWithFiniteLoopsInThePreload) {
  Built b = BuildAll("f5late");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::uint32_t runs = 0, bad = 0;
  for (const L& l : Loops()) {
    if (std::string(l.codec) == "OPUS" && !snd::codec::OpusAvailable()) continue;
    const Decoded ref = Reference(b, l.name);
    SND_REQUIRE(ref.ok);
    const std::string path = PathOf(b, l.name);
    for (std::uint32_t lat : {0u, 1u, 3u, 10u, 40u}) {
      for (std::uint32_t count : {2u, 3u}) {
        for (std::uint64_t start : {std::uint64_t{0}, l.lb + (l.le - l.lb) / 2u, l.le + 7u}) {
          IndepDriver d(b, IndepDriver::Opt{});
          SND_REQUIRE(d.Ok());
          d.fs.SetLatency(path, lat);
          IndepDriver::Voice v;
          v.ph.frame = start;
          v.slot = d.Start(d.Stream(), d.Asset(l.name), start, src::kStartLoop | (count << 16));
          SND_REQUIRE(v.slot != snd::kInvalidSlot);
          for (int i = 0; i < 1500 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
          const std::uint64_t s0 = Wrap(l.lb, l.le, start);
          const std::vector<float> want = Model(ref.ch[0], s0, l.lb, l.le, count, SIZE_MAX);
          const std::size_t got = v.out.empty() ? 0u : v.out[0].size();
          const double e = got == 0u ? 1e9 : MaxDiff(v.out[0], 0, want);
          const std::uint32_t stalls = d.Tables().Stream.col.stallCount[v.slot];
          ++runs;
          if (v.ended == 0u || got != want.size() || e >= Tol(l) || stalls != 0u) {
            ++bad;
            SND_CHECK_MSG(false, "%s latency %u count %u start %llu: ended %u, %zu of %zu frames, err %g, %u stalls", l.name, lat,
                          count, static_cast<unsigned long long>(start), v.ended, got, want.size(), e, stalls);
          }
        }
      }
    }
  }
  std::printf("  %u runs, %u wrong\n", runs, bad);
}

// A Seek right after the start (in the same block as Start, or one block
// later) with a finite loop in the preload and a late file: into the loop,
// before it and past it (wrapped). The voice plays from the target, every
// remaining pass, the material after it, and ends; the playhead names the end.
SND_TEST(IndepFix5, SeekRightAfterTheStart) {
  Built b = BuildAll("f5seek");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::uint32_t runs = 0;
  for (const L& l : Loops()) {
    if (std::string(l.codec) == "OPUS" && !snd::codec::OpusAvailable()) continue;
    const Decoded ref = Reference(b, l.name);
    SND_REQUIRE(ref.ok);
    const std::string path = PathOf(b, l.name);
    for (std::uint32_t lat : {0u, 10u}) {
      for (int when = 0; when < 2; ++when) {
        for (std::uint64_t target : {l.lb + (l.le - l.lb) / 3u, l.lb >= 50u ? l.lb - 50u : 0u, l.le + 11u}) {
          IndepDriver d(b, IndepDriver::Opt{});
          SND_REQUIRE(d.Ok());
          d.fs.SetLatency(path, lat);
          IndepDriver::Voice v;
          v.slot = d.Start(d.Stream(), d.Asset(l.name), 0, src::kStartLoop | (3u << 16));
          std::size_t before = 0;
          if (when == 1) {
            d.Block(d.Stream(), {&v}, 256);
            before = v.out[0].size();
          }
          // The pass the voice is in when the seek comes: 0 (no wrap within one block for these loops,
          // except the one-frame loop b, which wraps at once: skip the next-block case for it).
          if (when == 1 && l.le - l.lb < 256u && l.lb < 256u) continue;
          if (when == 1 && l.le - l.lb < 256u) continue;
          d.Control(d.Stream(), v.slot, src::ControlOp::Seek, target);
          for (int i = 0; i < 1500 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
          const std::uint64_t t0 = Wrap(l.lb, l.le, target);
          std::uint64_t end = 0;
          const std::vector<float> want = Model(ref.ch[0], t0, l.lb, l.le, 3u, SIZE_MAX, &end);
          const std::size_t got = v.out.empty() ? 0u : v.out[0].size() - before;
          const double e = MaxDiff(v.out[0], before, want);
          ++runs;
          SND_CHECK_MSG(v.ended != 0u && got == want.size() && e < Tol(l) && v.ph.frame == end,
                        "%s latency %u %s seek %llu: ended %u, %zu of %zu frames, err %g, playhead %llu (end %llu)", l.name, lat,
                        when == 0 ? "same block" : "next block", static_cast<unsigned long long>(target), v.ended, got,
                        want.size(), e, static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
        }
      }
    }
  }
  std::printf("  %u runs\n", runs);
}

// The stream file cannot be opened: the preload plays (with its loop passes)
// and the voice ends with an IO error, without stalling forever; nothing
// stays open or borrowed.
SND_TEST(IndepFix5, HeaderNeverComesAfterThePreloadLoop) {
  Built b = BuildAll("f5fail");
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const char* name : {"a", "d"}) {
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    d.fs.FailOpen(PathOf(b, name));
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset(name), 0, src::kStartLoop | (3u << 16));
    int blocks = 0;
    for (; blocks < 600 && v.ended == 0u; ++blocks) d.Block(d.Stream(), {&v}, 256);
    std::printf("  %s: ended after %d blocks, %zu frames, ioErrors %llu\n", name, blocks, v.out.empty() ? 0u : v.out[0].size(),
                static_cast<unsigned long long>(d.runtime.Stats().streamIoErrors));
    SND_CHECK(v.ended != 0u);
    SND_CHECK(d.runtime.Stats().streamIoErrors >= 1u);
    d.Release(d.Stream(), v.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
    SND_CHECK_EQ(d.pool.Used(), 0u);
  }
}
