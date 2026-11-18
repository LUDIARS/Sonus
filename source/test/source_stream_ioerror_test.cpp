// Stream IO errors (seventh round, decision): after an IO error no new read is
// posted, and everything already in memory (the preload, the records delivered
// before the failed one, the decoded PCM, loop passes served by it) plays out
// in order; then the voice ends (faded). Loops inside the preload, across its
// end and after it; PCM, ADPCM, Opus; reads failing from a byte on, and the
// stream file failing to open (only the preload exists).
//
// Model: play the material in order (loops included); a frame is available if
// it lies in the preload, or if all the bytes it needs lie before the failing
// byte F. The played length lies between the model with F - chunkBytes (a read
// overlapping F fails as a whole) and the model with F; for a failing open it
// is exactly the preload model. Every frame played (but the last fade) equals
// the playback without an error.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"
#include "snd/format/bank_format.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

constexpr std::uint64_t kChunk = 4096;   // IndepDriver::Opt::chunkBytes
constexpr std::size_t kFade = 240;       // 5 ms at 48 kHz (the end is faded)

std::string PathOf(const Built& b, const std::string& name) {
  for (const auto& f : b.files) {
    if (f.first.find("/" + name + "_") != std::string::npos) return f.first;
  }
  return std::string();
}
std::uint64_t DataOffset(const Built& b, const std::string& path) {
  for (const auto& f : b.files) {
    if (f.first == path && f.second.size() >= sizeof(snd::bank_format::StreamFileHeader)) {
      snd::bank_format::StreamFileHeader h{};
      std::memcpy(&h, f.second.data(), sizeof(h));
      return h.dataOffset;
    }
  }
  return 0;
}
// Frames played in material order from `start` under [lb, le) x count (0 =
// infinite) while avail(frame); at most `cap`.
std::size_t ModelLength(std::uint64_t total, std::uint64_t start, std::uint64_t lb, std::uint64_t le, std::uint32_t count,
                        const std::function<bool(std::uint64_t)>& avail, std::size_t cap) {
  std::size_t n = 0;
  std::uint32_t done = 0;
  std::uint64_t p = start;
  while (n < cap && p < total && avail(p)) {
    ++n;
    if (++p == le && le > lb && (count == 0u || done + 1u < count)) {
      p = lb;
      ++done;
    }
  }
  return n;
}

struct Run {
  std::vector<float> out;
  std::uint32_t ended = 0;
  std::uint64_t ioErrors = 0;
  std::uint32_t openFiles = 0, poolUsed = 0;
};
Run Play(const Built& b, const std::string& name, std::uint32_t flags, const std::function<void(IndepDriver&)>& inject,
         std::size_t cap) {
  Run r;
  IndepDriver d(b, IndepDriver::Opt{});
  if (!d.Ok()) return r;
  inject(d);
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset(name), 0, flags);
  if (v.slot == snd::kInvalidSlot) return r;
  for (int i = 0; i < 1500 && v.ended == 0u && (v.out.empty() || v.out[0].size() < cap); ++i) d.Block(d.Stream(), {&v}, 256);
  if (!v.out.empty()) r.out = v.out[0];
  r.ended = v.ended;
  r.ioErrors = d.runtime.Stats().streamIoErrors;
  d.Release(d.Stream(), v.slot);
  for (int i = 0; i < 6; ++i) d.Block(d.Stream(), {}, 256);
  r.openFiles = d.fs.OpenFiles();
  r.poolUsed = static_cast<std::uint32_t>(d.pool.Used());
  return r;
}
double PrefixDiff(const std::vector<float>& got, const std::vector<float>& ref, std::size_t n) {
  double m = 0;
  for (std::size_t i = 0; i < n; ++i) m = std::fmax(m, i < ref.size() ? std::fabs(got[i] - ref[i]) : 1e9);
  return m;
}

}  // namespace

SND_TEST(SourceStreamIoError, MemoryPlaysOutThenTheVoiceEnds) {
  struct L {
    const char* name;
    const char* codec;
    std::uint64_t lb, le;  // le == 0: no loop
    std::uint32_t count;
  };
  // The preload is 200 ms = 9600 frames (bank default).
  const std::vector<L> loops = {
      {"pn", "PC16", 0, 0, 0},           {"pa", "PC16", 0, 700, 3},          {"pb", "PC16", 20000, 21000, 3},
      {"pc", "PC16", 9100, 10100, 4},    {"pd", "PC16", 20000, 40000, 0},    {"pe", "PC16", 9000, 12000, 0},
      {"an", "ADPC", 0, 0, 0},           {"aa", "ADPC", 0, 1500, 3},         {"ab", "ADPC", 20000, 21000, 3},
      {"ac", "ADPC", 9100, 10100, 4},    {"ad", "ADPC", 20000, 40000, 0},
      {"on", "OPUS", 0, 0, 0},           {"oa", "OPUS", 0, 960, 3},          {"ob", "OPUS", 20000, 21000, 3},
      {"oc", "OPUS", 9100, 10100, 4},
  };
  std::vector<AssetSpec> as;
  for (const L& l : loops) {
    if (std::string(l.codec) == "OPUS" && !snd::codec::OpusAvailable()) continue;
    AssetSpec a;
    a.name = l.name;
    a.pcm = Signal(1, 48000, 60000, std::string(l.codec) == "OPUS" ? &Sine1k : &Ramp);
    a.loadForm = "stream";
    a.codec = l.codec;
    if (l.le != 0u) {
      a.extra = "\"loop\": {\"start\": " + std::to_string(l.lb) + ", \"end\": " + std::to_string(l.le) + "}";
    }
    as.push_back(a);
  }
  Built b = Build("sioerr", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::uint32_t cases = 0;
  for (const L& l : loops) {
    const std::string codec = l.codec;
    if (codec == "OPUS" && !snd::codec::OpusAvailable()) continue;
    const std::string path = PathOf(b, l.name);
    SND_REQUIRE(!path.empty());
    const std::uint64_t dataOff = DataOffset(b, path);
    SND_REQUIRE(dataOff != 0u);
    IndepDriver probe(b, IndepDriver::Opt{});
    SND_REQUIRE(probe.Ok());
    const src::SourceAsset& asset = *probe.Asset(l.name);
    const std::uint64_t total = asset.codec.total_frames;
    const std::uint64_t pre = asset.preloadFrames;
    const std::uint32_t flags = l.le != 0u ? (src::kStartLoop | (l.count << 16)) : 0u;
    // Data byte just past what frame f needs (PCM: its sample; ADPCM: its block).
    auto needs = [&](std::uint64_t f) -> std::uint64_t {
      if (codec == "PC16") return (f + 1u) * 2u;
      const std::uint64_t fpp = 1017, bpp = 512;  // ADPCM mono default block
      return ((f + asset.codec.pre_skip) / fpp + 1u) * bpp;
    };
    const std::size_t cap = 300000;  // an infinite loop with all its data in memory plays on: compared up to here
    const Run ok = Play(b, l.name, flags, [](IndepDriver&) {}, cap);
    SND_REQUIRE(!ok.out.empty());
    const double tol = codec == "OPUS" ? 1e-6 : 1e-7;
    // Reads from a data byte on fail (three points: in the loop, past it, just after the preload).
    for (std::uint64_t failFrame : {pre + 3000u, std::uint64_t{25000}, std::uint64_t{45000}}) {
      const std::uint64_t F = codec == "OPUS" ? 0u : needs(failFrame);  // data bytes
      std::uint64_t Fop = 0;  // Opus: the seek point at or after failFrame
      if (codec == "OPUS") {
        for (std::uint32_t i = 0; i < asset.seekCount; ++i) {
          if (asset.seekFrames[i] >= failFrame) {
            Fop = asset.seekOffsets[i];
            break;
          }
        }
        if (Fop == 0u) continue;
      }
      const std::uint64_t failByte = dataOff + (codec == "OPUS" ? Fop : F);
      const Run er = Play(b, l.name, flags, [&](IndepDriver& d) { d.fs.FailReads(path, failByte, UINT64_MAX); }, cap);
      ++cases;
      const std::size_t got = er.out.size() < cap ? er.out.size() : cap;
      std::size_t lo = 0, hi = cap;
      if (codec == "OPUS") {
        lo = ModelLength(total, 0, l.lb, l.le, l.count, [&](std::uint64_t f) { return f < pre; }, cap);
      } else {
        const std::uint64_t Flo = F > kChunk ? F - kChunk : 0u;
        lo = ModelLength(total, 0, l.lb, l.le, l.count, [&](std::uint64_t f) { return f < pre || needs(f) <= Flo; }, cap);
        hi = ModelLength(total, 0, l.lb, l.le, l.count, [&](std::uint64_t f) { return f < pre || needs(f) <= F; }, cap);
      }
      const bool endsByError = hi < cap || (codec == "OPUS" && got < cap);
      const double e = PrefixDiff(er.out, ok.out, got > kFade ? got - kFade : 0u);
      std::printf("  %s fail at frame %llu: %zu frames (model %zu .. %zu, no error %zu), err %.3g, ended %u\n", l.name,
                  static_cast<unsigned long long>(failFrame), got, lo, hi, ok.out.size(), e, er.ended);
      SND_CHECK_MSG(e <= tol, "%s fail %llu: wrong frames (err %g)", l.name, static_cast<unsigned long long>(failFrame), e);
      SND_CHECK_MSG(got >= lo && got <= hi, "%s fail %llu: %zu frames, model %zu .. %zu", l.name,
                    static_cast<unsigned long long>(failFrame), got, lo, hi);
      if (endsByError) {
        SND_CHECK_MSG(er.ended == 1u, "%s fail %llu: not ended", l.name, static_cast<unsigned long long>(failFrame));
        SND_CHECK(er.ioErrors >= 1u);
      }
      SND_CHECK_EQ(er.openFiles, 0u);
      SND_CHECK_EQ(er.poolUsed, 0u);
    }
    // The stream file does not open: exactly the preload plays (in material order), then the end.
    {
      const Run er = Play(b, l.name, flags, [&](IndepDriver& d) { d.fs.FailOpen(path); }, cap);
      ++cases;
      const std::size_t want = ModelLength(total, 0, l.lb, l.le, l.count, [&](std::uint64_t f) { return f < pre; }, cap);
      const std::size_t got = er.out.size() < cap ? er.out.size() : cap;
      const double e = PrefixDiff(er.out, ok.out, got > kFade ? got - kFade : 0u);
      std::printf("  %s open fails: %zu frames (preload model %zu), err %.3g, ended %u\n", l.name, got, want, e, er.ended);
      if (want < cap) {
        SND_CHECK_MSG(got == want && e <= tol, "%s open fails: %zu frames (model %zu), err %g", l.name, got, want, e);
        SND_CHECK_EQ(er.ended, 1u);
        SND_CHECK(er.ioErrors >= 1u);
      } else {  // an infinite loop inside the preload needs no read: it plays on
        SND_CHECK(got >= cap && e <= tol);
      }
      SND_CHECK_EQ(er.openFiles, 0u);
      SND_CHECK_EQ(er.poolUsed, 0u);
    }
  }
  std::printf("  %u cases\n", cases);
}
