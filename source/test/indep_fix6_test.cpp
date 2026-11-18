// Independent re-check of the stream IO error decision (a8bbdee): no new read
// after an IO error; everything in memory plays out in order, then the voice
// ends (STREAM_IO, faded). An infinite loop entirely in memory goes on (the IO
// error is counted at once). Properties:
//   - the played length is EXACTLY determined by the failing byte F and the
//     read plan: the read that overlaps F fails as a whole (and, delivered in
//     order, nothing after it is used); every read before it was used. The
//     reference read plan is io::StreamReadPlanner with the Stream kind's
//     parameters (chunk 4096, from the preload end). PCM: whole frames before
//     the cut; ADPCM: whole blocks; Opus: whole packets (walked in the file),
//     minus the pre-skip.
//   - Seek / SetLoop / Stop / Virtualize + Devirtualize after the error: the
//     voice plays only what is in memory and ends (never hangs silent);
//   - an infinite loop in the preload with a stream file that cannot be opened:
//     plays on exactly, the IO error is counted, Stop frees everything.
// Defect_source_* tests fail on purpose.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"
#include "snd/format/bank_format.hpp"
#include "snd/io/stream_read_planner.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

constexpr std::uint32_t kChunk = 4096;
constexpr std::size_t kFade = 240;

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
std::string PathOf(const Built& b, const std::string& name) {
  for (const auto& f : b.files) {
    if (f.first.find("/" + name + "_") != std::string::npos) return f.first;
  }
  return std::string();
}
const std::vector<std::uint8_t>* FileOf(const Built& b, const std::string& path) {
  for (const auto& f : b.files) {
    if (f.first == path) return &f.second;
  }
  return nullptr;
}
double MaxDiff(const std::vector<float>& got, std::size_t from, const std::vector<float>& want, std::size_t n) {
  double m = 0;
  for (std::size_t i = 0; i < n && i < want.size(); ++i) {
    const double g = from + i < got.size() ? got[from + i] : 1e9;
    m = std::fmax(m, std::fabs(g - want[i]));
  }
  return m;
}

// Data bytes (from the data start) the Stream kind uses before the read that overlaps file byte F fails.
std::uint64_t UsedBytes(std::uint64_t base, std::uint64_t dataBytes, std::uint64_t preloadBytes, std::uint64_t F) {
  if (F < base + preloadBytes) return preloadBytes;  // the first read after the preload fails
  snd::io::StreamReadPlanner p;
  p.Reset(base, base + dataBytes, 0, 0, kChunk);
  p.Seek(base + preloadBytes);
  snd::io::StreamRead rd{};
  while (p.Next(rd)) {
    if (F < rd.offset + rd.bytes) return rd.offset - base;
  }
  return dataBytes;
}
// Frames decodable from the first `used` data bytes.
std::uint64_t FramesFrom(const snd::CodecHeader& h, const std::vector<std::uint8_t>& file, std::uint64_t base,
                         std::uint64_t used) {
  if (h.codec == SND_CODEC_PCM_S16) return std::min<std::uint64_t>(h.total_frames, used / 2u);
  if (h.codec == SND_CODEC_ADPCM) {
    const std::uint64_t fpb = 1u + (h.block_align - 4u) / 4u * 8u;
    return std::min<std::uint64_t>(h.total_frames, used / h.block_align * fpb);
  }
  // Opus: u16 length + payload; whole packets only.
  std::uint64_t o = 0, packets = 0;
  while (o + 2u <= used) {
    const std::uint64_t len = file[base + o] | (std::uint64_t{file[base + o + 1u]} << 8);
    if (o + 2u + len > used) break;
    o += 2u + len;
    ++packets;
  }
  const std::uint64_t dec = packets * 960u;
  return dec <= h.pre_skip ? 0u : std::min<std::uint64_t>(h.total_frames, dec - h.pre_skip);
}

}  // namespace

// The played length is exactly the frames decodable from the bytes before the
// failing read (preload included), for cuts in the preload region, at, just
// before and just after a read boundary, and deep in the file; PCM, ADPCM,
// Opus; the content is exact but the last fade; the IO error is counted; a
// cut past the data end changes nothing.
SND_TEST(IndepFix6, PlayedLengthIsExactlyWhatPrecedesTheFailingRead) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("p", 96000, "PC16"));
  as.push_back(Spec("a", 96000, "ADPC"));
  if (snd::codec::OpusAvailable()) as.push_back(Spec("o", 96000, "OPUS", "", &Sine1k));
  Built b = Build("f6len", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    const std::string path = PathOf(b, a.name);
    const std::vector<std::uint8_t>* file = FileOf(b, path);
    SND_REQUIRE(file != nullptr);
    snd::bank_format::StreamFileHeader hd{};
    std::memcpy(&hd, file->data(), sizeof(hd));
    IndepDriver probe(b, IndepDriver::Opt{});
    const std::uint64_t pre = probe.Asset(a.name)->preloadBytes;
    const std::uint64_t base = hd.dataOffset;
    const double tol = a.codec == "OPUS" ? 1e-4 : 1e-6;
    std::vector<std::uint64_t> cuts = {base + 10u, base + pre, base + pre + 1u, base + pre + kChunk - 1u, base + pre + kChunk,
                                       base + pre + kChunk + 1u, base + pre + 3u * kChunk + 100u, base + pre + 9u * kChunk + 7u,
                                       base + hd.dataBytes - 1u, base + hd.dataBytes + 5000u};
    for (std::uint64_t F : cuts) {
      for (std::uint32_t lat : {0u, 3u}) {
        IndepDriver d(b, IndepDriver::Opt{});
        SND_REQUIRE(d.Ok());
        d.fs.FailReads(path, F, UINT64_MAX);
        d.fs.SetLatency(path, lat);
        IndepDriver::Voice v;
        v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
        for (int i = 0; i < 800 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
        const bool inData = F < base + hd.dataBytes;
        const std::uint64_t want = inData ? FramesFrom(ref.h, *file, base, UsedBytes(base, hd.dataBytes, pre, F)) : ref.h.total_frames;
        const std::size_t got = v.out.empty() ? 0u : v.out[0].size();
        const std::size_t cmp = got > kFade ? got - kFade : 0u;
        const double e = MaxDiff(v.out[0], 0, ref.ch[0], cmp);
        const std::uint64_t errs = d.runtime.Stats().streamIoErrors;
        SND_CHECK_MSG(v.ended != 0u && got == want && e < tol && (inData ? errs >= 1u : errs == 0u),
                      "%s cut at data byte %lld latency %u: ended %u, %zu frames (want %llu), err %g, ioErrors %llu", a.name.c_str(),
                      static_cast<long long>(F) - static_cast<long long>(base), lat, v.ended, got,
                      static_cast<unsigned long long>(want), e, static_cast<unsigned long long>(errs));
        d.Release(d.Stream(), v.slot);
        for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
        SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
        SND_CHECK_EQ(d.pool.Used(), 0u);
      }
    }
  }
}

// Controls after the IO error while the voice still plays from memory: it
// never hangs silent (ends within a bounded time) and plays nothing but the
// material in memory, from where the control sends it.
SND_TEST(IndepFix6, ControlsAfterTheError) {
  Built b = Build("f6ctl", {Spec("p", 96000, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "p");
  SND_REQUIRE(ref.ok);
  const std::string path = PathOf(b, "p");
  snd::bank_format::StreamFileHeader hd{};
  std::memcpy(&hd, FileOf(b, path)->data(), sizeof(hd));
  struct Case {
    const char* what;
    int op;  // 0 seek, 1 set loop, 2 stop, 3 virtualise + devirtualise
    std::uint64_t a, b2;
  };
  const Case cases[] = {{"seek into the preload", 0, 1000, 0},    {"seek into the read data", 0, 15000, 0},
                        {"seek past the cut", 0, 60000, 0},         {"set loop inside the read data", 1, 9000, 9500},
                        {"set loop across the cut", 1, 9000, 40000}, {"loop off", 1, 0, 0},
                        {"stop", 2, 0, 0},                          {"virtualise and return", 3, 0, 0}};
  for (const Case& c : cases) {
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    d.fs.FailReads(path, hd.dataOffset + 2u * 20000u, UINT64_MAX);  // data past frame ~20000 fails
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset("p"), 0, 0);
    for (int i = 0; i < 30; ++i) d.Block(d.Stream(), {&v}, 256);  // 7680 frames played; the error is in already
    SND_REQUIRE(d.runtime.Stats().streamIoErrors >= 1u);
    const std::size_t before = v.out[0].size();
    if (c.op == 0) d.Control(d.Stream(), v.slot, src::ControlOp::Seek, c.a);
    if (c.op == 1) d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, c.a, c.b2);
    if (c.op == 2) d.Release(d.Stream(), v.slot);
    if (c.op == 3) {
      d.Control(d.Stream(), v.slot, src::ControlOp::Virtualize);
      for (int i = 0; i < 8; ++i) {
        src::AdvanceItem it{0, v.slot, 256, 1.0f, &v.ph};
        snd::VoiceRenderStatus st{};
        d.Stream().AdvanceVirtual(&it, 1, &st);
        d.Block(d.Stream(), {}, 256);
      }
      d.Control(d.Stream(), v.slot, src::ControlOp::Devirtualize, v.ph.frame);
    }
    int blocks = 0;
    if (c.op != 2) {
      for (; blocks < 1500 && v.ended == 0u; ++blocks) d.Block(d.Stream(), {&v}, 256);
    }
    const std::size_t got = v.out[0].size() - before;
    // Every frame played after the control must be a frame of the material (no garbage).
    std::vector<float> values(ref.ch[0]);
    std::sort(values.begin(), values.end());
    std::size_t foreign = 0;
    for (std::size_t i = before; i + kFade < v.out[0].size(); ++i) {
      foreign += std::binary_search(values.begin(), values.end(), v.out[0][i]) ? 0u : 1u;
    }
    std::printf("  %s: %s after %d blocks, %zu frames after the control\n", c.what,
                c.op == 2 ? "released" : (v.ended != 0u ? "ended" : "STILL PLAYING"), blocks, got);
    if (c.op == 1 && c.b2 == 9500) {
      // A loop entirely in memory: it may go on (as an infinite loop in memory does).
    } else if (c.op != 2) {
      SND_CHECK_MSG(v.ended != 0u, "%s: the voice does not end", c.what);
    }
    SND_CHECK_MSG(foreign == 0u, "%s: frames that are not material", c.what);
    if (c.op != 2) d.Release(d.Stream(), v.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
    SND_CHECK_EQ(d.pool.Used(), 0u);
    SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
  }
}

namespace {
void InfiniteLoopInMemory(bool countErrors) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("p", 48000, "PC16", "\"loop\": {\"start\": 100, \"end\": 800}"));
  as.push_back(Spec("a", 48000, "ADPC", "\"loop\": {\"start\": 100, \"end\": 1500}"));
  if (snd::codec::OpusAvailable()) as.push_back(Spec("o", 48000, "OPUS", "\"loop\": {\"start\": 0, \"end\": 960}", &Sine1k));
  Built b = Build("f6inf", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    d.fs.FailOpen(PathOf(b, a.name));
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, src::kStartLoop);
    std::uint64_t errAt = 0;
    for (int i = 0; i < 300; ++i) {
      d.Block(d.Stream(), {&v}, 256);
      if (errAt == 0u && d.runtime.Stats().streamIoErrors != 0u) errAt = static_cast<std::uint64_t>(i + 1);
    }
    std::vector<float> want;
    for (std::uint64_t p = 0; want.size() < 300u * 256u;) {
      want.push_back(ref.ch[0][static_cast<std::size_t>(p)]);
      if (++p == ref.loopEnd) p = ref.loopStart;
    }
    const double e = MaxDiff(v.out[0], 0, want, want.size());
    std::printf("  %s: %zu frames, err %.3g, IO error counted at block %llu\n", a.name.c_str(), v.out[0].size(), e,
                static_cast<unsigned long long>(errAt));
    SND_CHECK(v.ended == 0u);
    SND_CHECK_EQ(v.out[0].size(), 300u * 256u);
    SND_CHECK(e < (a.codec == "OPUS" ? 1e-4 : 1e-6));
    if (countErrors) SND_CHECK_MSG(errAt != 0u && errAt <= 3u, "%s: the IO error is not counted (counted at block %llu)", a.name.c_str(),
                                   static_cast<unsigned long long>(errAt));
    d.Release(d.Stream(), v.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
    SND_CHECK_EQ(d.pool.Used(), 0u);
    SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
    SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 0u);
  }
}
}  // namespace

// Supplement: an infinite loop entirely in the preload, the stream file
// cannot be opened: the loop plays on exactly, Stop frees everything.
SND_TEST(IndepFix6, InfiniteLoopInMemoryPlaysOnAfterTheError) { InfiniteLoopInMemory(false); }

// Supplement, the count: the IO error is counted when it happens (here: the
// open fails, within the first blocks). Today it is never counted while the
// loop plays from memory (streamIoErrors stays 0 for 300 blocks).
SND_TEST(IndepFix6, IoErrorCountedWhileAnInfiniteLoopPlaysFromMemory) { InfiniteLoopInMemory(true); }

// Decision (after 548cda5): after an IO error only the resident preload counts
// as "in memory"; the decoded PCM ring is not a source for loops or seeks.
// SetLoop after the error to a loop outside the preload (9000..9500; the
// playhead at 7680, the ring decoded up to the cut at ~20000): the voice plays
// what the ring holds up to the loop end exactly (9500 - 7680 frames, the end
// faded) and ends; everything is returned.
SND_TEST(IndepFix6, LoopSetAfterTheErrorPlaysTheRingToTheLoopEndThenEnds) {
  Built b = Build("f6loop", {Spec("p", 96000, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, "p");
  SND_REQUIRE(ref.ok);
  const std::string path = PathOf(b, "p");
  snd::bank_format::StreamFileHeader hd{};
  std::memcpy(&hd, FileOf(b, path)->data(), sizeof(hd));
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  d.fs.FailReads(path, hd.dataOffset + 2u * 20000u, UINT64_MAX);
  IndepDriver::Voice v;
  v.slot = d.Start(d.Stream(), d.Asset("p"), 0, 0);
  for (int i = 0; i < 30; ++i) d.Block(d.Stream(), {&v}, 256);
  SND_REQUIRE(d.runtime.Stats().streamIoErrors >= 1u);
  const std::uint64_t p0 = v.ph.frame;
  SND_REQUIRE(p0 < 9000u);
  const std::size_t before = v.out[0].size();
  d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 9000, 9500);
  for (int i = 0; i < 100 && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
  const std::size_t got = v.out[0].size() - before;
  const std::size_t want = static_cast<std::size_t>(9500u - p0);
  const double e = MaxDiff(v.out[0], before, std::vector<float>(ref.ch[0].begin() + static_cast<std::ptrdiff_t>(p0), ref.ch[0].end()),
                           got > kFade ? got - kFade : 0u);  // the end is faded
  std::printf("  loop (9000, 9500) set after the error at %llu: %zu frames (want %zu), %s, err %.3g\n",
              static_cast<unsigned long long>(p0), got, want, v.ended != 0u ? "ended" : "still playing", e);
  SND_CHECK(v.ended != 0u);
  SND_CHECK_EQ(got, want);
  SND_CHECK(e < 1e-6);
  d.Release(d.Stream(), v.slot);
  for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
  SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
  SND_CHECK_EQ(d.pool.Used(), 0u);
  SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
  SND_CHECK_EQ(d.runtime.Stats().decodersInUse, 0u);
}

// Decision on seeks after the IO error: a Seek into the preload plays from
// the target to the preload end exactly and ends; a Seek anywhere else (into
// data read before the cut, or past it) and a Devirtualize end at once (no
// frame). Everything is returned.
SND_TEST(IndepFix6, SeekAfterTheErrorPlaysOnlyThePreload) {
  Built b = Build("f6seek", {Spec("p", 96000, "PC16"), Spec("a", 96000, "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const char* name : {"p", "a"}) {
    const Decoded ref = Reference(b, name);
    SND_REQUIRE(ref.ok);
    const std::string path = PathOf(b, name);
    snd::bank_format::StreamFileHeader hd{};
    std::memcpy(&hd, FileOf(b, path)->data(), sizeof(hd));
    struct Case {
      const char* what;
      int op;  // 0 seek, 1 virtualise + devirtualise
      std::uint64_t target;
    };
    const Case cases[] = {{"seek into the preload", 0, 1000},
                          {"seek to the preload start", 0, 0},
                          {"seek into the data read before the cut", 0, 15000},
                          {"seek past the cut", 0, 60000},
                          {"devirtualise", 1, 0}};
    for (const Case& c : cases) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      const std::uint64_t preFrames = d.Asset(name)->preloadFrames;
      SND_REQUIRE(preFrames > 1000u);
      // The cut past the preload, past the frames rendered before the control.
      const std::uint64_t cutFrame = 20000;
      const std::uint64_t cutByte = std::string(name) == "p" ? 2u * cutFrame : cutFrame / 1017u * 512u;
      d.fs.FailReads(path, hd.dataOffset + cutByte, UINT64_MAX);
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset(name), 0, 0);
      for (int i = 0; i < 30; ++i) d.Block(d.Stream(), {&v}, 256);
      SND_REQUIRE(d.runtime.Stats().streamIoErrors >= 1u);
      SND_REQUIRE(v.ended == 0u);
      const std::size_t before = v.out[0].size();
      if (c.op == 0) {
        d.Control(d.Stream(), v.slot, src::ControlOp::Seek, c.target);
      } else {
        d.Control(d.Stream(), v.slot, src::ControlOp::Virtualize);
        for (int i = 0; i < 4; ++i) {
          src::AdvanceItem it{0, v.slot, 256, 1.0f, &v.ph};
          snd::VoiceRenderStatus st{};
          d.Stream().AdvanceVirtual(&it, 1, &st);
          d.Block(d.Stream(), {}, 256);
        }
        d.Control(d.Stream(), v.slot, src::ControlOp::Devirtualize, v.ph.frame);
      }
      int blocks = 0;
      for (; blocks < 200 && v.ended == 0u; ++blocks) d.Block(d.Stream(), {&v}, 256);
      const std::size_t got = v.out[0].size() - before;
      const bool inPreload = c.op == 0 && c.target < preFrames;
      const std::size_t want = inPreload ? static_cast<std::size_t>(preFrames - c.target) : 0u;
      const double e = inPreload ? MaxDiff(v.out[0], before,
                                           std::vector<float>(ref.ch[0].begin() + static_cast<std::ptrdiff_t>(c.target), ref.ch[0].end()),
                                           got > kFade ? got - kFade : 0u)
                                 : 0.0;
      SND_CHECK_MSG(v.ended != 0u && got == want && e < 1e-6, "%s %s: ended %u after %d blocks, %zu frames (want %zu), err %g", name,
                    c.what, v.ended, blocks, got, want, e);
      if (!inPreload) SND_CHECK_MSG(blocks <= 2, "%s %s: ended only after %d blocks", name, c.what, blocks);
      d.Release(d.Stream(), v.slot);
      for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
      SND_CHECK_EQ(d.fs.OpenFiles(), 0u);
      SND_CHECK_EQ(d.pool.Used(), 0u);
      SND_CHECK_EQ(d.runtime.Stats().streamInstances, 0u);
    }
  }
}
