// Stage 3 group 2 (core): runtime bus-effect parameters (D-130), the compressor
// sidechain from DUCK rows (D-131), voice effects in the voice-effect pool
// (D-132..D-134), the standard voice LP / HP through core, the LOD bypass, and the
// determinism / allocation / RT / late-chunk properties of these paths.
// Expectations are analytic (levels, frequencies, ParamBlock values).
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/test/rt_guard.hpp"

using namespace coretest;
using snd::HashId;

namespace {
constexpr float kCenter = 0.70710678f;  // mono -> stereo (constant power centre)
constexpr double kRate = 48000.0;

// ---------------------------------------------------------------------------
// A recording plugin: out = in * param[0] (ramped), logs the ParamBlocks it gets.
// ---------------------------------------------------------------------------
struct BlockLog {
  float start[4];
  float end[4];
  std::uint32_t count;
  std::uint32_t mask;
  std::uint64_t playback;
  std::uint32_t channels;
};
struct GainPlugin {
  std::vector<BlockLog> log;
  std::uint32_t inits = 0;
  snd_dsp_effect table{};
  explicit GainPlugin(snd::Id type) {
    log.reserve(1u << 16);  // process() must not allocate (RT scope)
    table.user = this;
    table.effect_type = type;
    table.state_bytes = [](void*, const snd_effect_desc*) -> std::uint32_t { return 64u; };
    table.init = [](void* u, void* state, const snd_effect_desc* d, std::uint32_t) {
      ++static_cast<GainPlugin*>(u)->inits;
      std::memset(state, 0, 64);
      static_cast<std::uint32_t*>(state)[0] = d->channels;
    };
    table.process = [](void* u, void*, const float* const* in, float* const* out, std::uint32_t ch, std::uint32_t n,
                       const snd_param_block* p) {
      BlockLog l{};
      l.count = p != nullptr ? p->count : 0u;
      l.mask = p != nullptr ? p->changed_mask : 0u;
      l.channels = ch;
      for (std::uint32_t i = 0; i < 4 && p != nullptr && i < p->count; ++i) {
        l.start[i] = p->start[i];
        l.end[i] = p->end[i];
      }
      l.playback = p != nullptr && p->ctx != nullptr ? p->ctx->playback.value : 0u;
      static_cast<GainPlugin*>(u)->log.push_back(l);
      const float g0 = p != nullptr && p->count > 0 ? p->start[0] : 1.0f;
      const float g1 = p != nullptr && p->count > 0 ? p->end[0] : 1.0f;
      for (std::uint32_t c = 0; c < ch; ++c) {
        for (std::uint32_t f = 0; f < n; ++f) out[c][f] = in[c][f] * (g0 + (g1 - g0) * float(f + 1) / float(n));
      }
    };
    table.is_tail_done = [](void*, const void*) -> std::int32_t { return 1; };
  }
};

struct Warnings {
  std::vector<std::uint32_t> details;
  std::vector<snd_result> codes;
  std::uint32_t Count(snd_result code, std::uint32_t detail) const {
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < codes.size(); ++i) n += codes[i] == code && details[i] == detail ? 1u : 0u;
    return n;
  }
};
void CatchWarnings(Harness& h, Warnings* w) {
  snd_callbacks cb{};
  cb.user = w;
  cb.on_error = [](void* u, snd_result r, std::uint32_t detail, std::uint64_t) {
    static_cast<Warnings*>(u)->codes.push_back(r);
    static_cast<Warnings*>(u)->details.push_back(detail);
  };
  h.sys->SetCallbacks(&cb);
}

double Amp(const Harness& h, std::size_t from, std::size_t n, double hz, std::uint32_t ch = 0) {
  return ToneAmplitude(h.all, h.Ch(), ch, from, n, hz, kRate);
}
double MaxAbs(const Harness& h, std::size_t from, std::size_t to, std::uint32_t ch = 0) {
  double m = 0;
  for (std::size_t f = from; f < to && f < h.TotalFrames(); ++f) m = std::max(m, double(std::fabs(h.At(f, ch))));
  return m;
}
std::vector<float> Mix(std::size_t frames, std::initializer_list<double> hz, float amp) {
  std::vector<float> d(frames, 0.0f);
  for (double f : hz) {
    const std::vector<float> s = Sine(frames, f, kRate, amp);
    for (std::size_t i = 0; i < frames; ++i) d[i] += s[i];
  }
  return d;
}
float Db(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

// ===========================================================================
// A. D-130: bus effect parameters at run time
// ===========================================================================
SND_TEST(CoreFx, BusEffectParamBlocksRampToTheTargetWithTheMask) {
  GainPlugin plug(HashId("test.gain"));
  snd_config cfg = SmallConfig();
  cfg.plugins.effects = &plug.table;
  cfg.plugins.effect_count = 1;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.effects.push_back(TEffect{HashId("test.gain"), {1.0f, 7.0f}});
  b.buses[0].effects.assign(1, 0u);
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(480000, 0.5f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(2);
  plug.log.clear();
  // Fade param 0 from 1 to 0 over exactly 4 blocks (1024 frames; the fade is truncated to frames).
  const float fade = (4.0f * 256.0f + 0.5f) / 48000.0f;
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 0.0f, fade), SND_OK);
  h.Steps(6);
  SND_REQUIRE(plug.log.size() == 6u);
  const float want[5] = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
  for (int k = 0; k < 4; ++k) {
    SND_CHECK_NEAR(plug.log[k].start[0], want[k], 1e-6f);
    SND_CHECK_NEAR(plug.log[k].end[0], want[k + 1], 1e-6f);
    SND_CHECK_EQ(plug.log[k].mask, 1u);  // only parameter 0 changes
    SND_CHECK_EQ(plug.log[k].count, 2u);
    SND_CHECK_NEAR(plug.log[k].start[1], 7.0f, 0.0f);  // untouched parameter
  }
  SND_CHECK_EQ(plug.log[4].mask, 0u);  // reached: no change any more
  SND_CHECK_NEAR(plug.log[4].start[0], 0.0f, 0.0f);
  SND_CHECK_EQ(plug.log[4].playback, 0u);  // bus effects: no playback
  // The output follows the gain ramp (sample by sample inside the block).
  const std::size_t f0 = 2 * 256;
  SND_CHECK_NEAR(h.At(f0 + 127, 0), 0.5f * kCenter * (1.0f - 0.25f * 128.0f / 256.0f), 1e-5f);
  SND_CHECK_NEAR(h.At(f0 + 5 * 256 + 10, 0), 0.0f, 1e-7f);
  // Fade 0: the value is reached at the end of the next block (one block ramp, no click).
  plug.log.clear();
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 1, 3.0f, 0.0f), SND_OK);
  h.Steps(2);
  SND_REQUIRE(plug.log.size() == 2u);
  SND_CHECK_NEAR(plug.log[0].start[1], 7.0f, 0.0f);
  SND_CHECK_NEAR(plug.log[0].end[1], 3.0f, 0.0f);
  SND_CHECK_EQ(plug.log[0].mask, 2u);
  SND_CHECK_EQ(plug.log[1].mask, 0u);
}

SND_TEST(CoreFx, BusEffectParamChangesTheSoundAndIsClamped) {
  Harness h;
  TestBank b = BasicBank();
  b.effects.push_back(TEffect{snd::dsp::kEffectLowpass, {20000.0f, 0.707f}});
  b.buses[0].effects.assign(1, 0u);
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(480000, {100.0, 6000.0}, 0.4f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 6000.0), 0.4 * kCenter, 0.02);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 500.0f, 0.05f), SND_OK);
  h.Steps(30);
  const std::size_t at = 46 * 256 - 4096;
  SND_CHECK(Amp(h, at, 4096, 6000.0) < 0.4 * kCenter * 0.02);  // > 30 dB down
  SND_CHECK_NEAR(Amp(h, at, 4096, 100.0), 0.4 * kCenter, 0.02);
  // A value beyond the range is clamped (22000 Hz): open again, not a broken filter.
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 1.0e9f, 0.0f), SND_OK);
  h.Steps(20);
  const std::size_t at2 = 66 * 256 - 4096;
  SND_CHECK_NEAR(Amp(h, at2, 4096, 6000.0), 0.4 * kCenter, 0.03);
  for (std::size_t f = 0; f < h.TotalFrames(); ++f) SND_CHECK(std::isfinite(h.At(f, 0)));
}

SND_TEST(CoreFx, BusEffectParamLimiterCeilingClampedToItsRange) {
  Harness h;
  TestBank b = BasicBank();
  // Limiter ceiling -12 dB; then +20 dB requested -> clamped to 0 dB.
  b.effects.push_back(TEffect{snd::dsp::kEffectLimiter, {-12.0f, 50.0f, 2.0f, 0.0f}});
  b.buses[0].effects.assign(1, 0u);
  b.assets.push_back(TAsset{HashId("s"), 1, 48000, Sine(480000, 200.0, kRate, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  h.Steps(20);
  SND_CHECK(MaxAbs(h, 2048, 20 * 256) <= Db(-12.0f) * 1.001);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 20.0f, 0.0f), SND_OK);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 3, 24.0f, 0.0f), SND_OK);  // input gain +24 dB
  h.Steps(40);
  const double m = MaxAbs(h, 30 * 256, 60 * 256);
  SND_CHECK(m <= 1.0001);   // ceiling 0 dB (clamped), never above
  SND_CHECK(m > 0.9);       // and the input (+24 dB) reaches it
}

SND_TEST(CoreFx, BusEffectParamArgumentsAndUnknownTargets) {
  Harness h;
  Warnings w;
  CatchWarnings(h, &w);
  TestBank b = BasicBank();
  b.effects.push_back(TEffect{snd::dsp::kEffectLowpass, {1000.0f, 0.707f}});
  b.buses[0].effects.assign(1, 0u);
  h.Load("b", b.Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 16, 0, 1.0f, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 16, 1.0f, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, NAN, 0.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 0, 1.0f, INFINITY), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Nope"), 0, 0, 1.0f, 0.0f), SND_OK);  // unknown when applied
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 3, 0, 1.0f, 0.0f), SND_OK);   // no 4th effect
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Sfx"), 0, 9, 1.0f, 0.0f), SND_OK);   // lowpass has 2 params
  SND_CHECK_EQ(h.sys->SetEffectParam(HashId("Master"), 0, 0, 1.0f, 0.0f), SND_OK);  // no chain
  h.Steps(2);
  SND_CHECK_EQ(w.Count(SND_ERR_NOT_FOUND, 0u), 4u);
}

// ===========================================================================
// B. D-131: compressor sidechain from DUCK kind 0
// ===========================================================================
namespace {
// Buses: row `voiceRow` "Voice" and the other "Music" (compressor, sidechain), master row 2.
// The music is a 1 kHz tone of amplitude 0.1 (below the threshold); the voice a 200 Hz
// tone of amplitude 1 starting at block `voiceBlock`.
struct ScRun {
  std::vector<float> out;
  std::uint32_t ch = 2;
};
TestBank SidechainBank(bool voiceFirst, float duckDb, std::uint32_t voiceChannels) {
  TestBank b;
  const std::uint32_t vRow = voiceFirst ? 0u : 1u, mRow = voiceFirst ? 1u : 0u;
  b.buses.resize(3);
  b.buses[vRow] = TBus{HashId("Voice"), 2u, voiceChannels, 0, 0.0f, {}, {}};
  b.buses[mRow] = TBus{HashId("Music"), 2u, 2u, 0, 0.0f, {0u}, {}};
  b.buses[2] = TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}};
  // Threshold -20 dB, ratio 4, attack 0.05 ms, release 1000 ms, knee 0, makeup 0, sc 0 dB, use sc.
  b.effects.push_back(TEffect{snd::dsp::kEffectCompressor, {-20.0f, 4.0f, 0.05f, 1000.0f, 0.0f, 0.0f, 0.0f, 1.0f}});
  TDuck d;
  d.kind = 0;
  d.source = vRow;
  d.target = mRow;
  d.amountDb = duckDb;
  d.attackMs = 0.0f;
  d.releaseMs = 0.0f;
  d.thresholdDb = -40.0f;
  b.ducks.push_back(d);
  b.assets.push_back(TAsset{HashId("music"), 1, 48000, Sine(480000, 1000.0, kRate, 0.1f), 0, 0});
  b.assets.push_back(TAsset{HashId("voice"), 1, 48000, Sine(480000, 200.0, kRate, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("M", 0, mRow));
  b.events.push_back(SimpleEvent("V", 1, vRow));
  return b;
}
}  // namespace

SND_TEST(CoreFx, SidechainCompressorFollowsTheSourceBusStaticCurve) {
  for (int first = 0; first < 2; ++first) {
    Harness h;
    h.Load("b", SidechainBank(first != 0, 0.0f, 2u).Build(HashId("B")));
    h.Clear();
    h.sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
    h.Steps(20);
    // No sidechain signal: the music (below the threshold anyway) passes untouched.
    SND_CHECK_NEAR(Amp(h, 1024, 4096, 1000.0), 0.1 * kCenter, 0.003);
    h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
    h.Steps(40);
    // Sidechain peak = 1 * kCenter (-3 dB on the voice bus): 17 dB above -20, ratio 4:
    // gain = -(17)(1 - 1/4) = -12.75 dB on the music (the voice's own level is irrelevant).
    const double scDb = 20.0 * std::log10(kCenter);
    const double wantDb = -(scDb + 20.0) * 0.75;
    const double got = Amp(h, 60 * 256 - 8192, 8192, 1000.0);
    snd::test::Note("voice first %d: music %.2f dB (want %.2f dB)", first, 20.0 * std::log10(got / (0.1 * kCenter)), wantDb);
    SND_CHECK_NEAR(20.0 * std::log10(got / (0.1 * kCenter)), wantDb, 0.5);
    SND_CHECK_NEAR(Amp(h, 60 * 256 - 8192, 8192, 200.0), kCenter, 0.01);  // the voice is not compressed
  }
}

SND_TEST(CoreFx, SidechainSameBlockOrPreviousBlockByGraphOrder) {
  // Voice mixed first: the music drops in the block where the voice starts. Voice mixed
  // after the music: one block later (previous-block buffer). Fixed by the graph.
  std::size_t onset[2] = {0, 0};
  for (int first = 0; first < 2; ++first) {
    for (int run = 0; run < 2; ++run) {
      Harness h;
      h.Load("b", SidechainBank(first != 0, 0.0f, 2u).Build(HashId("B")));
      h.Clear();
      h.sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
      h.Steps(10);
      h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
      h.Steps(4);
      // First block (after block 10) whose 1 kHz level is clearly reduced.
      std::size_t found = 0;
      for (std::size_t blk = 10; blk < 14 && found == 0; ++blk) {
        // Music alone = 0.1 * kCenter peak on the 1 kHz part: look for the compressed level
        // through the residual after removing the voice tone is hard; use the sidechain-free
        // channel energy at 1 kHz over the block.
        const double a = ToneAmplitude(h.all, 2, 0, blk * 256 + 64, 192, 1000.0, kRate);
        if (a < 0.1 * kCenter * 0.6) found = blk;
      }
      if (run == 0) onset[first] = found;
      SND_CHECK_EQ(found, run == 0 ? found : onset[first]);  // the same every run
    }
  }
  snd::test::Note("onset block: voice mixed later %zu, voice mixed first %zu", onset[0], onset[1]);
  SND_CHECK_EQ(onset[1], 10u);
  SND_CHECK_EQ(onset[0], 11u);
}

SND_TEST(CoreFx, SidechainMonoSourceAndThresholdDuckingBothApply) {
  // Mono voice bus feeding a stereo target: the detector takes the peak over the source's
  // channels. The same row's threshold ducking (-6 dB, threshold -40 dB) applies too.
  Harness h;
  h.Load("b", SidechainBank(true, 6.0f, 1u).Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
  h.Steps(60);
  // Mono voice bus: sidechain peak 1.0 (0 dB): -(0 + 20)(0.75) = -15 dB, plus 6 dB of ducking.
  const double got = Amp(h, 60 * 256 - 8192, 8192, 1000.0);
  const double db = 20.0 * std::log10(got / (0.1 * kCenter));
  snd::test::Note("music %.2f dB (want -21)", db);
  SND_CHECK_NEAR(db, -21.0, 0.6);
}

// ===========================================================================
// C. Standard voice filters through core (LP / HP are on every real voice)
// ===========================================================================
SND_TEST(CoreFx, VoiceLowpassFromTheClipRandomFilterStaysInItsRange) {
  // randFilterHz 18000: cutoff uniformly in [2000, 20000]. 100 Hz always passes; 12 kHz
  // is between the 2 kHz and the 20 kHz cutoff responses, and the plays differ.
  std::vector<double> hi;
  for (int i = 0; i < 8; ++i) {
    snd_config cfg = SmallConfig();
    cfg.seed = 0x1234u + 7919u * static_cast<std::uint32_t>(i);  // playback seeds derive from it (2.5)
    Harness h(cfg);
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(96000, {100.0, 12000.0}, 0.3f), 0, 0});
    TEvent e = SimpleEvent("E", 0);
    e.tracks[0].clips[0].randFilterHz = 18000.0f;
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
    h.Clear();
    h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
    h.Steps(16);
    SND_CHECK_NEAR(Amp(h, 2048, 2048, 100.0), 0.3 * kCenter, 0.01);
    hi.push_back(Amp(h, 2048, 2048, 12000.0));
  }
  double lo = 1e9, hiMax = 0;
  for (double a : hi) {
    lo = std::min(lo, a);
    hiMax = std::max(hiMax, a);
  }
  snd::test::Note("12 kHz amplitude over the plays: %.4f .. %.4f", lo, hiMax);
  // Voice filter: 2 x 12 dB/oct Butterworth-like at the cutoff. 12 kHz with a 20 kHz cutoff
  // is nearly flat; with a 2 kHz cutoff it is ~2.6 octaves above: below -50 dB.
  SND_CHECK(hiMax <= 0.3 * kCenter * 1.02);  // never boosted
  SND_CHECK(lo >= 0.3 * kCenter * 0.001);    // never below the 2 kHz-cutoff response (-60 dB)
  SND_CHECK(hiMax > lo * 1.5);               // the random cutoff varies between plays
}

SND_TEST(CoreFx, VoiceLowpassAndHighpassFromParameterCurves) {
  // A Global parameter drives a lowpass curve (0 -> 20 kHz, 1 -> 300 Hz) and a highpass
  // curve (0 -> 10 Hz, 1 -> 3 kHz) through the standard voice filter.
  for (int target = 0; target < 2; ++target) {
    Harness h;
    TestBank b = BasicBank();
    b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(480000, {100.0, 8000.0}, 0.3f), 0, 0});
    b.params.push_back(TParam{HashId("P"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
    const bool lowpass = target == 0;
    b.curves.push_back(TCurve{HashId("P"),
                              static_cast<std::uint32_t>(lowpass ? bf::CurveTarget::Lowpass : bf::CurveTarget::Highpass),
                              0u,
                              lowpass ? std::vector<std::pair<float, float>>{{0.0f, 20000.0f}, {1.0f, 300.0f}}
                                      : std::vector<std::pair<float, float>>{{0.0f, 10.0f}, {1.0f, 3000.0f}}});
    TEvent e = SimpleEvent("E", 0);
    e.curves.push_back(0u);
    b.events.push_back(e);
    h.Load("b", b.Build(HashId("B")));
    h.Clear();
    h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
    h.Steps(16);
    const double lo0 = Amp(h, 2048, 2048, 100.0), hi0 = Amp(h, 2048, 2048, 8000.0);
    h.sys->SetParamGlobal(HashId("P"), 1.0f);
    h.Steps(32);
    const std::size_t at = 48 * 256 - 4096;
    const double lo1 = Amp(h, at, 4096, 100.0), hi1 = Amp(h, at, 4096, 8000.0);
    snd::test::Note("%s: 100 Hz %.4f -> %.4f, 8 kHz %.4f -> %.4f", lowpass ? "LP" : "HP", lo0, lo1, hi0, hi1);
    SND_CHECK_NEAR(lo0, 0.3 * kCenter, 0.01);
    SND_CHECK_NEAR(hi0, 0.3 * kCenter, 0.01);
    if (lowpass) {
      SND_CHECK_NEAR(lo1, 0.3 * kCenter, 0.02);
      SND_CHECK(hi1 < 0.3 * kCenter * 0.01);  // 24 dB/oct, ~4.7 octaves above 300 Hz
    } else {
      SND_CHECK_NEAR(hi1, 0.3 * kCenter, 0.02);
      SND_CHECK(lo1 < 0.3 * kCenter * 0.01);  // ~4.9 octaves below 3 kHz
    }
  }
}

SND_TEST(CoreFx, VoiceLowpassFromTheDistanceCurve) {
  Harness h;
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(480000, {100.0, 8000.0}, 0.3f), 0, 0});
  b.curves.push_back(TCurve{bf::kInputDistance, static_cast<std::uint32_t>(bf::CurveTarget::Volume), 0u,
                            {{0.0f, 0.0f}, {100.0f, 0.0f}}});  // flat volume (no 1/d)
  b.curves.push_back(TCurve{bf::kInputDistance, static_cast<std::uint32_t>(bf::CurveTarget::Lowpass), 0u,
                            {{1.0f, 20000.0f}, {50.0f, 400.0f}}});
  TEvent e = SimpleEvent("E", 0);
  e.flags = bf::kEvent3D;
  e.curveVolume = 0u;
  e.curveLowpass = 1u;
  b.events.push_back(e);
  h.Load("b", b.Build(HashId("B")));
  snd_listener_desc l{};
  l.forward = snd_vec3{0, 0, 1};
  l.up = snd_vec3{0, 1, 0};
  l.weight = 1.0f;
  l.enabled = 1;
  h.sys->SetListener(0, &l);
  const snd_emitter_handle em = h.sys->CreateEmitter();
  snd_vec3 near{0, 0, 1};
  h.sys->SetTransform(em, &near, nullptr, nullptr);
  h.Clear();
  h.sys->Play(HashId("E"), em, nullptr);
  h.Steps(16);
  // Straight ahead: both speakers get the centre gain.
  const double hiNear = Amp(h, 2048, 2048, 8000.0);
  snd_vec3 far{0, 0, 50};
  h.sys->SetTransform(em, &far, nullptr, nullptr);
  h.Steps(32);
  const std::size_t at = 48 * 256 - 4096;
  const double hiFar = Amp(h, at, 4096, 8000.0), loFar = Amp(h, at, 4096, 100.0);
  snd::test::Note("8 kHz near %.4f far %.5f, 100 Hz far %.4f", hiNear, hiFar, loFar);
  SND_CHECK_NEAR(hiNear, 0.3 * kCenter, 0.01);
  SND_CHECK(hiFar < 0.3 * kCenter * 0.01);
  SND_CHECK_NEAR(loFar, 0.3 * kCenter, 0.02);
}

// ===========================================================================
// C. D-132..D-134: heavy voice effects in the voice-effect pool
// ===========================================================================
namespace {
snd_config VfxConfig(std::uint32_t slots = 4) {
  snd_config c = SmallConfig();
  c.max_voice_effects = slots;
  return c;
}
// Assets: 0 = 100 Hz + 8 kHz (0.3 each, 10 s), 1 = an impulse then silence (100 ms),
// 2 = DC 0.1 (looping, endless). Events "Fx" (asset 0), "Dry" (asset 0), "Imp" (asset 1),
// "Dc" (asset 2), "Hi" (asset 2, priority 250).
TestBank VfxBank() {
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(480000, {100.0, 8000.0}, 0.3f), 0, 0});
  b.assets.push_back(TAsset{HashId("imp"), 1, 48000, Impulse(4800, 0, 1.0f), 0, 0});
  b.assets.push_back(TAsset{HashId("dc"), 1, 48000, Constant(4800, 0.1f), 0, 4800});
  b.events.push_back(SimpleEvent("Fx", 0));
  b.events.push_back(SimpleEvent("Dry", 0));
  b.events.push_back(SimpleEvent("Imp", 1));
  b.events.push_back(SimpleEvent("Dc", 2));
  TEvent hi = SimpleEvent("Hi", 2);
  hi.priority = 250;
  b.events.push_back(hi);
  return b;
}
const float kLp500[2] = {500.0f, 0.707f};
}  // namespace

SND_TEST(CoreFx, VoiceEffectRunsOnlyOnTheConfiguredEvent) {
  Harness h(VfxConfig());
  h.Load("b", VfxBank().Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_OK);
  h.Steps(1);
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 100.0), 0.3 * kCenter, 0.02);
  SND_CHECK(Amp(h, 2048, 2048, 8000.0) < 0.3 * kCenter * 0.02);  // 12 dB/oct, 4 octaves: < -40 dB
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 1u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
  h.sys->Stop(p, 0.0f, nullptr);
  h.Steps(4);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 0u);  // the slot goes back with the voice
  h.Clear();
  h.sys->Play(HashId("Dry"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 8000.0), 0.3 * kCenter, 0.01);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 1u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().dry, 0u);
}

SND_TEST(CoreFx, VoiceEchoPutsDecayingCopiesAndEndsWithTheVoice) {
  // Echo: delay 20 ms (960 frames), decay 0.5, dry 1, wet 1, max delay 50 ms. The source is
  // an impulse and 100 ms of silence: copies at 960 n with 0.5^(n-1), then the voice ends
  // at 4800 frames and the echo with it (D-134: no tail).
  Harness h(VfxConfig());
  h.Load("b", VfxBank().Build(HashId("B")));
  const float echo[5] = {20.0f, 0.5f, 1.0f, 1.0f, 50.0f};
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Imp"), 0, snd::dsp::kEffectEcho, echo, 5), SND_OK);
  h.Steps(1);
  h.Clear();
  h.sys->Play(HashId("Imp"), snd_emitter_handle{0}, nullptr);
  h.Steps(40);
  // The resampler shapes the impulse (a few frames); each copy is that shape scaled.
  SND_CHECK(h.At(0, 0) > 0.5f);
  for (int n = 1; n <= 4; ++n) {
    for (std::size_t j = 0; j < 8; ++j) {
      SND_CHECK_NEAR(h.At(std::size_t(960 * n) + j, 0), h.At(j, 0) * std::pow(0.5f, float(n - 1)), 1e-5f);
    }
  }
  // 960 * 5 = 4800: the voice has ended (source end), no copy at or after it.
  double after = MaxAbs(h, 4800 + 64, h.TotalFrames());
  snd::test::Note("after the end: max %g; at 4800..4808: %g", after, MaxAbs(h, 4800, 4808));
  SND_CHECK(after < 1e-6);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 0u);
}

SND_TEST(CoreFx, VoiceEffectPoolFullPlaysDryAndCounts) {
  Harness h(VfxConfig(2));
  Warnings w;
  CatchWarnings(h, &w);
  h.Load("b", VfxBank().Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_OK);
  h.Steps(1);
  h.Clear();
  for (int i = 0; i < 4; ++i) h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  const snd::core::System::VoiceFxStats st = h.sys->GetVoiceFxStats();
  SND_CHECK_EQ(st.acquired, 2u);
  SND_CHECK_EQ(st.dry, 2u);
  SND_CHECK_EQ(st.dryTooLarge, 0u);
  SND_CHECK_EQ(st.slotsInUse, 2u);
  SND_CHECK_EQ(w.Count(SND_ERR_LIMIT_REACHED, SND_END_DETAIL_VOICE_EFFECT_SLOTS), 2u);
  // Two voices filtered, two dry: the 8 kHz part is that of two voices, 100 Hz of four.
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 8000.0), 2.0 * 0.3 * kCenter, 0.03);
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 100.0), 4.0 * 0.3 * kCenter, 0.06);
}

SND_TEST(CoreFx, VoiceEffectTooLargeForTheSlotPlaysDry) {
  snd_config cfg = VfxConfig();
  cfg.voice_effect_bytes = 4096;  // a reverb needs ~132 KiB
  Harness h(cfg);
  h.Load("b", VfxBank().Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectReverb, nullptr, 0), SND_OK);
  h.Steps(1);
  h.Clear();
  h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().dry, 1u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().dryTooLarge, 1u);
  SND_CHECK_NEAR(Amp(h, 2048, 2048, 8000.0), 0.3 * kCenter, 0.01);  // dry
}

SND_TEST(CoreFx, VirtualisedVoiceReturnsItsSlotAndTakesOneBack) {
  snd_config cfg = VfxConfig();
  cfg.max_real_voices = 1;
  Harness h(cfg);
  h.Load("b", VfxBank().Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Dc"), 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_OK);
  h.Steps(1);
  h.sys->Play(HashId("Dc"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);
  const snd_playback_handle hi = h.sys->Play(HashId("Hi"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 0u);  // the DC voice is virtual: slot given back
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 1u);
  h.sys->Stop(hi, 0.0f, nullptr);
  h.Steps(8);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);  // real again: a fresh slot
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 2u);
  h.Clear();
  h.Steps(8);
  // The lowpass passes DC: the level is back to the voice's (no stuck state).
  SND_CHECK_NEAR(h.At(7 * 256, 0), 0.1f * kCenter, 1e-3f);
}

SND_TEST(CoreFx, PlaybackEffectParamAndConfigurationArguments) {
  Harness h(VfxConfig());
  h.Load("b", VfxBank().Build(HashId("B")));
  // Configuration arguments (D-132).
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(0u, 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 2, snd::dsp::kEffectLowpass, kLp500, 2),
               SND_ERR_INVALID_ARGUMENT);  // voice_effect_chain = 2
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, nullptr, 1),
               SND_ERR_INVALID_ARGUMENT);
  float many[17] = {};
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, many, 17), SND_ERR_INVALID_ARGUMENT);
  const float bad[1] = {NAN};
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, bad, 1), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, HashId("no.such.fx"), nullptr, 0), SND_ERR_NOT_FOUND);
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_OK);
  h.Steps(1);
  h.Clear();
  const snd_playback_handle p = h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  SND_CHECK(Amp(h, 2048, 2048, 8000.0) < 0.3 * kCenter * 0.02);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 2, 0, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 16, 1.0f), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 0, NAN), SND_ERR_INVALID_ARGUMENT);
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(snd_playback_handle{p.value + (1ull << 32)}, 0, 0, 1.0f),
               SND_ERR_INVALID_HANDLE);
  // Open the playback's filter: from the next block, ramped inside it.
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 0, 20000.0f), SND_OK);
  h.Steps(16);
  SND_CHECK_NEAR(Amp(h, 32 * 256 - 2048, 2048, 8000.0), 0.3 * kCenter, 0.02);
  // Removing the stage: voices that start later are dry and take no slot.
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, 0u, nullptr, 0), SND_OK);
  h.Steps(1);
  h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 1u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 1u);  // still the first playback's
}

SND_TEST(CoreFx, VoiceEffectConfigurationIsTakenAtTheVoiceStart) {
  Harness h(VfxConfig());
  h.Load("b", VfxBank().Build(HashId("B")));
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, kLp500, 2), SND_OK);
  h.Steps(1);
  h.Clear();
  h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  const float open[2] = {20000.0f, 0.707f};
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, snd::dsp::kEffectLowpass, open, 2), SND_OK);
  h.Steps(16);
  // The first voice keeps 500 Hz.
  SND_CHECK(Amp(h, 20 * 256 - 2048, 2048, 8000.0) < 0.3 * kCenter * 0.02);
  h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
  h.Steps(16);
  // + the second voice with the open filter: the 8 kHz part of one voice.
  SND_CHECK_NEAR(Amp(h, 36 * 256 - 2048, 2048, 8000.0), 0.3 * kCenter, 0.02);
}

SND_TEST(CoreFx, PluginAsVoiceEffectGetsTheVoiceChannelsAndPlayback) {
  GainPlugin plug(HashId("test.gain"));
  snd_config cfg = VfxConfig();
  cfg.plugins.effects = &plug.table;
  cfg.plugins.effect_count = 1;
  Harness h(cfg);
  h.Load("b", VfxBank().Build(HashId("B")));
  const float half[1] = {0.5f};
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Dc"), 0, HashId("test.gain"), half, 1), SND_OK);
  h.Steps(1);
  h.Clear();
  plug.log.clear();
  const snd_playback_handle p = h.sys->Play(HashId("Dc"), snd_emitter_handle{0}, nullptr);
  h.Steps(4);
  SND_CHECK_EQ(plug.inits, 1u);
  SND_REQUIRE(!plug.log.empty());
  SND_CHECK_EQ(plug.log.back().channels, 1u);       // the voice's channels (mono)
  SND_CHECK_EQ(plug.log.back().playback, p.value);  // snd_render_context.playback
  SND_CHECK_NEAR(h.At(3 * 256, 0), 0.1f * 0.5f * kCenter, 1e-5f);
  plug.log.clear();
  SND_CHECK_EQ(h.sys->SetPlaybackEffectParam(p, 0, 0, 0.25f), SND_OK);
  h.Steps(2);
  SND_REQUIRE(plug.log.size() == 2u);
  SND_CHECK_NEAR(plug.log[0].start[0], 0.5f, 0.0f);
  SND_CHECK_NEAR(plug.log[0].end[0], 0.25f, 0.0f);
  SND_CHECK_EQ(plug.log[0].mask, 1u);
  SND_CHECK_EQ(plug.log[1].mask, 0u);
}

SND_TEST(CoreFx, EveryStandardKindRunsAsAVoiceEffect) {
  using snd::dsp::EffectKind;
  for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(EffectKind::Count); ++k) {
    const snd::dsp::EffectInfo& info = snd::dsp::GetEffectInfo(static_cast<EffectKind>(k));
    Harness h(VfxConfig());
    h.Load("b", VfxBank().Build(HashId("B")));
    float params[16];
    snd::dsp::EffectDefaultParams(info.kind, params);
    // Delay lines sized for 1 s do not fit the default slot (160 KiB): 100 ms here.
    if (info.kind == EffectKind::Echo) params[4] = 100.0f, params[0] = 50.0f;
    if (info.kind == EffectKind::Delay) params[6] = 100.0f, params[0] = 50.0f;
    SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("Fx"), 0, info.typeId, params, info.paramCount), SND_OK);
    h.Steps(1);
    h.Clear();
    h.sys->Play(HashId("Fx"), snd_emitter_handle{0}, nullptr);
    h.Steps(20);
    const snd::core::System::VoiceFxStats st = h.sys->GetVoiceFxStats();
    SND_CHECK_MSG(st.acquired == 1u && st.dry == 0u, "%s: acquired %llu dry %llu", info.name,
                  static_cast<unsigned long long>(st.acquired), static_cast<unsigned long long>(st.dry));
    double e = 0;
    bool finite = true;
    for (std::size_t f = 0; f < h.TotalFrames(); ++f) {
      finite = finite && std::isfinite(h.At(f, 0));
      e += double(h.At(f, 0)) * h.At(f, 0);
    }
    SND_CHECK_MSG(finite && e > 1.0, "%s: output energy %g", info.name, e);
  }
}

// ===========================================================================
// D (core). LOD bypass of a bus chain only once its tail is done (2.4)
// ===========================================================================
SND_TEST(CoreFx, LodBypassWaitsForTheTailThenSkipsTheChain) {
  snd_config cfg = SmallConfig();
  cfg.tuning.lod_up_blocks = 0xFFFFFFFFu;  // stays at the forced level
  Harness h(cfg);
  TestBank b = BasicBank();
  // Echo 10 ms (480 frames), decay 0.5, dry 1, wet 1.
  b.effects.push_back(TEffect{snd::dsp::kEffectEcho, {10.0f, 0.5f, 1.0f, 1.0f, 20.0f}});
  b.buses[0].effects.assign(1, 0u);
  b.buses[0].flags = bf::kBusLodBypassable;
  b.assets.push_back(TAsset{HashId("imp"), 1, 48000, Impulse(256, 0, 1.0f), 0, 0});
  b.events.push_back(SimpleEvent("Imp", 0));
  h.Load("b", b.Build(HashId("B")));
  h.Clear();
  h.sys->Play(HashId("Imp"), snd_emitter_handle{0}, nullptr);
  h.Steps(1);  // level 0: the impulse enters the echo
  h.sys->SetLodLevelForTesting(3);
  h.Steps(7);
  // Level 3 while the tail sounds: the chain keeps running (copies at 480 n).
  SND_CHECK(h.At(0, 0) > 0.5f);
  for (int n = 1; n <= 3; ++n) {
    for (std::size_t j = 0; j < 4; ++j) {
      SND_CHECK_NEAR(h.At(std::size_t(480 * n) + j, 0), h.At(j, 0) * std::pow(0.5f, float(n - 1)), 1e-5f);
    }
  }
  // The tail (0.5^n down to -100 dB: ~17 copies, ~8200 frames) ends; then the chain is skipped.
  h.Steps(60);
  h.Clear();
  h.sys->Play(HashId("Imp"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK(h.At(0, 0) > 0.5f);
  SND_CHECK(MaxAbs(h, 400, h.TotalFrames()) < 1e-7);  // no copy: bypassed
  // Back to level 0: the chain runs again.
  h.sys->SetLodLevelForTesting(0);
  h.Clear();
  h.sys->Play(HashId("Imp"), snd_emitter_handle{0}, nullptr);
  h.Steps(8);
  SND_CHECK_NEAR(h.At(480, 0), h.At(0, 0), 1e-5f);
}

// ===========================================================================
// E. Determinism, allocations, RT safety, late chunks
// ===========================================================================
namespace {
// A script using every new path: bus effect fades, a sidechain compressor, voice effects
// (reverb + chorus, lowpass) with playback values, a pool smaller than the voices.
std::vector<float> FxScript(bool parallel) {
  snd_config cfg = VfxConfig(3);
  if (parallel) {
    cfg.jobs.realtime_workers = 2;
    cfg.tuning.rt_spin_microseconds = 200000;  // no late chunk (bit identity needs every chunk)
  }
  Harness h(cfg);
  TestBank b = SidechainBank(true, 3.0f, 2u);
  b.effects.push_back(TEffect{snd::dsp::kEffectReverb, {}});
  b.buses[0].effects.assign(1, 1u);  // the voice bus gets a reverb
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(96000, {150.0, 3000.0}, 0.2f), 0, 0});
  b.events.push_back(SimpleEvent("X", 2, 0));
  b.events.back().tracks[0].clips[0].randFilterHz = 15000.0f;
  b.events.back().tracks[0].clips[0].randPitchCents = 300.0f;
  h.Load("b", b.Build(HashId("B")));
  h.sys->SetEventVoiceEffect(HashId("X"), 0, snd::dsp::kEffectReverb, nullptr, 0);
  const float ch[6] = {0.5f, 0.5f, 10.0f, 1.5f, 0.4f, 0.2f};
  h.sys->SetEventVoiceEffect(HashId("X"), 1, snd::dsp::kEffectChorus, ch, 6);
  h.sys->SetEventVoiceEffect(HashId("M"), 0, snd::dsp::kEffectLowpass, kLp500, 2);
  h.Steps(1);
  h.Clear();
  h.sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
  std::vector<snd_playback_handle> ps;
  for (int blk = 0; blk < 120; ++blk) {
    if (blk % 9 == 0) ps.push_back(h.sys->Play(HashId("X"), snd_emitter_handle{0}, nullptr));
    if (blk == 30) h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
    if (blk % 17 == 5) h.sys->SetEffectParam(HashId("Voice"), 0, 4, 0.3f + 0.05f * float(blk % 5), 0.02f);
    if (blk % 13 == 3 && !ps.empty()) h.sys->SetPlaybackEffectParam(ps.back(), 1, 3, 0.5f + 0.1f * float(blk % 7));
    if (blk % 23 == 7 && ps.size() > 2) h.sys->Stop(ps[ps.size() - 3], 0.01f, nullptr);
    h.Step();
  }
  return h.all;
}
}  // namespace

SND_TEST(CoreFxDeterminism, SameScriptSameBitsSerialAndParallel) {
  const std::vector<float> a = FxScript(false);
  const std::vector<float> b = FxScript(false);
  SND_REQUIRE_EQ(a.size(), b.size());
  SND_CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
  const std::vector<float> c = FxScript(true);
  SND_REQUIRE_EQ(a.size(), c.size());
  SND_CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
  double energy = 0;
  for (float v : a) energy += double(v) * v;
  SND_CHECK(energy > 1.0);
}

SND_TEST(CoreFxMemory, NoAllocationAfterInitialisationOnTheNewPaths) {
  CountingAllocator alloc;
  {
    Harness h(VfxConfig(2), {}, nullptr, 0, &alloc);
    TestBank b = SidechainBank(true, 3.0f, 2u);
    b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(96000, {150.0, 3000.0}, 0.2f), 0, 0});
    b.events.push_back(SimpleEvent("X", 2, 0));
    h.Load("b", b.Build(HashId("B")));
    h.Steps(2);
    const std::uint64_t before = alloc.allocations.load();
    for (int i = 0; i < 100; ++i) {
      if (i % 10 == 0) h.sys->SetEventVoiceEffect(HashId("X"), static_cast<std::uint32_t>(i / 10 % 2), snd::dsp::kEffectReverb, nullptr, 0);
      const snd_playback_handle p = h.sys->Play(HashId("X"), snd_emitter_handle{0}, nullptr);
      if (i % 3 == 0) h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
      h.sys->SetPlaybackEffectParam(p, 0, 4, 0.5f + 0.01f * float(i));
      h.sys->SetEffectParam(HashId("Music"), 0, 1, 2.0f + 0.1f * float(i % 10), 0.01f);
      h.Steps(1);
    }
    SND_CHECK_EQ(alloc.allocations.load(), before);
    SND_CHECK(h.sys->GetVoiceFxStats().acquired > 0u);
    SND_CHECK(h.sys->GetVoiceFxStats().dry > 0u);
  }
  SND_CHECK_EQ(alloc.allocations.load(), alloc.frees.load());
}

SND_TEST(CoreFxRt, NewPathsHaveNoRtViolations) {
  snd_config cfg = VfxConfig(2);
  cfg.jobs.realtime_workers = 2;
  Harness h(cfg);
  TestBank b = SidechainBank(true, 3.0f, 2u);
  b.assets.push_back(TAsset{HashId("mix"), 1, 48000, Mix(96000, {150.0, 3000.0}, 0.2f), 0, 0});
  b.events.push_back(SimpleEvent("X", 2, 0));
  h.Load("b", b.Build(HashId("B")));
  h.sys->SetEventVoiceEffect(HashId("X"), 0, snd::dsp::kEffectReverb, nullptr, 0);
  h.sys->SetEventVoiceEffect(HashId("X"), 1, snd::dsp::kEffectFlanger, nullptr, 0);
  h.sys->Play(HashId("M"), snd_emitter_handle{0}, nullptr);
  h.sys->Play(HashId("V"), snd_emitter_handle{0}, nullptr);
  snd::test::rt::Reset();
  for (int i = 0; i < 40; ++i) {
    const snd_playback_handle p = h.sys->Play(HashId("X"), snd_emitter_handle{0}, nullptr);
    h.sys->SetPlaybackEffectParam(p, 1, 0, 0.3f);
    h.sys->SetEffectParam(HashId("Music"), 0, 0, -30.0f + float(i % 5), 0.005f);
    h.sys->Update();
    {
      SND_RT_SCOPE();
      h.sys->RenderBlock(h.out.data());
    }
  }
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}

namespace {
int WaitCalmFx(Harness& h) {
  std::uint32_t calm = 0;
  bool released = false;
  for (int i = 0; i < 2000 && calm < 3u; ++i) {
    h.Step();
    const snd::core::BlockProfile& p = h.sys->LastBlock();
    released = released || p.heldVoices == 0u;
    calm = (p.heldVoices == 0u && p.lateChunks == 0u) ? calm + 1u : 0u;
    if (calm == 0u) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return calm >= 3u ? 2 : (released ? 1 : 0);
}
}  // namespace

SND_TEST(CoreFxParallel, LateChunkKeepsTheVoiceEffectState) {
  // D-43 with voice effects: while a late chunk holds its voices, their slots stay bound
  // (not freed, not reinitialised); afterwards the voices play on through their effects.
  snd_config cfg = VfxConfig(4);
  cfg.jobs.realtime_workers = 2;
  cfg.jobs.voices_per_chunk = 1;
  cfg.tuning.rt_spin_microseconds = 20000;
  Harness h(cfg);
  TestBank b = BasicBank();
  b.assets.push_back(TAsset{HashId("a"), 1, 48000, Constant(4800000, 0.1f), 0, 0});
  b.events.push_back(SimpleEvent("E", 0));
  h.Load("b", b.Build(HashId("B")));
  const float lp[2] = {2000.0f, 0.707f};
  const float rv[1] = {0.0f};  // reverb dry level 0 mB: DC passes, the tail adds on top
  (void)rv;
  SND_CHECK_EQ(h.sys->SetEventVoiceEffect(HashId("E"), 0, snd::dsp::kEffectLowpass, lp, 2), SND_OK);
  h.Steps(1);
  for (int i = 0; i < 4; ++i) h.sys->Play(HashId("E"), snd_emitter_handle{0}, nullptr);
  const float steady = 4 * 0.1f * kCenter;
  WaitCalmFx(h);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 4u);
  for (int cycle = 0; cycle < 3; ++cycle) {
    h.sample.CloseGate();
    bool late = false;
    for (int i = 0; i < 20000 && !late; ++i) {
      h.Step();
      late = h.sys->LastBlock().lateChunks != 0u;
      for (float v : h.out) SND_CHECK(std::isfinite(v) && std::fabs(v) <= steady + 1e-3f);
      if (!late) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SND_CHECK(late);
    for (int k = 0; k < 3; ++k) {
      h.Step();
      SND_CHECK(h.sys->LastBlock().heldVoices != 0u);
      SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 4u);  // held voices keep their slots
      for (float v : h.out) SND_CHECK(std::isfinite(v) && std::fabs(v) <= steady + 1e-3f);
    }
    h.sample.OpenGate();
    WaitCalmFx(h);
  }
  const int calm = WaitCalmFx(h);
  SND_CHECK_EQ(h.sample.gateTimeouts.load(), 0u);
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().acquired, 4u);  // never re-taken: same assignment
  SND_CHECK_EQ(h.sys->GetVoiceFxStats().slotsInUse, 4u);
  if (calm == 2) SND_CHECK_NEAR(h.out[100 * 2], steady, 1e-4f);
}
