// Independent re-check of the fifth round through core: playbacks bind their
// stream files and close them when they end (no CloseIdleFiles at bank
// unregistration any more).
//   - StopAndUnload while streams play, the stream file rebuilt, the bank
//     loaded again (another path) and played: the new content; after the
//     playbacks no stream file stays open (memory FS OpenFiles);
//   - two playbacks of one stream asset overlapping while the stream file is
//     rebuilt and the bank reloaded under another path: the earlier keeps
//     its content to its end, the later plays the new one;
//   - many plays of one stream event (one-shots, overlapping) leave no file open.
#include <cmath>
#include <cstdio>
#include <vector>

#include "reference.hpp"
#include "source_harness.hpp"

using namespace srctest;

namespace {
// Not periodic (unique alignment): a slow chirp, and a different one.
double ChirpA(std::uint32_t, std::uint64_t i) {
  const double t = double(i) / 48000.0;
  return 0.4 * std::sin(6.283185307179586 * (200.0 + 300.0 * t) * t) + 0.05;
}
double ChirpB(std::uint32_t, std::uint64_t i) {
  const double t = double(i) / 48000.0;
  return -0.4 * std::sin(6.283185307179586 * (700.0 - 200.0 * t) * t) - 0.05;
}
AssetSpec StreamSpec(double (*f)(std::uint32_t, std::uint64_t), std::uint64_t frames = 48000) {
  AssetSpec a;
  a.name = "s";
  a.pcm = Signal(1, 48000, frames, f);
  a.loadForm = "stream";
  a.codec = "PC16";
  a.extra = "\"preloadMs\": 0";
  return a;
}
int Which(const Rig& r, std::size_t from, const std::vector<float>& e1, const std::vector<float>& e2) {
  const std::size_t f0 = FirstNonZero(r, 0, 1e-4f, from);
  if (f0 == SIZE_MAX) return 0;
  const std::size_t at = f0 >= 8 ? f0 - 8 : 0;
  std::vector<float> h1(e1.begin(), e1.begin() + 4000), h2(e2.begin(), e2.begin() + 4000);
  for (std::size_t s = at; s < at + 40; ++s) {
    const Fit f1 = FitGain(r, 0, s, h1), f2 = FitGain(r, 0, s, h2);
    if (std::fabs(f1.gain - 0.70710678) < 0.01 && f1.maxErr < 1e-3) return 1;
    if (std::fabs(f2.gain - 0.70710678) < 0.01 && f2.maxErr < 1e-3) return 2;
  }
  return 0;
}
}  // namespace

SND_TEST(IndepSourceReload3, StopAndUnloadRebuildReloadPlay) {
  Built b1 = Build("rl5a", {StreamSpec(&Ramp)});
  Built b2 = Build("rl5b", {StreamSpec(&Sine1k)});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  SND_REQUIRE(b1.files[0].first == b2.files[0].first);
  const Decoded r1 = Reference(b1, "s"), r2 = Reference(b2, "s");
  const std::vector<float> e1 = Resampled(r1.ch[0], 8000), e2 = Resampled(r2.ch[0], 8000);
  Rig r;
  SND_REQUIRE(r.Ok());
  const snd_bank_handle h1 = r.LoadAs(b1, "A.sndb");
  r.Play("s");
  r.Play("s");
  r.Steps(30);
  SND_CHECK(r.fs.OpenFiles() >= 1u);
  SND_CHECK_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 400 && r.sys->GetBankState(h1) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  SND_REQUIRE_EQ(r.sys->GetBankState(h1), SND_BANK_STATE_UNLOADED);
  r.Steps(10);
  const std::uint32_t openAfterUnload = r.fs.OpenFiles();
  r.LoadAs(b2, "B.sndb");  // rebuilt bank, its stream file replaces the old one in the FS
  r.all.clear();
  const snd_playback_handle p = r.Play("s");
  for (int i = 0; i < 400 && r.EndOf(p) == nullptr; ++i) r.Step();
  const int w = Which(r, 0, e1, e2);
  r.Steps(10);
  std::printf("  open files after the unload %u; the reloaded bank plays %s; open files at the end %u\n", openAfterUnload,
              w == 2 ? "the new stream" : (w == 1 ? "the OLD stream" : "neither"), r.fs.OpenFiles());
  SND_CHECK_EQ(openAfterUnload, 0u);
  SND_CHECK_EQ(w, 2);
  SND_CHECK_EQ(r.fs.OpenFiles(), 0u);
}

SND_TEST(IndepSourceReload3, OverlappingPlaybacksAcrossARebuild) {
  Built b1 = Build("rl6a", {StreamSpec(&ChirpA, 96000)});
  Built b2 = Build("rl6b", {StreamSpec(&ChirpB, 96000)});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  const Decoded r1 = Reference(b1, "s"), r2 = Reference(b2, "s");
  SND_REQUIRE(r1.ok && r2.ok);
  const std::vector<float> e1 = Resampled(r1.ch[0], r1.ch[0].size() + 64);
  const std::vector<float> e2 = Resampled(r2.ch[0], r2.ch[0].size() + 64);
  Rig r;
  SND_REQUIRE(r.Ok());
  r.LoadAs(b1, "A.sndb");
  const snd_playback_handle p1 = r.Play("s");
  r.Steps(40);
  const std::size_t cut = r.Frames();
  r.fs.AddFile(b2.files[0].first, b2.files[0].second);  // the stream file is rebuilt while p1 plays
  for (int i = 0; i < 1200 && r.EndOf(p1) == nullptr; ++i) r.Step();
  SND_REQUIRE(r.EndOf(p1) != nullptr);
  // p1 as a whole (before and after the rebuild) must be r1's material at one alignment.
  const std::size_t f0 = FirstNonZero(r, 0, 1e-4f);
  SND_REQUIRE(f0 != SIZE_MAX);
  double best = 1e9;
  std::size_t at = 0;
  for (std::size_t s0 = f0 >= 32 ? f0 - 32 : 0; s0 < f0 + 32; ++s0) {
    double m = 0;
    for (std::size_t i = 0; i < 2000; ++i) m = std::fmax(m, std::fabs(r.At(s0 + i, 0) - 0.70710678 * e1[i]));
    if (m < best) {
      best = m;
      at = s0;
    }
  }
  double tail = 0;
  std::size_t firstBad = SIZE_MAX;
  for (std::size_t i = 0; at + i < r.Frames() && i < e1.size(); ++i) {
    const double d = std::fabs(r.At(at + i, 0) - 0.70710678 * e1[i]);
    if (d > 1e-3 && firstBad == SIZE_MAX) firstBad = i;
    tail = std::fmax(tail, d);
  }
  std::printf("  earlier playback: head err %.3g, whole err %.3g, first bad frame %lld (rebuild at output frame %zu)\n", best,
              tail, firstBad == SIZE_MAX ? -1LL : static_cast<long long>(firstBad), cut - at);
  SND_CHECK_MSG(tail < 1e-3, "the earlier playback leaves its content at frame %zu", firstBad);
  // A playback started now reads the rebuilt file.
  r.all.clear();
  const snd_playback_handle p2 = r.Play("s");
  for (int i = 0; i < 1200 && r.EndOf(p2) == nullptr; ++i) r.Step();
  const int w = Which(r, 0, e1, e2);
  SND_CHECK_MSG(w == 2, "the later playback plays %s", w == 1 ? "the old file" : "neither file");
  r.Steps(10);
  SND_CHECK_EQ(r.fs.OpenFiles(), 0u);
}

SND_TEST(IndepSourceReload3, ManyPlaysLeaveNoFileOpen) {
  Built b = Build("rl7", {StreamSpec(&Ramp, 12000)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.maxStreams = 4;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  std::uint32_t worst = 0;
  for (int i = 0; i < 200; ++i) {
    if (i % 3 == 0) r.Play("s");
    r.Step();
    if (r.fs.OpenFiles() > worst) worst = r.fs.OpenFiles();
    if (i % 50 == 0) r.all.clear();
  }
  r.Steps(80);
  std::printf("  most open files %u (4 stream slots); at the end %u\n", worst, r.fs.OpenFiles());
  SND_CHECK(worst <= 5u);
  SND_CHECK_EQ(r.fs.OpenFiles(), 0u);
}
