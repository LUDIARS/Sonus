// core, stage 3 group 4: the new paths (shuffle, weights, actions, nested events, layer
// crossfade, counted loops, fade-in, quietest steals, virtual returns, analysis) under the
// system-wide guarantees: the same script gives the same bits (serial and parallel, 2.5),
// no allocation after initialisation (3.3), no RT violation (1.2).
#include <cmath>
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "snd/internal/rt_check.hpp"
#include "snd/test/rt_guard.hpp"

using namespace coretest;
using snd::HashId;

namespace {
TestBank ScriptBank() {
  TestBank b;
  b.buses.push_back(TBus{HashId("Meter"), 1u, 0, bf::kBusMeter, 0.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  for (int i = 0; i < 8; ++i) {
    b.assets.push_back(TAsset{HashId(("s" + std::to_string(i)).c_str()), 1, 44100,
                              Sine(20000, 200.0 + 50.0 * i, 44100.0, 0.1f), 0, 0});
  }
  b.groups.push_back(TGroup{HashId("Quiet"), 3, 1});
  b.params.push_back(TParam{HashId("X"), 0, 0.0f, 1.0f, 0.0f, 1.5f});
  b.params.push_back(TParam{HashId("V"), 0, 0.0f, 1.0f, 0.0f, 0.0f});
  b.curves.push_back(TCurve{HashId("V"), static_cast<std::uint32_t>(bf::CurveTarget::Volume), 0, {{0.0f, 0.0f}, {1.0f, -12.0f}}});
  // 0 Shuffle (8 clips, random ranges)
  TEvent sh;
  sh.id = HashId("Shuffle");
  sh.sequence = static_cast<std::uint32_t>(bf::SequenceKind::Shuffle);
  sh.group = 0;
  TTrack t;
  for (std::uint32_t i = 0; i < 8; ++i) {
    TClip c;
    c.target = i;
    c.randVolumeDb = 4.0f;
    c.randPitchCents = 80.0f;
    c.randPan = 0.6f;
    c.randStartMs = 5.0f;
    c.randFilterHz = 9000.0f;
    t.clips.push_back(c);
  }
  sh.tracks.push_back(t);
  // 1 Weighted random, counted loop and fade-out
  TEvent wr;
  wr.id = HashId("Weighted");
  wr.sequence = static_cast<std::uint32_t>(bf::SequenceKind::Random);
  TTrack t2;
  for (std::uint32_t i = 0; i < 3; ++i) {
    TClip c;
    c.target = i;
    c.weight = float(i);
    c.loopCount = 2 + i;
    c.fadeOutMs = 30.0f;
    t2.clips.push_back(c);
  }
  wr.tracks.push_back(t2);
  // 2 Layer crossfade
  TEvent ly;
  ly.id = HashId("Layer");
  ly.sequence = static_cast<std::uint32_t>(bf::SequenceKind::LayerCrossfade);
  ly.bus = 0;
  TTrack t3;
  t3.layerParam = HashId("X");
  for (std::uint32_t i = 0; i < 3; ++i) {
    TClip c;
    c.target = 3 + i;
    c.layerMin = 0.3f * float(i);
    c.layerMax = 0.3f * float(i) + 0.45f;
    c.loopCount = 0;
    t3.clips.push_back(c);
  }
  ly.tracks.push_back(t3);
  // 3 Parent: nested event 4, actions (play Shuffle at 0 and 7 ms, set V, stop Layer later)
  TEvent pa;
  pa.id = HashId("Parent");
  pa.curves = {0};
  TTrack t4;
  TClip nested;
  nested.kind = 1;
  nested.target = 4;
  nested.volumeDb = -3.0f;
  t4.clips.push_back(nested);
  t4.actions.push_back(TAction{0.0f, 0u, HashId("Shuffle"), 0.0f});
  t4.actions.push_back(TAction{7.0f, 0u, HashId("Shuffle"), 0.0f});
  t4.actions.push_back(TAction{3.0f, 2u, HashId("V"), 0.7f});
  t4.actions.push_back(TAction{60.0f, 1u, HashId("Layer"), 0.0f});
  pa.tracks.push_back(t4);
  // 4 Child (nested)
  TEvent ch;
  ch.id = HashId("Child");
  ch.bus = 0;
  ch.virtualReturn = 1;
  TTrack t5;
  TClip c5;
  c5.target = 6;
  c5.pitchCents = 30.0f;
  t5.clips.push_back(c5);
  ch.tracks.push_back(t5);
  b.events = {sh, wr, ly, pa, ch};
  return b;
}

struct ScriptResult {
  std::vector<float> out;
  std::vector<float> spectrum;
  snd_meter meter{};
};

// The script; `probe` runs once the bank is loaded (before the scripted blocks).
template <class Probe>
ScriptResult RunScript(bool parallel, snd::IAllocator* alloc, bool rtScope, Probe&& probe) {
  snd_config cfg = SmallConfig();
  cfg.max_real_voices = 6;
  cfg.jobs.realtime_workers = parallel ? 2u : 0u;
  cfg.jobs.voices_per_chunk = 2;
  cfg.tuning.rt_spin_microseconds = 200000;  // no late chunk (D-43 holds change the output)
  // Bit identity is promised with the load LOD off (2.5): a slow build (TSan) would
  // otherwise lower the real-voice limit / resampler mid-script, at a time-dependent block.
  cfg.tuning.lod_down_blocks = 0;
  Harness h(cfg, {}, nullptr, 0, alloc);
  h.Load("b", ScriptBank().Build(HashId("B")));
  h.Steps(2);
  probe();
  h.Clear();
  ScriptResult r;
  std::vector<float> mag(513);
  for (int i = 0; i < 160; ++i) {
    switch (i % 16) {
      case 0: h.sys->Play(HashId("Parent"), snd_emitter_handle{0}, nullptr); break;
      case 3: h.sys->Play(HashId("Weighted"), snd_emitter_handle{0}, nullptr); break;
      case 5: h.sys->PlayFade(HashId("Layer"), snd_emitter_handle{0}, nullptr, 0.03f); break;
      case 7: h.sys->SetParamGlobal(HashId("X"), (i / 16) % 2 == 0 ? 1.0f : 0.0f); break;
      case 9: h.sys->PlayOneShot(HashId("Shuffle"), snd_emitter_handle{0}, nullptr); break;
      default: break;
    }
    h.sys->Update();
    if (rtScope) {
      SND_RT_SCOPE();
      h.sys->RenderBlock(h.out.data());
    } else {
      h.sys->RenderBlock(h.out.data());
    }
    h.all.insert(h.all.end(), h.out.begin(), h.out.end());
    if (i % 8 == 0 && h.sys->GetSpectrum(HashId("Meter"), 0u, mag.data(), 513u) == SND_OK) {
      r.spectrum.insert(r.spectrum.end(), mag.begin(), mag.end());
    }
  }
  h.sys->Update();
  h.sys->GetMeter(HashId("Meter"), &r.meter);
  snd_stats st{};
  h.sys->GetStats(&st);
  SND_CHECK_EQ(st.lod_level, 0u);
  SND_CHECK_EQ(st.late_chunks, 0u);
  r.out = h.all;
  return r;
}
}  // namespace

SND_TEST(CoreG4System, SameScriptSameBits) {
  const ScriptResult a = RunScript(false, nullptr, false, [] {});
  const ScriptResult b = RunScript(false, nullptr, false, [] {});
  SND_REQUIRE_EQ(a.out.size(), b.out.size());
  SND_CHECK(std::memcmp(a.out.data(), b.out.data(), a.out.size() * sizeof(float)) == 0);
  SND_REQUIRE_EQ(a.spectrum.size(), b.spectrum.size());
  SND_CHECK(!a.spectrum.empty());
  SND_CHECK(std::memcmp(a.spectrum.data(), b.spectrum.data(), a.spectrum.size() * sizeof(float)) == 0);
  SND_CHECK(std::memcmp(&a.meter, &b.meter, sizeof(snd_meter)) == 0);
  const ScriptResult c = RunScript(true, nullptr, false, [] {});
  SND_REQUIRE_EQ(a.out.size(), c.out.size());
  double maxDiff = 0, energy = 0;
  for (std::size_t i = 0; i < a.out.size(); ++i) {
    maxDiff = std::max(maxDiff, double(std::fabs(a.out[i] - c.out[i])));
    energy += double(a.out[i]) * a.out[i];
  }
  snd::test::Note("serial vs parallel max |diff| = %g, energy %g", maxDiff, energy);
  SND_CHECK(maxDiff <= 1e-6);
  SND_CHECK(energy > 1.0);
}

SND_TEST(CoreG4System, NoAllocationAfterInitialisation) {
  CountingAllocator alloc;
  std::uint64_t before = 0;
  RunScript(false, &alloc, false, [&] { before = alloc.allocations.load(); });
  // (The harness's own vectors use the global heap, not this allocator.)
  SND_CHECK(before > 0u);
  SND_CHECK_EQ(alloc.allocations.load(), before);
  SND_CHECK_EQ(alloc.allocations.load(), alloc.frees.load());
}

SND_TEST(CoreG4System, RenderPathHasNoRtViolations) {
  snd::test::rt::Reset();
  RunScript(true, nullptr, true, [] { snd::test::rt::Reset(); });
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
}
