// Independent re-check of the fourth round (30813e1 .. ee9c628): more Seek /
// SetLoop orders (target exactly at a loop end, loops of one frame, two
// pending SetLoops), a stream file rebuilt between playbacks (StreamChannel::
// ReopenFile at Start), one open per playback across loops and seeks, and the
// bank unregistration counters for slots beyond 256 (maxBanks).
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

AssetSpec Spec(const char* name, std::uint64_t frames, const char* form, const char* codec, const char* extra = "",
               double (*f)(std::uint32_t, std::uint64_t) = &Ramp) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, f);
  a.loadForm = form;
  a.codec = codec;
  a.extra = extra;
  return a;
}
std::vector<float> Model(const std::vector<float>& x, std::uint64_t p, std::uint64_t lb, std::uint64_t le, std::size_t n,
                         std::uint64_t* end = nullptr) {
  std::vector<float> v;
  while (v.size() < n && p < x.size()) {
    v.push_back(x[static_cast<std::size_t>(p)]);
    if (++p == le && le != 0u) p = lb;
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

struct Op {
  int kind;  // 0 seek, 1 set loop
  std::uint64_t a, b;
};
// The decided composition (decisions 1 and A): a Seek's target is wrapped by
// the loop in force; a later SetLoop that ends at or before the pending target
// sends the target to its loop start.
void Compose(const std::vector<Op>& ops, std::uint64_t* target, std::uint64_t* lb, std::uint64_t* le) {
  bool has = false;
  std::uint64_t t = 0, l0 = 0, l1 = 0;
  for (const Op& o : ops) {
    if (o.kind == 0) {
      t = (l1 > l0 && o.a >= l1) ? l0 + (o.a - l1) % (l1 - l0) : o.a;
      has = true;
    } else {
      l0 = o.a;
      l1 = o.b;
      if (has && l1 > l0 && t >= l1) t = l0;
    }
  }
  *target = t;
  *lb = l0;
  *le = l1;
}

}  // namespace

// More orders in one block, in every form: playback from the composed target
// under the last loop, exact, the playhead naming the next frame.
SND_TEST(IndepFix3, MoreSeekSetLoopOrders) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("pm", 96000, "pcm", "PC16"));
  as.push_back(Spec("ra", 96000, "compressed", "ADPC"));
  as.push_back(Spec("cp", 96000, "decompressCache", "PC16"));
  as.push_back(Spec("st", 96000, "stream", "PC16"));
  Built b = Build("f3orders", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  struct Case {
    const char* what;
    std::vector<Op> ops;
  };
  const std::vector<Case> cases = {
      {"loop, seek exactly to the loop end", {{1, 2000, 40001}, {0, 40001, 0}}},
      {"loop, seek to the last frame of the loop", {{1, 2000, 40001}, {0, 40000, 0}}},
      {"seek, loop of one frame ending at the target", {{0, 30000, 0}, {1, 29999, 30000}}},
      {"two loops then a seek (one-frame loop)", {{1, 5000, 6001}, {1, 9000, 9001}, {0, 20000, 0}}},
      {"seek, two loops, the second ending before the target", {{0, 20000, 0}, {1, 1000, 50001}, {1, 1000, 15001}}},
      {"seek, loop ending exactly at the target", {{0, 20000, 0}, {1, 1000, 20000}}},
      {"seek, loop ending one frame after the target", {{0, 20000, 0}, {1, 1000, 20001}}},
      {"seek, loop, seek past it, loop", {{0, 20000, 0}, {1, 1000, 30001}, {0, 35000, 0}, {1, 3000, 70001}}},
  };
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    const bool stream = a.loadForm == "stream";
    for (const Case& c : cases) {
      // Streams with a one-frame loop are covered by ShortStreamLoopsPlayExactly.
      if (stream && std::string(c.what).find("one-frame") != std::string::npos) continue;
      if (stream && std::string(c.what).find("one frame ending") != std::string::npos) continue;
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      src::ISourceKind& k = stream ? d.Stream() : d.Sample();
      IndepDriver::Voice v;
      v.slot = d.Start(k, d.Asset(a.name), 0, 0);
      SND_REQUIRE(v.slot != snd::kInvalidSlot);
      for (int i = 0; i < 30; ++i) d.Block(k, {&v}, 256);
      const std::size_t before = v.out[0].size(), b0 = v.blocks.size();
      for (const Op& o : c.ops) {
        if (o.kind == 0) d.Control(k, v.slot, src::ControlOp::Seek, o.a);
        else d.Control(k, v.slot, src::ControlOp::SetLoop, o.a, o.b);
      }
      for (int i = 0; i < 60; ++i) d.Block(k, {&v}, 256);
      std::uint64_t t = 0, lb = 0, le = 0, end = 0;
      Compose(c.ops, &t, &lb, &le);
      const std::size_t got = v.out[0].size() - before;
      const std::vector<float> want = Model(ref.ch[0], t, lb, le, got, &end);
      const double e = MaxDiff(v.out[0], before, want);
      std::uint32_t sb = 0;
      for (std::size_t i = b0; i < v.blocks.size(); ++i) sb += v.blocks[i] < 256u ? 1u : 0u;
      SND_CHECK_MSG(e < 1e-6, "%s, %s: err %g (first %g, want %g from %llu)", a.name.c_str(), c.what, e,
                    got != 0u ? v.out[0][before] : -9.0, want.empty() ? -9.0 : want[0], static_cast<unsigned long long>(t));
      SND_CHECK_MSG(v.ph.frame == end, "%s, %s: playhead %llu, next frame %llu", a.name.c_str(), c.what,
                    static_cast<unsigned long long>(v.ph.frame), static_cast<unsigned long long>(end));
      if (!stream) SND_CHECK_MSG(sb == 0u, "%s, %s: %u short blocks", a.name.c_str(), c.what, sb);
      SND_CHECK_MSG(got > 10000u, "%s, %s: only %zu frames (ended %u)", a.name.c_str(), c.what, got, v.ended);
    }
  }
}

namespace {
// Two builds of the same stream asset (same name, length and codec: same
// stream file path and header), with other content.
struct TwoBuilds {
  Built b1, b2;
  Decoded r1, r2;
  bool ok = false;
};
TwoBuilds MakeTwo(const char* tag, const char* extra = "") {
  TwoBuilds t;
  t.b1 = Build(std::string(tag) + "1", {Spec("s", 96000, "stream", "PC16", extra, &Ramp)});
  t.b2 = Build(std::string(tag) + "2", {Spec("s", 96000, "stream", "PC16", extra, &Sine1k)});
  if (!t.b1.ok || !t.b2.ok || t.b1.files.size() != 1u || t.b2.files.size() != 1u) return t;
  if (t.b1.files[0].first != t.b2.files[0].first) return t;
  t.r1 = Reference(t.b1, "s");
  t.r2 = Reference(t.b2, "s");
  t.ok = t.r1.ok && t.r2.ok;
  return t;
}
}  // namespace

// A playback started after the stream file was rebuilt reads the new file,
// also when the start and a Seek come in the same block (core: Play then Seek
// before the next block). Today the Seek's Cancel makes the header read posted
// by Start stale; the IO thread drops it before resolving the file, so the
// reopen flag it carried is lost and the next reads use the old open file.
SND_TEST(IndepFix3, StartAndSeekInOneBlockReadTheRebuiltStreamFile) {
  TwoBuilds t = MakeTwo("f3reopen", "\"preloadMs\": 0");
  SND_REQUIRE(t.ok);
  // 0: start; 1: start + seek; 2: start + seek and Publish before the IO thread runs (it is
  // slower than the block: the Seek's Cancel makes the header read stale before it is drained).
  for (int withSeek = 0; withSeek < 3; ++withSeek) {
    IndepDriver d(t.b1, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    IndepDriver::Voice v1;
    v1.slot = d.Start(d.Stream(), d.Asset("s"), 0, 0);
    for (int i = 0; i < 40; ++i) d.Block(d.Stream(), {&v1}, 256);
    SND_REQUIRE(v1.out[0].size() > 4000u);
    SND_CHECK(MaxDiff(v1.out[0], 0, Model(t.r1.ch[0], 0, 0, 0, 4000)) < 1e-6);
    d.Release(d.Stream(), v1.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    d.fs.AddFile(t.b2.files[0].first, t.b2.files[0].second);  // rebuilt (new bytes)
    IndepDriver::Voice v2;
    v2.slot = d.Start(d.Stream(), d.Asset("s"), 0, 0);
    std::uint64_t from = 0;
    if (withSeek != 0) {
      d.Control(d.Stream(), v2.slot, src::ControlOp::Seek, 30000);
      from = 30000;
    }
    if (withSeek == 2) d.Stream().ScheduleBackground(src::BlockInfo{0, 0, 256, 48000});
    for (int i = 0; i < 60; ++i) d.Block(d.Stream(), {&v2}, 256);
    SND_REQUIRE(!v2.out.empty() && v2.out[0].size() > 4000u);
    const double eNew = MaxDiff(v2.out[0], 0, Model(t.r2.ch[0], from, 0, 0, 4000));
    const double eOld = MaxDiff(v2.out[0], 0, Model(t.r1.ch[0], from, 0, 0, 4000));
    const char* what = withSeek == 0 ? "start" : (withSeek == 1 ? "start + seek" : "start + seek, IO after Publish");
    std::printf("  %s: err to the new file %.3g, to the old %.3g\n", what, eNew, eOld);
    SND_CHECK_MSG(eNew < 1e-6, "%s: the new playback does not read the rebuilt file", what);
  }
}

// Two playbacks of the same stream asset overlap while the file is rebuilt:
// the earlier one goes on with the old content, the new one reads the new
// content (the expectation given for decision B: "the earlier user keeps the
// old file until it is done"). Today the earlier playback's later reads open
// the file again and switch to the new bytes mid-play.
SND_TEST(IndepFix3, EarlierPlaybackKeepsItsFile) {
  TwoBuilds t = MakeTwo("f3overlap", "\"preloadMs\": 0");  // no preload: the bank (not rebuilt here) holds none
  SND_REQUIRE(t.ok);
  IndepDriver d(t.b1, IndepDriver::Opt{});
  SND_REQUIRE(d.Ok());
  IndepDriver::Voice v1, v2;
  v1.slot = d.Start(d.Stream(), d.Asset("s"), 0, 0);
  for (int i = 0; i < 20; ++i) d.Block(d.Stream(), {&v1}, 256);
  d.fs.AddFile(t.b2.files[0].first, t.b2.files[0].second);
  v2.slot = d.Start(d.Stream(), d.Asset("s"), 0, 0);
  SND_REQUIRE(v2.slot != snd::kInvalidSlot);
  for (int i = 0; i < 200; ++i) d.Block(d.Stream(), {&v1, &v2}, 256);
  const double e1 = MaxDiff(v1.out[0], 0, Model(t.r1.ch[0], 0, 0, 0, v1.out[0].size()));
  const double e2 = MaxDiff(v2.out[0], 0, Model(t.r2.ch[0], 0, 0, 0, v2.out[0].size()));
  std::size_t firstNew = SIZE_MAX;
  for (std::size_t i = 0; i < v1.out[0].size(); ++i) {
    if (std::fabs(v1.out[0][i] - t.r1.ch[0][i]) > 1e-6) {
      firstNew = i;
      break;
    }
  }
  std::size_t firstOld2 = SIZE_MAX;
  for (std::size_t i = 0; i < v2.out[0].size(); ++i) {
    if (std::fabs(v2.out[0][i] - t.r2.ch[0][i]) > 1e-6) {
      firstOld2 = i;
      break;
    }
  }
  std::printf("  new playback leaves the new file at frame %lld (old file there: %s)\n",
              firstOld2 == SIZE_MAX ? -1LL : static_cast<long long>(firstOld2),
              firstOld2 != SIZE_MAX && std::fabs(v2.out[0][firstOld2] - t.r1.ch[0][firstOld2]) < 1e-6 ? "yes" : "no");
  std::printf("  earlier playback: err to the old file %.3g (leaves it at frame %lld); new playback: err to the new file %.3g\n",
              e1, firstNew == SIZE_MAX ? -1LL : static_cast<long long>(firstNew), e2);
  SND_CHECK_MSG(e2 < 1e-6, "the new playback does not read the new file (err %g)", e2);
  SND_CHECK_MSG(e1 < 1e-6, "the earlier playback leaves the old file at frame %zu", firstNew);
}

// One playback opens its file once, across loops, seeks and a time-priority
// re-seek; a new playback opens it once more.
SND_TEST(IndepFix3, OnePlaybackOpensItsFileOnce) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("lp", 48000, "stream", "PC16", "\"loop\": {\"start\": 1000, \"end\": 30001}"));
  as.push_back(Spec("la", 48000, "stream", "ADPC", "\"loop\": {\"start\": 1000, \"end\": 30001}"));
  if (snd::codec::OpusAvailable()) as.push_back(Spec("lo", 48000, "stream", "OPUS", "\"loop\": {\"start\": 1000, \"end\": 30001}", &Sine1k));
  Built b = Build("f3opens", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    IndepDriver d(b, IndepDriver::Opt{});
    SND_REQUIRE(d.Ok());
    std::string path;
    for (const auto& f : b.files) {
      if (f.first.find("/" + a.name + "_") != std::string::npos) path = f.first;
    }
    SND_REQUIRE(!path.empty());
    const std::uint32_t o0 = d.fs.OpenCalls();
    IndepDriver::Voice v;
    v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, src::kStartLoop);
    for (int i = 0; i < 400; ++i) {  // ~3.5 loops
      if (i == 100) d.Control(d.Stream(), v.slot, src::ControlOp::Seek, 12345);
      if (i == 200) d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 2000, 25001);
      if (i == 250) d.fs.Hold(path);  // a stall long enough for a time-priority re-seek
      if (i == 330) d.fs.Release(path);
      d.Block(d.Stream(), {&v}, 256);
    }
    const std::uint32_t one = d.fs.OpenCalls() - o0;
    d.Release(d.Stream(), v.slot);
    for (int i = 0; i < 4; ++i) d.Block(d.Stream(), {}, 256);
    IndepDriver::Voice w;
    w.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
    for (int i = 0; i < 20; ++i) d.Block(d.Stream(), {&w}, 256);
    const std::uint32_t two = d.fs.OpenCalls() - o0;
    std::printf("  %s: opens during one playback %u, after a second playback %u\n", a.name.c_str(), one, two);
    SND_CHECK_EQ(one, 1u);
    SND_CHECK_EQ(two, 2u);
  }
}

// Unregistration counters sized by maxBanks (300 here): unregistering slot 43
// does not touch the cache entries of slot 299 (43 + 256); unregistering 299
// does. A slot past maxBanks is printed.
SND_TEST(IndepFix3, UnregistrationCountersBeyond256Slots) {
  Built b = Build("f3banks", {Spec("c", 12000, "decompressCache", "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  IndepDriver::Opt o;
  o.maxBanks = 300;
  IndepDriver d(b, o);
  SND_REQUIRE(d.Ok());
  auto play = [&](std::uint16_t slot) {
    src::SourceAsset a = *d.Asset("c");
    a.bankSlot = slot;
    a.reserved = 1;
    IndepDriver::Voice v;
    v.slot = d.Start(d.Sample(), &a, 0, 0);
    for (int i = 0; i < 60 && v.ended == 0u; ++i) d.Block(d.Sample(), {&v}, 256);
    d.Release(d.Sample(), v.slot);
    for (int i = 0; i < 2; ++i) d.Block(d.Sample(), {}, 256);
  };
  auto hits = [&] { return d.runtime.Stats().cacheHits; };
  play(299);
  std::uint64_t h = hits();
  play(299);
  SND_CHECK_EQ(hits(), h + 1u);  // cached
  snd::source::Runtime::BankUnregistered(&d.runtime, 43);
  h = hits();
  play(299);
  SND_CHECK_MSG(hits() == h + 1u, "unregistering slot 43 dropped the entry of slot 299");
  snd::source::Runtime::BankUnregistered(&d.runtime, 299);
  h = hits();
  play(299);
  SND_CHECK_MSG(hits() == h, "unregistering slot 299 kept its entry");
  // A slot past maxBanks (core never makes one): printed, must not crash.
  play(450);
  snd::source::Runtime::BankUnregistered(&d.runtime, 450);
  h = hits();
  play(450);
  std::printf("  slot 450 of maxBanks 300 after its unregistration: %s\n", hits() == h ? "miss" : "hit");
}

// Short stream loops (SetLoop after the start, the loop behind the playhead:
// decision 4, the voice goes to the loop start): every length plays, exactly.
// Today a stream loop of up to 16 frames renders nothing (no frame, not
// ended) and a loop of 100 frames plays wrong frames.
SND_TEST(IndepFix3, ShortStreamLoopsPlayExactly) {
  std::vector<AssetSpec> as;
  as.push_back(Spec("sp", 96000, "stream", "PC16"));
  as.push_back(Spec("sa", 96000, "stream", "ADPC"));
  Built b = Build("f3short", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (const AssetSpec& a : as) {
    const Decoded ref = Reference(b, a.name);
    SND_REQUIRE(ref.ok);
    for (std::uint64_t len : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{3}, std::uint64_t{16}, std::uint64_t{100},
                              std::uint64_t{1017}, std::uint64_t{5000}}) {
      IndepDriver d(b, IndepDriver::Opt{});
      SND_REQUIRE(d.Ok());
      IndepDriver::Voice v;
      v.slot = d.Start(d.Stream(), d.Asset(a.name), 0, 0);
      for (int i = 0; i < 30; ++i) d.Block(d.Stream(), {&v}, 256);
      const std::size_t before = v.out[0].size();
      d.Control(d.Stream(), v.slot, src::ControlOp::SetLoop, 5000, 5000 + len);
      for (int i = 0; i < 60; ++i) d.Block(d.Stream(), {&v}, 256);
      const std::size_t got = v.out[0].size() - before;
      std::uint64_t end = 0;
      const std::uint64_t p = 30u * 256u;
      const std::uint64_t from = p < 5000 + len ? p : 5000;
      const std::vector<float> m = Model(ref.ch[0], from, 5000, 5000 + len, got, &end);
      const double e = MaxDiff(v.out[0], before, m);
      std::size_t leave = SIZE_MAX;
      for (std::size_t i = 0; i < m.size(); ++i) {
        if (std::fabs(v.out[0][before + i] - m[i]) > 1e-6) {
          leave = i;
          break;
        }
      }
      std::printf("  %s loop of %llu frames: %zu frames, err %.3g (leaves the model at %lld, short blocks %llu)\n", a.name.c_str(),
                  static_cast<unsigned long long>(len), got, e, leave == SIZE_MAX ? -1LL : static_cast<long long>(leave),
                  static_cast<unsigned long long>(v.silentBlocks));
      SND_CHECK_MSG(got > 10000u && e < 1e-6, "%s loop %llu: %zu frames, err %g", a.name.c_str(),
                    static_cast<unsigned long long>(len), got, e);
    }
  }
}
