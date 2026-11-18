// Independent re-check of bfc6b36: a stream IO error is counted exactly once
// per playback (Stats().streamIoErrors), when the failed read is delivered or
// a bad header is found; decode errors are counted separately
// (decodeErrors). Properties, as exact deltas per scenario:
//   - no error -> 0 IO errors (plain play, loops repeated from memory, seeks,
//     an IO hold that stalls, release mid-play);
//   - one playback with one or many failed reads, a file that cannot be
//     opened, a bad header, a short file -> exactly 1, and it stays 1 while the
//     voice keeps playing (an infinite loop in memory) or is seeked afterwards;
//   - two playbacks of a failing file -> 2; a good playback reusing the slot
//     of a failed one -> no new count;
//   - corrupt Opus data -> decodeErrors 1, streamIoErrors 0.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "indep_driver.hpp"
#include "reference.hpp"
#include "snd/format/bank_format.hpp"

using namespace indep;
namespace src = snd::source;

namespace {

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
snd::bank_format::StreamFileHeader HeaderOf(const Built& b, const std::string& path) {
  snd::bank_format::StreamFileHeader h{};
  for (const auto& f : b.files) {
    if (f.first == path) std::memcpy(&h, f.second.data(), sizeof(h));
  }
  return h;
}
struct Counts {
  std::uint64_t io = 0, dec = 0;
};
Counts Now(IndepDriver& d) {
  const auto s = d.runtime.Stats();
  return Counts{s.streamIoErrors, s.decodeErrors};
}
// Plays `blocks` blocks (or until the end); returns the voice.
IndepDriver::Voice Play(IndepDriver& d, const char* name, int blocks, std::uint32_t flags = 0, std::uint64_t start = 0) {
  IndepDriver::Voice v;
  v.ph.frame = start;
  v.slot = d.Start(d.Stream(), d.Asset(name), start, flags);
  for (int i = 0; i < blocks && v.ended == 0u; ++i) d.Block(d.Stream(), {&v}, 256);
  return v;
}
void Free(IndepDriver& d, IndepDriver::Voice& v) {
  if (v.slot != snd::kInvalidSlot) d.Release(d.Stream(), v.slot);
  for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
}

}  // namespace

// No error, no count: plain play to the end, a short loop repeated from the
// ring, an infinite loop in the preload, seeks, a long IO hold (stall, re-seek),
// a release mid-play with reads in flight.
SND_TEST(IndepFix7, NoErrorNoCount) {
  Built b = Build("f7none", {Spec("p", 96000, "PC16"), Spec("l", 96000, "ADPC", "\"loop\": {\"start\": 20000, \"end\": 20500}"),
                             Spec("i", 48000, "PC16", "\"loop\": {\"start\": 100, \"end\": 800}")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v = Play(d, "p", 1000);
  SND_CHECK(v.ended != 0u);
  Free(d, v);
  v = Play(d, "l", 300, src::kStartLoop);
  Free(d, v);
  v = Play(d, "i", 300, src::kStartLoop);
  Free(d, v);
  v = Play(d, "p", 20);
  for (std::uint64_t t : {std::uint64_t{50000}, std::uint64_t{100}, std::uint64_t{90000}}) {
    d.Control(d.Stream(), v.slot, src::ControlOp::Seek, t);
    for (int i = 0; i < 20; ++i) d.Block(d.Stream(), {&v}, 256);
  }
  Free(d, v);
  d.fs.Hold(PathOf(b, "p"));
  v = Play(d, "p", 20);
  for (int i = 0; i < 200; ++i) d.Block(d.Stream(), {&v}, 256);  // stalls, time-priority re-seek
  d.fs.Release(PathOf(b, "p"));
  for (int i = 0; i < 50; ++i) d.Block(d.Stream(), {&v}, 256);
  Free(d, v);
  d.fs.SetLatency(PathOf(b, "p"), 20);
  v = Play(d, "p", 3);  // reads in flight
  Free(d, v);
  const Counts c = Now(d);
  SND_CHECK_EQ(c.io, 0u);
  SND_CHECK_EQ(c.dec, 0u);
}

// Exactly one count per failing playback, whatever the kind and the number of
// failed reads, and it does not grow while the voice keeps playing or is
// seeked after the error.
SND_TEST(IndepFix7, OneCountPerFailingPlayback) {
  Built good = Build("f7one", {Spec("p", 96000, "PC16"), Spec("i", 48000, "PC16", "\"loop\": {\"start\": 100, \"end\": 800}")});
  SND_REQUIRE_MSG(good.ok, "%s", good.diag.c_str());
  const std::string path = PathOf(good, "p");
  const snd::bank_format::StreamFileHeader h = HeaderOf(good, path);
  struct Case {
    const char* what;
    int kind;
  };
  const Case cases[] = {{"reads failing from frame 20000 to the end (many failed reads)", 0},
                        {"one failed read (the 6th)", 1},
                        {"the file cannot be opened", 2},
                        {"bad header magic", 3},
                        {"file cut short (short read)", 4},
                        {"infinite loop in the preload, file cannot be opened, 300 blocks", 5},
                        {"error, then seeks and a SetLoop", 6}};
  for (const Case& c : cases) {
    Built b = good;
    for (auto& f : b.files) {
      if (f.first != path) continue;
      if (c.kind == 3) f.second[0] ^= 1u;
      if (c.kind == 4) f.second.resize(static_cast<std::size_t>(h.dataOffset + h.dataBytes / 2u));
    }
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    const char* name = c.kind == 5 ? "i" : "p";
    const std::string pth = PathOf(b, name);
    if (c.kind == 0 || c.kind == 6) d.fs.FailReads(pth, h.dataOffset + 2u * 20000u, UINT64_MAX);
    if (c.kind == 1) d.fs.FailNthRead(5);
    if (c.kind == 2 || c.kind == 5) d.fs.FailOpen(pth);
    IndepDriver::Voice v = Play(d, name, c.kind == 5 ? 300 : 1000, c.kind == 5 ? src::kStartLoop : 0u);
    std::uint64_t mid = Now(d).io;
    if (c.kind == 6) {
      d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 1000);
      for (int i = 0; i < 50; ++i) d.Block(d.Stream(), {&v}, 256);
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 100, 900);
      for (int i = 0; i < 50; ++i) d.Block(d.Stream(), {&v}, 256);
      d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 70000);
      for (int i = 0; i < 50; ++i) d.Block(d.Stream(), {&v}, 256);
    }
    Free(d, v);
    const Counts after = Now(d);
    std::printf("  %s: IO errors %llu (during play %llu), decode errors %llu, ended %u\n", c.what,
                static_cast<unsigned long long>(after.io), static_cast<unsigned long long>(mid),
                static_cast<unsigned long long>(after.dec), v.ended);
    SND_CHECK_MSG(after.io == 1u && mid == 1u, "%s: %llu IO errors", c.what, static_cast<unsigned long long>(after.io));
    SND_CHECK_EQ(after.dec, 0u);
  }
}

// Per playback: two failing playbacks (one after the other in the same slot,
// and two at once) count 2; a good playback in the slot of a failed one adds
// nothing.
SND_TEST(IndepFix7, CountsArePerPlayback) {
  Built b = Build("f7per", {Spec("p", 96000, "PC16"), Spec("q", 96000, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  {
    IndepDriver::Opt o;
    o.maxStreams = 1;
    IndepDriver d(b, o);
    SND_REQUIRE(d.Ok());
    d.fs.FailOpen(PathOf(b, "p"));
    IndepDriver::Voice v = Play(d, "p", 200);
    Free(d, v);
    SND_CHECK_EQ(Now(d).io, 1u);
    v = Play(d, "q", 1000);  // good file, same slot
    SND_CHECK(v.ended != 0u && !v.out.empty() && v.out[0].size() == 96000u);
    Free(d, v);
    SND_CHECK_MSG(Now(d).io == 1u, "a good playback in the slot of a failed one counted (%llu)",
                  static_cast<unsigned long long>(Now(d).io));
    v = Play(d, "p", 200);  // the failing file again, same slot
    Free(d, v);
    SND_CHECK_EQ(Now(d).io, 2u);
  }
  {
    IndepDriver::Opt o;
    o.maxStreams = 2;
    IndepDriver d(b, o);
    SND_REQUIRE(d.Ok());
    d.fs.FailReads(PathOf(b, "p"), 2048u + 2u * 20000u, UINT64_MAX);
    IndepDriver::Voice a, c;
    a.slot = d.Start(d.Stream(), d.Asset("p"), 0, 0);
    c.slot = d.Start(d.Stream(), d.Asset("p"), 5000, 0);
    for (int i = 0; i < 1000 && (a.ended == 0u || c.ended == 0u); ++i) d.Block(d.Stream(), {&a, &c}, 256);
    Free(d, a);
    Free(d, c);
    SND_CHECK_EQ(Now(d).io, 2u);
  }
}

// Decode errors are counted apart: corrupt Opus data (a packet length of 0 in
// the middle of the file, after 30 packets) -> the 30 packets play out,
// decodeErrors 1, streamIoErrors 0.
SND_TEST(IndepFix7, DecodeErrorsCountedApart) {
  if (!snd::codec::OpusAvailable()) return;
  Built b = Build("f7dec", {Spec("o", 96000, "OPUS", "\"preloadMs\": 0", &Sine1k)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const std::string path = PathOf(b, "o");
  const snd::bank_format::StreamFileHeader h = HeaderOf(b, path);
  for (auto& f : b.files) {
    if (f.first != path) continue;
    // Walk 30 packets and zero the next length prefix.
    std::uint64_t o = h.dataOffset;
    for (int k = 0; k < 30; ++k) o += 2u + (f.second[o] | (std::uint64_t{f.second[o + 1u]} << 8));
    f.second[o] = 0;
    f.second[o + 1u] = 0;
  }
  IndepDriver d(b, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v = Play(d, "o", 1000);
  Free(d, v);
  const Counts c = Now(d);
  std::printf("  corrupt Opus packet: ended %u after %zu frames, decode errors %llu, IO errors %llu\n", v.ended,
              v.out.empty() ? 0u : v.out[0].size(), static_cast<unsigned long long>(c.dec), static_cast<unsigned long long>(c.io));
  SND_CHECK(v.ended != 0u);
  // What is decoded before the error plays out: the 30 good packets give at most
  // 30 x 960 - pre-skip 312 frames; ICodec::Decode reports an error for the
  // whole call (D-23), so the frames of good packets decoded in the failing call
  // can be lost: at most one packet less (spec gap, reported).
  const std::size_t got = v.out.empty() ? 0u : v.out[0].size();
  SND_CHECK(got <= 30u * 960u - 312u && got + 960u >= 30u * 960u - 312u);
  SND_CHECK_EQ(c.dec, 1u);
  SND_CHECK_EQ(c.io, 0u);
}
