// Sample kind through core (project -> buildtool -> bank -> core + source):
// analytic checks of level, start, length, loops, pitch, rate, channels and
// the load forms (PCM resident, compressed ADPCM / Opus, PCM cache).
#include <cmath>
#include <cstdio>
#include <vector>

#include "reference.hpp"
#include "source_harness.hpp"

using namespace srctest;

namespace {
constexpr double kMono = 0.70710678;  // mono voice on the stereo master (equal power)

AssetSpec RampAsset(const char* name, std::uint64_t frames, const char* form = "pcm", const char* codec = "PC16") {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, &Ramp);
  a.loadForm = form;
  a.codec = codec;
  return a;
}
}  // namespace

SND_TEST(SourceSample, PcmResidentPlaysExactlyAndEnds) {
  const std::uint64_t n = 9000;
  Built b = Build("pcm", {RampAsset("a", n)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle p = r.Play("a");
  r.Steps(60);
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  SND_CHECK(f0 < 512);
  const Fit fit = FitPlayed(r, 0, f0, Chan(Signal(1, 48000, n, &Ramp), 0));
  SND_CHECK_NEAR(fit.gain, kMono, 0.01);
  SND_CHECK(fit.maxErr < 1e-5);
  // Length: the n frames (plus the resampler's 16-frame look-ahead ringing), then silence.
  SND_CHECK(std::fabs(r.At(f0 + n - 1, 0)) > 0.05f);
  for (std::size_t i = f0 + n + 16; i < f0 + n + 600; ++i) SND_CHECK(std::fabs(r.At(i, 0)) < 1e-7f);
  const EndRec* e = r.EndOf(p);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(e->reason, SND_END_COMPLETED);
  SND_CHECK_EQ(r.runtime.Stats().sampleInstances, 0u);
}

SND_TEST(SourceSample, StereoChannelsStaySeparate) {
  AssetSpec a;
  a.name = "s";
  a.pcm = Signal(2, 48000, 6000, &Ramp);  // L positive ramp, R negative ramp
  a.codec = "PCMF";
  Built b = Build("stereo", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  r.Play("s");
  r.Steps(40);
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  const Fit l = FitPlayed(r, 0, f0, Chan(a.pcm, 0));
  const Fit rr = FitPlayed(r, 1, f0, Chan(a.pcm, 1));
  SND_CHECK(l.gain > 0.5 && rr.gain > 0.5);
  SND_CHECK(l.maxErr < 1e-5 && rr.maxErr < 1e-5);
  SND_CHECK(r.At(f0 + 10, 0) > 0.0f && r.At(f0 + 10, 1) < 0.0f);
}

SND_TEST(SourceSample, LoopPointsAreSampleExact) {
  AssetSpec a = RampAsset("l", 9000);
  a.extra = "\"loop\": {\"start\": 1000, \"end\": 7001}";
  Built b = Build("loop", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle p = r.Play("l");
  r.Steps(160);  // ~41 k frames: 6 loops
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  const std::vector<float> model = LoopModel(Chan(a.pcm, 0), 1000, 7001, r.Frames() - f0 + 64);
  const Fit fit = FitGain(r, 0, f0, Resampled(model, r.Frames() - f0));
  SND_CHECK_NEAR(fit.gain, kMono, 0.01);
  SND_CHECK(fit.maxErr < 1e-5);
  SND_CHECK_EQ(r.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
}

SND_TEST(SourceSample, LoopSeamOfASineIsContinuous) {
  AssetSpec a;
  a.name = "sine";
  a.pcm = Signal(1, 48000, 6000, &Sine1k);
  a.codec = "PCMF";
  a.extra = "\"loop\": {\"start\": 480, \"end\": 4800}";  // 90 periods of 1 kHz
  Built b = Build("seam", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  r.Play("sine");
  r.Steps(100);
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  // |x[n+1] - x[n]| <= 2 pi f / fs * A for a sine of amplitude A (plus rounding).
  const double bound = 6.283185307179586 * 1000.0 / 48000.0 * 0.5 * kMono * 1.05 + 1e-5;  // sinc ripple
  double worst = 0;
  for (std::size_t i = f0 + 1; i + 1 < r.Frames(); ++i) {
    worst = std::fmax(worst, std::fabs(double(r.At(i, 0)) - r.At(i - 1, 0)));
  }
  SND_CHECK_MSG(worst <= bound, "worst step %g > %g", worst, bound);
  SND_CHECK_NEAR(Tone(r, 0, f0 + 4000, 16384, 1000.0), 0.5 * kMono, 0.01);
}

SND_TEST(SourceSample, CompressedAdpcmAndOpusMatchTheCodec) {
  std::vector<AssetSpec> as;
  AssetSpec ad = RampAsset("adpcm", 20000, "compressed", "ADPC");
  ad.pcm = Signal(1, 48000, 20000, &Sine1k);
  as.push_back(ad);
  const bool opus = snd::codec::OpusAvailable();
  if (opus) {
    AssetSpec op = ad;
    op.name = "opus";
    op.codec = "OPUS";
    as.push_back(op);
  }
  Built b = Build("comp", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (std::uint32_t row = 0; row < as.size(); ++row) {
    const Decoded ref = Reference(b, as[row].name);
    SND_REQUIRE(ref.ok);
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p = r.Play(as[row].name.c_str());
    r.Steps(100);
    const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
    SND_REQUIRE(f0 != SIZE_MAX);
    SND_CHECK(f0 < 512);  // primed at Start: no extra latency
    // Find the exact alignment (the first decoded frames may be ~0 for Opus).
    const std::vector<float> played = Resampled(ref.ch[0], ref.ch[0].size() + 64);
    std::size_t best = f0;
    double bestErr = 1e9;
    for (std::size_t s = f0 >= 64 ? f0 - 64 : 0; s < f0 + 64; ++s) {
      const Fit f = FitGain(r, 0, s, std::vector<float>(played.begin(), played.begin() + 4000));
      if (f.maxErr < bestErr) {
        bestErr = f.maxErr;
        best = s;
      }
    }
    const Fit fit = FitGain(r, 0, best, played);
    SND_CHECK_NEAR(fit.gain, kMono, 0.01);
    SND_CHECK_MSG(fit.maxErr < 1e-5, "%s: max error %g", as[row].name.c_str(), fit.maxErr);
    SND_CHECK_NEAR(Tone(r, 0, best + 2048, 8192, 1000.0), 0.5 * kMono, 0.02);
    SND_CHECK(r.EndOf(p) != nullptr);
    SND_CHECK_EQ(r.runtime.Stats().decodersInUse, 0u);
  }
}

SND_TEST(SourceSample, CompressedLoopIsSampleExact) {
  std::vector<AssetSpec> as;
  AssetSpec ad = RampAsset("adl", 12000, "compressed", "ADPC");
  ad.pcm = Signal(1, 48000, 12000, &Sine1k);
  ad.extra = "\"loop\": {\"start\": 2000, \"end\": 9000}";
  as.push_back(ad);
  if (snd::codec::OpusAvailable()) {
    AssetSpec op = ad;
    op.name = "opl";
    op.codec = "OPUS";
    as.push_back(op);
  }
  Built b = Build("comploop", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (std::uint32_t row = 0; row < as.size(); ++row) {
    const Decoded ref = Reference(b, as[row].name);
    SND_REQUIRE(ref.ok);
    SND_REQUIRE_EQ(ref.loopEnd, 9000u);
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    r.Play(as[row].name.c_str());
    r.Steps(150);
    const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
    SND_REQUIRE(f0 != SIZE_MAX);
    const std::vector<float> model = Resampled(LoopModel(ref.ch[0], 2000, 9000, r.Frames()), r.Frames() - f0 - 64);
    std::size_t best = f0;
    double bestErr = 1e9;
    for (std::size_t s = f0 >= 64 ? f0 - 64 : 0; s < f0 + 64; ++s) {
      const Fit f = FitGain(r, 0, s, std::vector<float>(model.begin(), model.begin() + 4000));
      if (f.maxErr < bestErr) {
        bestErr = f.maxErr;
        best = s;
      }
    }
    const Fit fit = FitGain(r, 0, best, model);
    // ADPCM resumes exactly at block boundaries; Opus (no inter-packet
    // prediction, 80 ms pre-roll, D-60) bit-exactly after the pre-roll.
    const double tol = as[row].codec == "OPUS" ? 1e-3 : 1e-5;
    SND_CHECK_MSG(fit.maxErr < tol, "%s: loop max error %g", as[row].name.c_str(), fit.maxErr);
  }
}

SND_TEST(SourceSample, DecompressCacheFillsOnceAndIsShared) {
  AssetSpec a = RampAsset("c", 30000, "decompressCache", "ADPC");
  a.pcm = Signal(1, 48000, 30000, &Sine1k);
  Built b = Build("cache", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  const Decoded ref = Reference(b, a.name);
  SND_REQUIRE(ref.ok);
  Rig r;
  SND_REQUIRE(r.Ok());
  r.Load(b);
  r.Play("c");
  r.Steps(200);
  const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
  SND_REQUIRE(f0 != SIZE_MAX);
  SND_CHECK(f0 < 512);
  const Fit fit = FitPlayed(r, 0, f0, ref.ch[0]);
  SND_CHECK(fit.maxErr < 1e-5);
  snd::source::RuntimeStats s = r.runtime.Stats();
  SND_CHECK_EQ(s.cacheMisses, 1u);
  SND_CHECK_EQ(s.cacheEntries, 1u);
  SND_CHECK_EQ(s.decodersInUse, 0u);  // the fill finished and returned its decoder
  // Second play: from the cache (no decoder), same output.
  r.all.clear();
  r.Play("c");
  r.Steps(200);
  s = r.runtime.Stats();
  SND_CHECK_EQ(s.cacheHits, 1u);
  const std::size_t f1 = FirstNonZero(r, 0, 1e-7f);
  SND_REQUIRE(f1 != SIZE_MAX);
  SND_CHECK(FitPlayed(r, 0, f1, ref.ch[0]).maxErr < 1e-5);
}

SND_TEST(SourceSample, StartOffsetIsSampleAccurate) {
  AssetSpec a = RampAsset("a", 4000);
  AssetSpec d = RampAsset("d", 4000);
  d.clip = "\"startMs\": 10";
  Built b = Build("start", {a, d});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::size_t first[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    r.Play(k == 0 ? "a" : "d");
    r.Steps(30);
    first[k] = FirstNonZero(r, 0);
    SND_REQUIRE(first[k] != SIZE_MAX);
    SND_CHECK(FitPlayed(r, 0, first[k], Chan(a.pcm, 0)).maxErr < 1e-5);
  }
  SND_CHECK_EQ(first[1] - first[0], 480u);
}

SND_TEST(SourceSample, PitchAndSourceRate) {
  AssetSpec up;
  up.name = "up";
  up.pcm = Signal(1, 48000, 48000, &Sine1k);
  up.codec = "PCMF";
  up.clip = "\"pitchCents\": 1200";
  AssetSpec slow;
  slow.name = "r24";
  slow.pcm = Signal(1, 24000, 24000, [](std::uint32_t, std::uint64_t i) {
    return 0.5 * std::sin(6.283185307179586 * 1000.0 * static_cast<double>(i) / 24000.0);
  });
  slow.codec = "PCMF";
  Built b = Build("pitch", {up, slow});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  {
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    r.Play("up");
    r.Steps(120);
    const std::size_t f0 = FirstNonZero(r, 0);
    SND_REQUIRE(f0 != SIZE_MAX);
    SND_CHECK_NEAR(Tone(r, 0, f0 + 1024, 8192, 2000.0), 0.5 * kMono, 0.02);
    SND_CHECK(Tone(r, 0, f0 + 1024, 8192, 1000.0) < 0.01);
    // Twice as fast: 48000 source frames in ~24000 output frames.
    SND_CHECK(std::fabs(r.At(f0 + 23000, 0)) + std::fabs(r.At(f0 + 23001, 0)) > 0.01f);
    SND_CHECK(std::fabs(r.At(f0 + 24100, 0)) < 1e-6f);
  }
  {
    Rig r;
    SND_REQUIRE(r.Ok());
    r.Load(b);
    r.Play("r24");
    r.Steps(220);
    const std::size_t f0 = FirstNonZero(r, 0);
    SND_REQUIRE(f0 != SIZE_MAX);
    SND_CHECK_NEAR(Tone(r, 0, f0 + 4096, 16384, 1000.0), 0.5 * kMono, 0.02);
    // 24000 source frames at 24 kHz last 48000 output frames.
    SND_CHECK(std::fabs(r.At(f0 + 47000, 0)) + std::fabs(r.At(f0 + 47001, 0)) > 0.01f);
    SND_CHECK(std::fabs(r.At(f0 + 48100, 0)) < 1e-6f);
  }
}

SND_TEST(SourceSample, DecoderPoolFullRefusesWithReason) {
  AssetSpec a = RampAsset("x", 48000, "compressed", "ADPC");
  Built b = Build("full", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.maxDecoders = 2;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle p1 = r.Play("x");
  const snd_playback_handle p2 = r.Play("x");
  const snd_playback_handle p3 = r.Play("x");
  r.Steps(4);
  SND_CHECK(r.EndOf(p1) == nullptr);
  SND_CHECK(r.EndOf(p2) == nullptr);
  const EndRec* e = r.EndOf(p3);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(e->detail, SND_END_DETAIL_DECODER_POOL_FULL);
  SND_CHECK_EQ(r.runtime.Stats().rejectedDecoder, 1u);
  // A slot comes back when a voice ends: a new play is accepted.
  r.sys->Stop(p1, 0.0f, nullptr);
  r.Steps(6);
  const snd_playback_handle p4 = r.Play("x");
  r.Steps(4);
  SND_CHECK(r.EndOf(p4) == nullptr);
}

// The PCM cache never serves a bank that was unloaded: a new bank in the same
// slot whose asset has the same name and length but other content plays its
// own audio (the cache key holds the bank load generation).
SND_TEST(SourceSample, CacheDoesNotServeAnUnloadedBank) {
  AssetSpec a1 = RampAsset("c", 30000, "decompressCache", "ADPC");
  AssetSpec a2 = a1;
  a2.pcm = Signal(1, 48000, 30000, &Sine1k);
  Built b1 = Build("cgen1", {a1});
  Built b2 = Build("cgen2", {a2});
  SND_REQUIRE_MSG(b1.ok && b2.ok, "%s %s", b1.diag.c_str(), b2.diag.c_str());
  const Decoded r2 = Reference(b2, "c");
  SND_REQUIRE(r2.ok);
  Rig r;
  SND_REQUIRE(r.Ok());
  const snd_bank_handle h1 = r.LoadAs(b1, "One.sndb");
  const snd_playback_handle p1 = r.Play("c");
  for (int i = 0; i < 300 && r.EndOf(p1) == nullptr; ++i) r.Step();
  SND_REQUIRE(r.EndOf(p1) != nullptr);
  SND_CHECK_EQ(r.sys->UnloadBank(h1, SND_UNLOAD_IF_UNUSED), SND_OK);
  for (int i = 0; i < 200 && r.sys->GetBankState(h1) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  SND_REQUIRE_EQ(r.sys->GetBankState(h1), SND_BANK_STATE_UNLOADED);
  r.LoadAs(b2, "Two.sndb");
  r.Play("c");
  r.Steps(200);
  const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
  SND_REQUIRE(f0 != SIZE_MAX);
  const std::vector<float> played = Resampled(r2.ch[0], r2.ch[0].size() + 64);
  Fit best{0, 1e9};
  for (std::size_t s = f0 >= 32 ? f0 - 32 : 0; s < f0 + 32; ++s) {
    const Fit f = FitGain(r, 0, s, played);
    if (f.maxErr < best.maxErr) best = f;
  }
  SND_CHECK_MSG(best.maxErr < 1e-5, "second bank: err %g", best.maxErr);
  SND_CHECK_EQ(r.runtime.Stats().cacheHits, 0u);
}
