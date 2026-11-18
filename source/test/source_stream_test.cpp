// Stream kind through core: stream files built by buildtool, read by the IO
// scheduler from the memory FS (latency and failures injected), decoded by
// prefetch jobs. Analytic checks against the codec's own decode.
#include <cmath>
#include <cstdio>
#include <vector>

#include "reference.hpp"
#include "source_harness.hpp"

using namespace srctest;

namespace {
constexpr double kMono = 0.70710678;

AssetSpec StreamAsset(const char* name, std::uint64_t frames, const char* codec, double (*f)(std::uint32_t, std::uint64_t) = &Ramp) {
  AssetSpec a;
  a.name = name;
  a.pcm = Signal(1, 48000, frames, f);
  a.loadForm = "stream";
  a.codec = codec;
  return a;
}

// Best alignment of `played` (already through the resampler) near `f0`.
std::size_t Align(const Rig& r, std::size_t f0, const std::vector<float>& played) {
  std::size_t best = f0;
  double bestErr = 1e9;
  const std::vector<float> head(played.begin(), played.begin() + (played.size() < 3000 ? played.size() : 3000));
  for (std::size_t s = f0 >= 64 ? f0 - 64 : 0; s < f0 + 64; ++s) {
    const Fit f = FitGain(r, 0, s, head);
    if (f.maxErr < bestErr) {
      bestErr = f.maxErr;
      best = s;
    }
  }
  return best;
}

}  // namespace

SND_TEST(SourceStream, PcmWithPreloadStartsAtOnceAndPlaysExactly) {
  const std::uint64_t n = 60000;
  Built b = Build("spcm", {StreamAsset("st", n, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  SND_REQUIRE_EQ(b.files.size(), 1u);
  Options o;
  o.fsLatency = 3;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle p = r.Play("st");
  r.Steps(280);
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  SND_CHECK(f0 < 512);  // the preload (100 ms) hides the IO: the stream starts in its first block
  const Fit fit = FitPlayed(r, 0, f0, Chan(Signal(1, 48000, n, &Ramp), 0));
  SND_CHECK_NEAR(fit.gain, kMono, 0.01);
  SND_CHECK_MSG(fit.maxErr < 1e-5, "max error %g", fit.maxErr);
  const EndRec* e = r.EndOf(p);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(e->reason, SND_END_COMPLETED);
  r.Steps(4);
  const snd::source::RuntimeStats s = r.runtime.Stats();
  SND_CHECK_EQ(s.streamInstances, 0u);
  SND_CHECK_EQ(s.decodersInUse, 0u);
  SND_CHECK_EQ(s.streamStalls, 0u);
  SND_CHECK_EQ(r.pool.Used(), 0u);  // every compressed chunk went back
  SND_CHECK_EQ(r.io.Channel(0).Outstanding(), 0u);
}

SND_TEST(SourceStream, WithoutPreloadPrimesThenPlaysExactly) {
  AssetSpec a = StreamAsset("np", 30000, "PC16");
  a.extra = "\"preloadMs\": 0";
  Built b = Build("snopre", {a});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.fsLatency = 2;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  const snd_playback_handle p = r.Play("np");
  r.Step();
  r.sys->Update();
  SND_CHECK_EQ(r.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PRIMING);
  r.Steps(200);
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_REQUIRE(f0 != SIZE_MAX);
  SND_CHECK(f0 >= 256);  // waited for the header and the first reads
  const std::vector<float> pl = Resampled(Chan(a.pcm, 0), 30064);
  const std::size_t at = Align(r, f0, pl);  // a late start rings 16 frames ahead (resampler look-ahead)
  const Fit ff = FitGain(r, 0, at, pl);
  SND_CHECK(ff.maxErr < 1e-5);
  if (ff.maxErr >= 1e-5) {
    int shown = 0;
    for (std::size_t i = 0; i < pl.size() && shown < 6; ++i) {
      if (std::fabs(r.At(at + i, 0) - ff.gain * pl[i]) > 1e-5) { std::printf("np i=%zu out %g model %g\n", i, r.At(at+i,0), ff.gain*pl[i]); ++shown; }
    }
  }
  SND_CHECK(r.EndOf(p) != nullptr);
}

SND_TEST(SourceStream, AdpcmAndOpusMatchTheCodec) {
  std::vector<AssetSpec> as;
  as.push_back(StreamAsset("sad", 50000, "ADPC", &Sine1k));
  if (snd::codec::OpusAvailable()) as.push_back(StreamAsset("sop", 50000, "OPUS", &Sine1k));
  Built b = Build("scodec", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (std::uint32_t row = 0; row < as.size(); ++row) {
    const Decoded ref = Reference(b, as[row].name);
    SND_REQUIRE(ref.ok);
    Options o;
    o.fsLatency = 2;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p = r.Play(as[row].name.c_str());
    r.Steps(250);
    const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
    SND_REQUIRE(f0 != SIZE_MAX);
    SND_CHECK(f0 < 512);
    const std::vector<float> played = Resampled(ref.ch[0], ref.ch[0].size() + 64);
    const std::size_t at = Align(r, f0, played);
    const Fit fit = FitGain(r, 0, at, played);
    SND_CHECK_NEAR(fit.gain, kMono, 0.01);
    SND_CHECK_MSG(fit.maxErr < 1e-5, "%s: max error %g", as[row].name.c_str(), fit.maxErr);
    SND_CHECK(r.EndOf(p) != nullptr);
    SND_CHECK_EQ(r.runtime.Stats().streamStalls, 0u);
  }
}

SND_TEST(SourceStream, LoopsWrapSampleExactlyAcrossReads) {
  std::vector<AssetSpec> as;
  AssetSpec pc = StreamAsset("lpc", 20000, "PC16");
  pc.extra = "\"loop\": {\"start\": 3000, \"end\": 15001}";
  as.push_back(pc);
  AssetSpec ad = StreamAsset("lad", 20000, "ADPC", &Sine1k);
  ad.extra = "\"loop\": {\"start\": 3000, \"end\": 15001}";
  as.push_back(ad);
  if (snd::codec::OpusAvailable()) {
    AssetSpec op = ad;
    op.name = "lop";
    op.codec = "OPUS";
    as.push_back(op);
  }
  Built b = Build("sloop", as);
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  for (std::uint32_t row = 0; row < as.size(); ++row) {
    const Decoded ref = Reference(b, as[row].name);
    SND_REQUIRE(ref.ok);
    Options o;
    o.chunkBytes = 4u << 10;  // many reads per loop
    o.fsLatency = 1;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p = r.Play(as[row].name.c_str());
    r.Steps(300);  // ~77 k frames: 6 loops
    const std::size_t f0 = FirstNonZero(r, 0, 1e-7f);
    SND_REQUIRE(f0 != SIZE_MAX);
    const std::vector<float> model = Resampled(LoopModel(ref.ch[0], 3000, 15001, r.Frames() + 64), r.Frames() - f0 - 64);
    const std::size_t at = Align(r, f0, model);
    const Fit fit = FitGain(r, 0, at, model);
    const double tol = as[row].codec == "OPUS" ? 1e-3 : 1e-5;
    SND_CHECK_MSG(fit.maxErr < tol, "%s: loop max error %g", as[row].name.c_str(), fit.maxErr);
    if (fit.maxErr >= tol) {
      int shown = 0;
      for (std::size_t i = 0; i < model.size() && shown < 6; ++i) {
        if (std::fabs(r.At(at + i, 0) - fit.gain * model[i]) > tol) { std::printf("i=%zu out %g model %g\n", i, r.At(at+i,0), fit.gain*model[i]); ++shown; }
      }
    }
    SND_CHECK_EQ(r.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
    SND_CHECK_EQ(r.runtime.Stats().streamStalls, 0u);
    r.sys->Stop(p, 0.0f, nullptr);
    r.Steps(8);
    SND_CHECK_EQ(r.runtime.Stats().streamInstances, 0u);
    SND_CHECK_EQ(r.pool.Used(), 0u);
  }
}

// Underrun (6.3): the reads stall, the voice fades out and reports starved, and
// resumes. Sound priority continues where it stopped (the playback gets longer
// by the stall), time priority skips the elapsed frames (same end time).
SND_TEST(SourceStream, UnderrunSoundAndTimePriority) {
  const std::uint64_t n = 96000;
  AssetSpec snd_ = StreamAsset("snd", n, "PC16");
  snd_.extra = "\"stallPolicy\": \"sound\"";
  AssetSpec tim = StreamAsset("tim", n, "PC16");
  Built b = Build("stall", {snd_, tim});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  std::size_t lastNz[2] = {0, 0}, firstNz[2] = {0, 0};
  std::uint64_t stalls[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    Options o;
    o.fsLatency = 1;
    o.chunkBytes = 4u << 10;  // little compressed read-ahead: the stall comes soon after the hold
    o.ringChunks = 2;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    std::string path;
    for (const auto& f : b.files) {
      if (f.first.find(k == 0 ? "/snd_" : "/tim_") != std::string::npos) path = f.first;
    }
    SND_REQUIRE(!path.empty());
    const snd_playback_handle p = r.Play(k == 0 ? "snd" : "tim");
    r.Steps(40);
    r.fs.Hold(path);
    r.Steps(250);  // 1.3 s without data: much longer than the PCM ring (0.34 s) + read-ahead
    SND_CHECK_EQ(r.sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
    r.fs.Release(path);
    r.Steps(700);
    SND_CHECK(r.EndOf(p) != nullptr);
    firstNz[k] = FirstNonZero(r, 0);
    for (std::size_t f = r.Frames(); f-- > 0;) {
      if (std::fabs(r.At(f, 0)) > 1e-6f) {
        lastNz[k] = f;
        break;
      }
    }
    stalls[k] = r.runtime.Stats().streamStalls;
    SND_CHECK(stalls[k] >= 1u);
    SND_CHECK(r.runtime.Stats().streamStallBlocks > 10u);
    // There is a silent gap (the stall) after the start.
    std::size_t silent = 0, longest = 0;
    for (std::size_t f = firstNz[k]; f < lastNz[k]; ++f) {
      silent = std::fabs(r.At(f, 0)) < 1e-6f ? silent + 1 : 0;
      if (silent > longest) longest = silent;
    }
    SND_CHECK(longest > 20000);
  }
  const double lenSound = double(lastNz[0] - firstNz[0]);
  const double lenTime = double(lastNz[1] - firstNz[1]);
  SND_CHECK_MSG(lenSound > double(n) + 20000.0, "sound priority length %g", lenSound);
  SND_CHECK_MSG(std::fabs(lenTime - double(n)) < 2048.0, "time priority length %g", lenTime);
}

SND_TEST(SourceStream, ReadErrorAndMissingFileEndTheVoice) {
  Built b = Build("sfail", {StreamAsset("bad", 96000, "PC16"), StreamAsset("gone", 96000, "PC16")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.fsLatency = 1;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  r.Load(b);
  std::string badPath, gonePath;
  for (const auto& f : b.files) (f.first.find("bad") != std::string::npos ? badPath : gonePath) = f.first;
  r.fs.FailReads(badPath, 60000, UINT64_MAX);  // reads past ~0.6 s fail
  r.fs.RemoveFile(gonePath);
  const snd_playback_handle pb = r.Play("bad");
  const snd_playback_handle pg = r.Play("gone");
  r.Steps(200);
  SND_CHECK(r.EndOf(pb) != nullptr);
  SND_CHECK(r.EndOf(pg) != nullptr);
  SND_CHECK(r.runtime.Stats().streamIoErrors >= 2u);
  // "bad" played its first part, then ended long before its 2 s.
  const std::size_t f0 = FirstNonZero(r, 0);
  SND_CHECK(f0 != SIZE_MAX);
  SND_CHECK(std::fabs(r.At(r.Frames() - 1, 0)) < 1e-7f);
  r.Steps(4);
  SND_CHECK_EQ(r.runtime.Stats().streamInstances, 0u);
  SND_CHECK_EQ(r.pool.Used(), 0u);
}

SND_TEST(SourceStream, SlotAndDecoderLimitsRefuseWithReason) {
  Built b = Build("slimit", {StreamAsset("a", 96000, "PC16"), StreamAsset("d", 96000, "ADPC")});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  {
    Options o;
    o.maxStreams = 2;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p1 = r.Play("a"), p2 = r.Play("a"), p3 = r.Play("a");
    r.Steps(4);
    SND_CHECK(r.EndOf(p1) == nullptr && r.EndOf(p2) == nullptr);
    const EndRec* e = r.EndOf(p3);
    SND_REQUIRE(e != nullptr);
    SND_CHECK_EQ(e->reason, SND_END_REJECTED);
    SND_CHECK_EQ(e->detail, SND_END_DETAIL_NO_STREAM_SLOT);
  }
  {
    Options o;
    o.maxDecoders = 1;
    Rig r(o);
    SND_REQUIRE(r.Ok());
    r.Load(b);
    const snd_playback_handle p1 = r.Play("d"), p2 = r.Play("d");
    r.Steps(4);
    SND_CHECK(r.EndOf(p1) == nullptr);
    const EndRec* e = r.EndOf(p2);
    SND_REQUIRE(e != nullptr);
    SND_CHECK_EQ(e->detail, SND_END_DETAIL_DECODER_POOL_FULL);
  }
}

SND_TEST(SourceStream, UnloadWhileStreamingReturnsEverything) {
  Built b = Build("sunload", {StreamAsset("u", 200000, "ADPC", &Sine1k)});
  SND_REQUIRE_MSG(b.ok, "%s", b.diag.c_str());
  Options o;
  o.fsLatency = 4;
  Rig r(o);
  SND_REQUIRE(r.Ok());
  const snd_bank_handle bank = r.Load(b);
  const snd_playback_handle p1 = r.Play("u");
  const snd_playback_handle p2 = r.Play("u");
  r.Steps(30);
  SND_CHECK_EQ(r.runtime.Stats().streamInstances, 2u);
  SND_CHECK_EQ(r.sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 100 && r.sys->GetBankState(bank) != SND_BANK_STATE_UNLOADED; ++i) r.Step();
  SND_CHECK_EQ(r.sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
  const EndRec* e = r.EndOf(p1);
  SND_REQUIRE(e != nullptr);
  SND_CHECK_EQ(e->reason, SND_END_DISCARDED);
  SND_CHECK(r.EndOf(p2) != nullptr);
  r.Steps(10);
  SND_CHECK_EQ(r.runtime.Stats().streamInstances, 0u);
  SND_CHECK_EQ(r.runtime.Stats().decodersInUse, 0u);
  SND_CHECK_EQ(r.pool.Used(), 0u);
}
