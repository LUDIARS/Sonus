// Independent tests of the Stream kind (written from the spec): the stream
// file header check (bank-format.md 6.3), underrun policies checked frame by
// frame against the material and the clock, SetLoop / seek continuity, IO held
// in flight at release, the Stream columns (D-45), priming, and the capacity
// promised by maxStreams for multichannel streams.
// Driven through ISourceKind (indep_driver.hpp; IO by IoScheduler::RunOnce
// over the memory FS before each block). Defect_source_* tests fail on purpose.
#include <cmath>
#include <cstdio>
#include <cstring>
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
bfm::StreamFileHeader& Header(Built& b, std::size_t i) {
  return *reinterpret_cast<bfm::StreamFileHeader*>(b.files[i].second.data());
}

}  // namespace

// C2: the 64-byte header must match the bank (bank-format.md 6.3: assetId =
// ASST.id, codec, channels, sampleRate, totalFrames, dataOffset = STRM.dataOffset,
// dataBytes = the encoded bytes). A mismatching header is an IO error
// (streamIoErrors, the voice ends after what is buffered) and the data is not
// played as if it were right. The unmodified file plays to the end.
SND_TEST(IndepStream, HeaderMismatchesAreRejected) {
  const std::uint64_t n = 96000;
  Built good = Build("ihdr", {StreamSpec("h", n, "PC16")});
  SND_REQUIRE_MSG(good.ok, "%s", good.diag.c_str());
  SND_REQUIRE_EQ(good.files.size(), 1u);
  struct Case {
    const char* name;
    void (*edit)(Built&);
    bool rejected;  // what the spec requires
  };
  const Case cases[] = {
      {"unchanged", [](Built&) {}, false},
      {"magic", [](Built& b) { Header(b, 0).magic ^= 1u; }, true},
      {"version", [](Built& b) { Header(b, 0).version = 2; }, true},
      {"codec", [](Built& b) { Header(b, 0).codec = SND_CODEC_PCM_F32; }, true},
      {"channels", [](Built& b) { Header(b, 0).channels = 2; }, true},
      {"totalFrames", [](Built& b) { Header(b, 0).totalFrames -= 1; }, true},
      {"dataOffset", [](Built& b) { Header(b, 0).dataOffset += 2048; }, true},
      {"short header", [](Built& b) { b.files[0].second.resize(40); }, true},
  };
  for (const Case& c : cases) {
    Built b = good;
    c.edit(b);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset("h"), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    for (int i = 0; i < 600 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
    const auto s = d.runtime.Stats();
    const bool rejected = s.streamIoErrors != 0u;
    SND_CHECK(v.ended != 0u);
    SND_CHECK_MSG(rejected == c.rejected, "%s: ioErrors %llu, played %zu of %llu", c.name,
                  static_cast<unsigned long long>(s.streamIoErrors), v.out[0].size(), static_cast<unsigned long long>(n));
    if (!c.rejected) SND_CHECK_EQ(v.out[0].size(), n);
    if (c.rejected) SND_CHECK(v.out[0].size() < n);
    d.Release(d.Stream(), v.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
    SND_CHECK_EQ(d.pool.Used(), 0u);
  }
}

// Same check for the header fields the implementation does not compare today
// (sampleRate, assetId) and for a dataBytes / file length that cannot hold
// totalFrames of PC16: the stream silently plays something else (or less) and
// ends as if complete, no IO error.
SND_TEST(IndepStream, Defect_source_HeaderSampleRateAssetIdAndSizeNotChecked) {
  const std::uint64_t n = 96000;
  Built good = Build("ihdr2", {StreamSpec("h", n, "PC16")});
  SND_REQUIRE_MSG(good.ok, "%s", good.diag.c_str());
  struct Case {
    const char* name;
    void (*edit)(Built&);
  };
  const Case cases[] = {
      {"sampleRate 44100", [](Built& b) { Header(b, 0).sampleRate = 44100; }},
      {"assetId", [](Built& b) { Header(b, 0).assetId ^= 0x5555u; }},
      {"dataBytes half", [](Built& b) { Header(b, 0).dataBytes /= 2; }},
      {"file cut in half", [](Built& b) {
         bfm::StreamFileHeader& h = Header(b, 0);
         b.files[0].second.resize(static_cast<std::size_t>(h.dataOffset + h.dataBytes / 2));
       }},
  };
  for (const Case& c : cases) {
    Built b = good;
    c.edit(b);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset("h"), 0, 0);
    SND_REQUIRE(v.slot != snd::kInvalidSlot);
    for (int i = 0; i < 600 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
    const auto s = d.runtime.Stats();
    SND_CHECK_MSG(s.streamIoErrors != 0u, "%s: accepted (played %zu of %llu frames, no IO error)", c.name,
                  v.out[0].size(), static_cast<unsigned long long>(n));
  }
}

// C4: sound priority. The file is held (reads pending) long enough for the PCM
// ring to run dry, then released. Frame k of the output must be material frame
// k (nothing lost, nothing repeated), with a gain in [0, 1] and exactly 1 away
// from the short fades around the stall; all frames are played; stallCount and
// the starved status are reported.
SND_TEST(IndepStream, SoundPriorityStallLosesNoFrame) {
  const std::uint64_t n = 150000;
  Built b = Build("isnd", {StreamSpec("s", n, "PC16", "\"stallPolicy\": \"sound\"")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "s");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("s"), 0, 0);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 40; ++i) d.Block(d.Stream(), {&v}, 256);
  d.fs.Hold(b.files[0].first);
  for (int i = 0; i < 150; ++i) d.Block(d.Stream(), {&v}, 256);
  d.fs.Release(b.files[0].first);
  for (int i = 0; i < 2000 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
  SND_CHECK(v.ended != 0u);
  SND_CHECK(v.starved > 10u);
  SND_CHECK_EQ(v.out[0].size(), n);
  const std::uint32_t stalls = d.Tables().Stream.col.stallCount[v.slot];
  SND_CHECK(stalls >= 1u);
  std::size_t faded = 0, bad = 0;
  for (std::size_t i = 0; i < v.out[0].size() && i < n; ++i) {
    const double g = v.out[0][i] / ref.ch[0][i];
    if (g < -1e-6 || g > 1.0 + 1e-6) ++bad;
    else if (g < 1.0 - 1e-6) ++faded;
  }
  SND_CHECK_MSG(bad == 0u, "%zu frames are not the material at their index", bad);
  SND_CHECK_MSG(faded <= 2u * 240u * stalls + 2u, "%zu faded frames for %u stalls", faded, stalls);
}

// C4: time priority keeps the clock: the frames rendered in block j are
// material[256 j + i] (gain in [0, 1], 1 away from the fades) and the voice
// ends at about n / 256 blocks. Returns the number of blocks in which the
// playhead was ahead of the clock.
namespace {
std::size_t RunTimePriority(const Built& b, const Decoded& ref, std::uint64_t n, int hold) {
  IndepDriver d(b, IndepDriver::Opt{});
  SND_CHECK(d.Ok());
  if (!d.Ok()) return 0;
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("t"), 0, 0);
  SND_CHECK(v.slot != snd::kInvalidSlot);
  std::size_t bad = 0, faded = 0, ahead = 0;
  for (int j = 0; j < 1000 && v.ended == 0u; ++j) {
    if (j == 40) d.fs.Hold(b.files[0].first);
    if (j == 40 + hold) d.fs.Release(b.files[0].first);
    const std::size_t before = v.out.empty() ? 0u : v.out[0].size();
    d.Block(d.Stream(), {&v}, 256);
    for (std::size_t i = before; i < v.out[0].size(); ++i) {
      const std::size_t m = std::size_t(j) * 256u + (i - before);
      if (m >= n) {
        ++bad;
        continue;
      }
      const double g = v.out[0][i] / ref.ch[0][m];
      if (g < -1e-6 || g > 1.0 + 1e-6) ++bad;
      else if (g < 1.0 - 1e-6) ++faded;
    }
    if (v.ended == 0u && v.ph.frame > std::uint64_t(j + 1) * 256u) ++ahead;
  }
  const std::uint32_t stalls = d.Tables().Stream.col.stallCount[v.slot];
  SND_CHECK(v.ended != 0u);
  SND_CHECK_MSG(stalls >= 1u, "hold %d: no stall", hold);
  SND_CHECK_MSG(bad == 0u, "hold %d: %zu frames off the clock", hold, bad);
  SND_CHECK_MSG(faded <= 2u * 240u * stalls + 2u, "hold %d: %zu faded frames", hold, faded);
  SND_CHECK_MSG(v.blocks.size() <= n / 256u + 3u, "hold %d: ended after %zu blocks", hold, v.blocks.size());
  std::printf("  hold %d: played %zu of %llu, %u stalls, %zu blocks with the playhead ahead of the clock\n", hold,
              v.out[0].size(), static_cast<unsigned long long>(n), stalls, ahead);
  return ahead;
}
}  // namespace

SND_TEST(IndepStream, TimePriorityStallStaysOnTheClock) {
  const std::uint64_t n = 150000;
  Built b = Build("itim", {StreamSpec("t", n, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "t");
  SND_REQUIRE(ref.ok);
  RunTimePriority(b, ref, n, 120);  // the gap fits the ring: frames skipped in the ring
  RunTimePriority(b, ref, n, 200);  // it does not: re-seek ahead of the voice
}

// The playhead (Playhead.frame, what core reports as the position and sends
// back at Devirtualize) must not run ahead of the clock. After a time-priority
// re-seek the kind reports the re-seek target (time + ready + 2 blocks) while
// the voice waits for it: up to ~2560 frames ahead for several blocks.
SND_TEST(IndepStream, Defect_source_TimePriorityPlayheadRunsAheadWhileReseeking) {
  const std::uint64_t n = 150000;
  Built b = Build("itim2", {StreamSpec("t", n, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "t");
  SND_REQUIRE(ref.ok);
  const std::size_t ahead = RunTimePriority(b, ref, n, 200);
  SND_CHECK_MSG(ahead == 0u, "%zu blocks with the playhead ahead of the clock", ahead);
}

// SetLoop while streaming (loop off to ring out, a common "loop release"):
// playback must not be interrupted. Today SetLoop re-seeks the stream to the
// current position: the PCM ring is dropped and the voice re-primes from IO.
SND_TEST(IndepStream, Defect_source_SetLoopInterruptsAStream) {
  Built b = Build("isetl", {StreamSpec("l", 96000, "PC16", "\"loop\": {\"start\": 10000, \"end\": 60001}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "l");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("l"), 0, src::kStartLoop);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 100; ++i) d.Block(d.Stream(), {&v}, 256);
  SND_CHECK_EQ(v.silentBlocks, 0u);
  const std::uint64_t pos = v.ph.frame;
  const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
  d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 0, 0);
  for (int i = 0; i < 20; ++i) d.Block(d.Stream(), {&v}, 256);
  std::uint32_t shortBlocks = 0;
  for (std::size_t i = b0; i < v.blocks.size(); ++i) shortBlocks += v.blocks[i] < 256u ? 1u : 0u;
  SND_CHECK_MSG(shortBlocks == 0u, "%u short blocks after SetLoop", shortBlocks);
  SND_CHECK(MaxDiff(v.out[0], before, Slice(ref.ch[0], static_cast<std::size_t>(pos), 2000)) < 1e-6);
}

// C7: release while reads are held in flight (the file is held from the
// start: header and data reads stay pending in the FS) and the preload decode
// job is queued: the slot stays closed (a new stream gets NO_STREAM_SLOT)
// until every read came back and the job ran; then the decoder, the pool
// chunks and the slot are returned and a new stream in the same slot plays
// exactly from its start.
SND_TEST(IndepStream, ReleaseWithReadsInFlightReturnsEverything) {
  Built b = Build("irelio", {StreamSpec("r", 200000, "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "r");
  SND_REQUIRE(ref.ok);
  IndepDriver::Opt o;
  o.maxStreams = 1;
  o.manualJobs = true;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  d.fs.Hold(b.files[0].first);
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("r"), 0, 0);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  for (int i = 0; i < 3; ++i) d.Block(d.Stream(), {&v}, 256);  // reads issued and held
  SND_CHECK(!d.queue.empty());                                   // the preload job is queued
  SND_CHECK(d.pool.Used() > 0u);
  d.Release(d.Stream(), v.slot);
  for (int i = 0; i < 5; ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK_EQ(d.runtime.Stats().streamInstances, 1u);  // closing
  std::uint32_t detail = 0;
  SND_CHECK_EQ(d.Start(d.Stream(), d.Asset("r"), 0, 0, &detail), snd::kInvalidSlot);
  SND_CHECK_EQ(detail, SND_END_DETAIL_NO_STREAM_SLOT);
  d.RunJobs();
  for (int i = 0; i < 5; ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK_EQ(d.runtime.Stats().streamInstances, 1u);  // the reads are still in flight
  d.fs.Release(b.files[0].first);
  for (int i = 0; i < 10; ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
  SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 0u);
  SND_CHECK_EQ(d.pool.Used(), 0u);
  IndepDriver::Voice w;
  w.slot = d.Start(d.Stream(), d.Asset("r"), 0, 0);
  SND_REQUIRE(w.slot != snd::kInvalidSlot);
  for (int i = 0; i < 300 && (w.out.empty() || w.out[0].size() < 20000u); ++i) {
    d.Block(d.Stream(), {&w}, 256);
    d.RunJobs();
  }
  SND_REQUIRE(!w.out.empty());
  SND_CHECK(w.out[0].size() >= 20000u);
  SND_CHECK(MaxDiff(w.out[0], 0, Slice(ref.ch[0], 0, 20000)) < 1e-6);
}

// D-45 / C9: the Stream columns. ringRead <= ringWrite <= ringRead + ring
// frames; readPos (the IO side's read position mirrored by the decode side)
// never goes back while no seek happens and stays inside the file.
SND_TEST(IndepStream, StreamColumnsStayConsistent) {
  Built b = Build("icol", {StreamSpec("c", 200000, "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("c"), 0, 0);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  auto& col = d.Tables().Stream.col;
  std::uint64_t lastRead = 0, lastPos = 0;
  std::uint32_t bad = 0, readPosMoves = 0;
  const std::uint64_t fileBytes = b.files[0].second.size();
  for (int i = 0; i < 900 && v.ended == 0u; ++i) {
    d.Block(d.Stream(), {&v}, 256);
    const std::uint64_t w = col.ringWrite[v.slot].load();
    const std::uint64_t r = col.ringRead[v.slot].load();
    const std::uint64_t p = col.readPos[v.slot].load();
    if (r > w || w - r > 65536u || r < lastRead) ++bad;
    if (p < lastPos || p > fileBytes) ++bad;
    if (p != lastPos) ++readPosMoves;
    lastRead = r;
    lastPos = p;
  }
  SND_CHECK_EQ(bad, 0u);
  SND_CHECK(readPosMoves > 5u);
  SND_CHECK_EQ(lastRead, v.out[0].size());  // ringRead counts the frames consumed (no seek here)
}

// C1 / E4: a start inside the preload needs no priming and PrimeFrames is 0;
// a start past the preload primes (needsPriming, QueryReady false until data)
// and PrimeFrames > 0; once ready the first frame is the start frame exactly.
SND_TEST(IndepStream, PrimingAndPrimeFrames) {
  Built b = Build("iprime", {StreamSpec("p", 96000, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "p");
  SND_REQUIRE(ref.ok);
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  const src::SourceAsset* a = d.Asset("p");
  SND_REQUIRE(a != nullptr);
  SND_REQUIRE(a->preload != nullptr && a->preloadFrames > 1000u);
  SND_CHECK_EQ(d.Stream().PrimeFrames(*a, 0), 0u);
  SND_CHECK(d.Stream().PrimeFrames(*a, 50000) > 0u);
  SND_CHECK_EQ(d.Sample().PrimeFrames(*a, 50000), 0u);
  std::uint32_t priming = 9;
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), a, 100, 0, nullptr, &priming);
  SND_REQUIRE(v.slot != snd::kInvalidSlot);
  SND_CHECK_EQ(priming, 0u);
  d.Block(d.Stream(), {&v}, 256);
  SND_CHECK_EQ(v.blocks[0], 256u);  // plays in its first block
  SND_CHECK(MaxDiff(v.out[0], 0, Slice(ref.ch[0], 100, 256)) < 1e-6);
  IndepDriver::Voice w;
  w.ph.frame = 50000;
  w.slot = d.Start(d.Stream(), a, 50000, 0, nullptr, &priming);
  SND_REQUIRE(w.slot != snd::kInvalidSlot);
  SND_CHECK_EQ(priming, 1u);
  SND_CHECK(!d.Ready(d.Stream(), w.slot));
  for (int i = 0; i < 50 && !d.Ready(d.Stream(), w.slot); ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK(d.Ready(d.Stream(), w.slot));
  d.Block(d.Stream(), {&w}, 256);
  SND_CHECK_EQ(w.blocks[0], 256u);
  SND_CHECK(MaxDiff(w.out[0], 0, Slice(ref.ch[0], 50000, 256)) < 1e-6);
}

// C6: maxStreams streams must be playable at once whatever their channel count
// (Config.maxStreams = simultaneous streams, design 3.3 / 6.3; channels 1..8).
// Today the PCM rings are sized for 2 channels per stream on average
// (max(8, 2 * maxStreams) rings): with maxStreams = 2, two 6-channel streams do
// not fit and the second is refused with NO_STREAM_SLOT while a slot is free.
SND_TEST(IndepStream, Defect_source_MultichannelStreamsRefusedBelowMaxStreams) {
  Built b = Build("i6ch", {StreamSpec("six", 48000, "PC16", "", 6)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver::Opt o;
  o.maxStreams = 2;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  std::uint32_t d1 = 0, d2 = 0;
  const snd::SlotIndex s1 = d.Start(d.Stream(), d.Asset("six"), 0, 0, &d1);
  const snd::SlotIndex s2 = d.Start(d.Stream(), d.Asset("six"), 0, 0, &d2);
  SND_CHECK(s1 != snd::kInvalidSlot);
  SND_CHECK_MSG(s2 != snd::kInvalidSlot, "second 6-channel stream of maxStreams 2 refused (detail %u)", d2);
}
