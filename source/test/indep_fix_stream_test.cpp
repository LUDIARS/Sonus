// Independent re-check of the Stream fixes (728ea97 / 18013cb): requests
// meeting in one block (generations reqGen / execGen / adoptGen), SetLoop by
// the read plan only, a seek while a time-priority re-seek is pending, the
// boundaries of the header check (bank-format.md 6.3) and 8-channel streams
// up to maxStreams. Defect_source_* tests fail on purpose.
#include <cmath>
#include <cstdio>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"
#include "snd/format/bank_format.hpp"

using namespace indep;
namespace src = snd::source;
namespace bfm = snd::bank_format;

namespace {

AssetSpec StreamSpec(const char* name, std::uint64_t frames, const char* codec, const char* extra = "",
                     std::uint32_t channels = 1, double (*f)(std::uint32_t, std::uint64_t) = &Ramp) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(channels, 48000, frames, f);
  a.loadForm = "stream";
  a.codec = codec;
  a.extra = extra;
  return a;
}
double PerChannel(std::uint32_t c, std::uint64_t i) {
  return 0.1 + 0.08 * c + 0.05 * std::sin(6.283185307179586 * (300.0 + 70.0 * c) * double(i) / 48000.0);
}
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
  for (int i = 0; i < blocks; ++i) d.Block(d.Stream(), {&v}, 256);
}
bfm::StreamFileHeader& Header(Built& b) { return *reinterpret_cast<bfm::StreamFileHeader*>(b.files[0].second.data()); }

constexpr const char* kLoop = "\"loop\": {\"start\": 4000, \"end\": 60001}";

}  // namespace

// Seek then SetLoop in one block: the stream must play from the seek target
// under the new loop. Today SetLoop (not in place while a seek is pending)
// re-reads "from the current position" (renderPos) and replaces the target.
SND_TEST(IndepFixStream, Defect_source_SeekThenSetLoopInOneBlockLosesTheSeek) {
  Built b = Build("fst1", {StreamSpec("l", 96000, "PC16", kLoop)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "l");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("l"), 0, src::kStartLoop);
  Render(d, v, 40);
  const std::size_t before = v.out[0].size();
  d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 30000);
  d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 0, 0);
  Render(d, v, 80);
  const double e = MaxDiff(v.out[0], before, Model(ref.ch[0], 30000, 0, 0, 10000));
  SND_CHECK_MSG(e < 1e-6, "seek 30000 + loop off: err %g (first %g, want %g)", e, v.out[0][before], ref.ch[0][30000]);
}

// SetLoop while streaming, swept over new loop ends relative to the playhead
// (behind the reads, around them, far ahead), a new loop start and loop off:
// always exact under the new loop. In place (no hole) when the plan can be
// changed; the number of short blocks is printed per case.
SND_TEST(IndepFixStream, SetLoopSweepIsExact) {
  std::vector<AssetSpec> as;
  as.push_back(StreamSpec("lp", 96000, "PC16", kLoop));
  if (snd::codec::OpusAvailable()) as.push_back(StreamSpec("lo", 96000, "OPUS", kLoop, 1, &Sine1k));
  Built b = Build("fst2", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    const double tol = a.codec == "OPUS" ? 1e-4 : 1e-6;
    for (std::int64_t off : {std::int64_t{1}, std::int64_t{300}, std::int64_t{2048}, std::int64_t{6000},
                             std::int64_t{12000}, std::int64_t{20000}, std::int64_t{40000}, std::int64_t{-1},
                             std::int64_t{-2}}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, src::kStartLoop);
      Render(d, v, 40);
      const std::uint64_t p = v.ph.frame;  // 10240
      std::uint64_t lb = 2000, le = p + static_cast<std::uint64_t>(off);
      if (off == -1) {  // loop off
        lb = 0;
        le = 0;
      } else if (off == -2) {  // new loop start only
        lb = 7000;
        le = 60001;
      }
      const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, lb, le);
      Render(d, v, 150);
      // Frames until the first hole are compared directly; after a hole (re-read) the stream
      // resumes where it stopped (position kept), so the concatenation follows the model too.
      const std::vector<float> want = Model(ref.ch[0], p, lb, le, 30000);
      const double e = MaxDiff(v.out[0], before, want);
      std::printf("  %s SetLoop(%llu, %llu) at %llu: err %.3g, %u short blocks\n", a.name.c_str(),
                  static_cast<unsigned long long>(lb), static_cast<unsigned long long>(le),
                  static_cast<unsigned long long>(p), e, ShortBlocks(v, b0));
      SND_CHECK_MSG(e < tol, "%s off %lld: err %g", a.name.c_str(), static_cast<long long>(off), e);
    }
  }
}

// Seek while a time-priority re-seek is waiting (long IO hold): after the
// hold the stream plays from the seek target, every frame the material in
// order (gain in [0, 1], 1 away from the fades).
SND_TEST(IndepFixStream, SeekDuringATimePriorityReseek) {
  const std::uint64_t n = 150000;
  Built b = Build("fst3", {StreamSpec("t", n, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "t");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("t"), 0, 0);
  std::size_t at = 0;
  for (int j = 0; j < 700 && v.ended == 0u; ++j) {
    if (j == 40) d.fs.Hold(b.files[0].first);
    if (j == 190) {
      d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 20000);
      at = v.out[0].size();
    }
    if (j == 240) d.fs.Release(b.files[0].first);
    d.Block(d.Stream(), {&v}, 256);
  }
  SND_REQUIRE(v.out[0].size() > at + 10000u);
  std::size_t bad = 0, faded = 0;
  for (std::size_t i = at; i < at + 10000u; ++i) {
    const double g = v.out[0][i] / ref.ch[0][20000 + (i - at)];
    if (g < -1e-6 || g > 1.0 + 1e-6) ++bad;
    else if (g < 1.0 - 1e-6) ++faded;
  }
  SND_CHECK_MSG(bad == 0u, "%zu frames after the seek are not material[20000 + k]", bad);
  SND_CHECK(faded <= 480u);
}

// Header check boundaries (bank-format.md 6.3): dataBytes exact for PCM and
// ADPCM (one byte off is rejected), within the packet bounds for Opus (the
// built file plays; fewer bytes than 3 per packet is rejected); a file without
// its trailing padding plays to the end; a file short of the data end (frames
// missing) is an IO error.
SND_TEST(IndepFixStream, HeaderCheckBoundaries) {
  std::vector<AssetSpec> as;
  as.push_back(StreamSpec("p", 48000, "PC16"));
  as.push_back(StreamSpec("a", 48000, "ADPC"));
  if (snd::codec::OpusAvailable()) {
    as.push_back(StreamSpec("o", 48000, "OPUS", "", 1, &Sine1k));
    as.push_back(StreamSpec("o2", 48000, "OPUS", "", 2, &Sine1k));
  }
  struct Case {
    const char* what;
    void (*edit)(Built&);
    bool rejected;
  };
  const Case cases[] = {
      {"unchanged", [](Built&) {}, false},
      {"dataBytes + 1", [](Built& b) { Header(b).dataBytes += 1; }, true},
      {"dataBytes - 1", [](Built& b) { Header(b).dataBytes -= 1; }, true},
      {"no trailing padding", [](Built& b) {
         const bfm::StreamFileHeader h = Header(b);
         b.files[0].second.resize(static_cast<std::size_t>(h.dataOffset + h.dataBytes));
       }, false},
      // PCM: one byte; ADPCM / Opus: 600 bytes (more than a block / a few packets: frames are missing).
      // (An ADPCM file cut by 1 byte loses only padding of its last block and plays completely.)
      {"short of the data end", [](Built& b) {
         const bfm::StreamFileHeader h = Header(b);
         const std::uint64_t cut = h.codec == SND_CODEC_PCM_S16 ? 1u : 600u;
         b.files[0].second.resize(static_cast<std::size_t>(h.dataOffset + h.dataBytes - cut));
       }, true},
  };
  for (const AssetSpec& a : as) {
    Built one = Build(std::string("fst4") + a.name, {a});
    SND_REQUIRE_MSG(one.ok, "%s", one.diag.c_str());
    for (const Case& c : cases) {
      if (a.codec == "OPUS" && (std::string(c.what) == "dataBytes + 1" || std::string(c.what) == "dataBytes - 1")) continue;
      Built b = one;
      c.edit(b);
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.channels = a.pcm.channels;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      for (int i = 0; i < 400 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
      const bool rejected = d.runtime.Stats().streamIoErrors != 0u;
      SND_CHECK_MSG(rejected == c.rejected, "%s %s: ioErrors %llu, played %zu", a.name.c_str(), c.what,
                    static_cast<unsigned long long>(d.runtime.Stats().streamIoErrors), v.out[0].size());
      if (!c.rejected) SND_CHECK_EQ(v.out[0].size(), 48000u);
    }
    if (a.codec == "OPUS") {  // fewer than 3 bytes per packet
      Built b = one;
      Header(b).dataBytes = 2u * ((48000u + 312u + 959u) / 960u);
      IndepDriver d(b, IndepDriver::Opt{});
      IndepDriver::Voice v;
      v.channels = a.pcm.channels;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
      for (int i = 0; i < 400 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
      SND_CHECK_MSG(d.runtime.Stats().streamIoErrors != 0u, "%s: dataBytes 2 per packet accepted", a.name.c_str());
    }
  }
}

// 7: maxStreams streams of 8 channels at once (maxStreams 2), every channel exact.
SND_TEST(IndepFixStream, EightChannelStreamsUpToMaxStreams) {
  Built b = Build("fst5", {StreamSpec("e8", 30000, "PC16", "", 8, &PerChannel)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "e8");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.maxStreams = 2;
  o.poolBytes = 512u << 10;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v1, v2;
  v1.channels = v2.channels = 8;
  v1.slot = d.Start(d.Stream(), d.Asset("e8"), 0, 0);
  v2.slot = d.Start(d.Stream(), d.Asset("e8"), 5000, 0);
  SND_REQUIRE(v1.slot != snd::kInvalidSlot && v2.slot != snd::kInvalidSlot);
  for (int i = 0; i < 200 && (v1.ended == 0u || v2.ended == 0u); ++i) d.Block(d.Stream(), {&v1, &v2}, 256);
  SND_CHECK(v1.ended != 0u && v2.ended != 0u);
  double e = 0;
  for (std::uint32_t c = 0; c < 8; ++c) {
    e = std::fmax(e, MaxDiff(v1.out[c], 0, Model(ref.ch[c], 0, 0, 0, 30000)));
    e = std::fmax(e, MaxDiff(v2.out[c], 0, Model(ref.ch[c], 5000, 0, 0, 25000)));
  }
  SND_CHECK_MSG(e < 1e-6, "err %g", e);
  SND_CHECK_EQ(v1.out[0].size(), 30000u);
  SND_CHECK_EQ(v2.out[0].size(), 25000u);
}
