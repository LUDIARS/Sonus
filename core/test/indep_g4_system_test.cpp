// Independent tests, stage 3 group 4: determinism (2.5: same script -> same bits, serial
// and parallel, load LOD disabled), no allocation after initialisation, RT safety and the
// D-43 checks of IndepKind over a script that drives every new path. See indep_g4_fixture.hpp.
// Repeat under TSan / Release: ctest --repeat until-fail:20 -R indep_g4_system_test
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "indep_g4_fixture.hpp"
#include "snd/test/rt_guard.hpp"

using namespace g4;

namespace {

TAction Act(float ms, std::uint32_t kind, Id target, float value = 0) {
  TAction a;
  a.timeMs = ms;
  a.action = kind;
  a.target = target;
  a.value = value;
  return a;
}

// Buses: 0 "Sfx" -> 3, 1 "Kid" (meter) -> 3, 2 "Met" (meter) -> 3, 3 "Master".
std::vector<std::uint8_t> ScriptBank() {
  B bk;
  bk.b.buses.clear();
  bk.b.buses.push_back(TBus{HashId("Sfx"), 3u, 0, 0, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Kid"), 3u, 0, bf::kBusMeter, 0.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Met"), 3u, 0, bf::kBusMeter, -3.0f, {}, {}});
  bk.b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  for (std::uint32_t m = 0; m < 4; ++m) bk.b.groups.push_back(TGroup{HashId("G") + m, 2, m});
  TParam lp;
  lp.id = HashId("L");
  bk.b.params.push_back(lp);
  TParam vp;
  vp.id = HashId("V");
  bk.b.params.push_back(vp);
  TCurve cv;
  cv.input = HashId("V");
  cv.target = 0;
  cv.compose = 0;
  cv.points = {{0.0f, 0.0f}, {1.0f, -20.0f}};
  bk.b.curves.push_back(cv);
  // Row 0: nested parent -> row 1 (child on Kid).
  TClip nest;
  nest.kind = 1;
  nest.target = 1;
  nest.volumeDb = -3.0f;
  nest.pitchCents = 300.0f;
  bk.Event("Nest", {nest});
  bk.Event("Child", {B::AssetClip(bk.Asset("child", Sine(9000, 330.0, 48000.0, 0.3f)))}, 0, 1);
  // Random (weighted), shuffle, combo with random ranges.
  std::vector<TClip> sel;
  for (int i = 0; i < 6; ++i) {
    char n[16];
    std::snprintf(n, sizeof n, "sel%d", i);
    TClip c = B::AssetClip(bk.Asset(n, Sine(3000 + 500 * i, 200.0 + 50.0 * i, 48000.0, 0.2f)));
    c.weight = float(i + 1);
    c.randVolumeDb = 3.0f;
    c.randPitchCents = 200.0f;
    c.randPan = 0.5f;
    c.randStartMs = 5.0f;
    sel.push_back(c);
  }
  bk.Event("Rnd", sel, 1, 2);
  bk.Event("Shf", sel, 4, 0);
  bk.Event("Cmb", sel, 6, 0).comboWindowMs = 40.0f;
  // Layers.
  {
    TClip a = B::SynthClip(bk.Synth(300.0f));
    a.layerMin = 0.0f;
    a.layerMax = 0.6f;
    TClip b = B::SynthClip(bk.Synth(1000.0f));  // counter
    b.layerMin = 0.4f;
    b.layerMax = 1.0f;
    TEvent& e = bk.Event("Lay", {a, b}, 7, 2);
    e.tracks[0].layerParam = HashId("L");
  }
  // Actions: play X at 10 ms, set V at 20 ms, stop X at 40 ms.
  {
    TEvent& e = bk.Event("Act", {B::SynthClip(bk.Synth(200.0f))});
    e.tracks[0].actions = {Act(10.0f, 0, HashId("X")), Act(20.0f, 2, HashId("V"), 1.0f), Act(40.0f, 1, HashId("X"))};
    e.curves = {0};
  }
  bk.Event("X", {B::AssetClip(bk.Asset("x", Sine(48000, 550.0, 48000.0, 0.2f)))});
  // Groups with each steal mode.
  for (std::uint32_t m = 0; m < 4; ++m) {
    char n[16];
    std::snprintf(n, sizeof n, "Grp%u", m);
    TEvent& e = bk.Event(n, {B::SynthClip(bk.Synth(50.0f + 30.0f * float(m)), -2.0f * float(m))});
    e.group = m;
    e.priority = 100u + 10u * m;
  }
  // N loops + clip fade-out; virtual return modes.
  {
    TClip c = B::AssetClip(bk.Asset("loop", Sine(1500, 700.0, 48000.0, 0.25f)));
    c.loopCount = 3;
    c.fadeOutMs = 10.0f;
    bk.Event("Loop", {c});
  }
  for (std::uint32_t m = 0; m < 3; ++m) {
    char n[16];
    std::snprintf(n, sizeof n, "Vr%u", m);
    TEvent& e = bk.Event(n, {B::AssetClip(bk.Asset(n, Sine(20000, 410.0 + 20.0 * m, 48000.0, 0.15f)))});
    e.virtualReturn = m;
    e.priority = 60;
  }
  bk.Event("Fade", {B::SynthClip(bk.Synth(1000.0f))});
  return bk.Build("Script");
}

struct Run {
  std::vector<float> out;
  std::uint64_t violations = 0;
  std::uint64_t ends = 0;
  std::uint64_t markers = 0;
  std::uint64_t spectra = 0;
  std::uint64_t lateChunks = 0;
};

// The script. `allocDelta` (if not null) gets the allocations after initialisation; `rt`
// wraps every render in an RT scope.
Run RunScript(std::uint32_t workers, CountingAllocator* alloc = nullptr, std::uint64_t* allocDelta = nullptr,
              bool rt = false, std::uint32_t caseMask = 0xFFFFu, bool lateChunks = false) {
  MarkerTable mt;
  mt.asset = HashId("child");
  mt.frames = {100, 3000, 8000};
  mt.ids = {HashId("k0"), HashId("k1"), HashId("k2")};
  snd_config cfg = Config(8, workers);
  // No late chunk (2.5: bit identity holds without deadline misses): wait for every chunk.
  cfg.tuning.rt_spin_microseconds = lateChunks ? 0u : 5000000u;
  cfg.max_virtual_voices = 128;
  cfg.max_playbacks = 256;
  snd::core::CoreLimits lim;
  Run run;
  {
    Rig r(cfg, lim, alloc, &mt);
    r.Load("s", ScriptBank());
    r.Steps(2);
    std::vector<float> spec(513);
    snd_meter m{};
    r.sys->GetSpectrum(SND_ID_NONE, 0, spec.data(), 513);
    r.sys->GetSpectrum(HashId("Kid"), 0, spec.data(), 513);
    r.Steps(1);
    const std::uint64_t before = alloc != nullptr ? alloc->allocations.load() : 0;
    r.all.clear();
    if (rt) snd::test::rt::Reset();
    std::vector<snd_playback_handle> live;
    const snd_emitter_handle em = r.sys->CreateEmitter();
    snd_playback_handle actH{0};
    for (int b = 0; b < 160; ++b) {
      switch ((caseMask >> (b % 16)) & 1u ? b % 16 : 99) {
        case 0: live.push_back(r.sys->Play(HashId("Nest"), em, nullptr)); break;
        case 1: r.sys->Play(HashId("Rnd"), NoEm(), nullptr); r.sys->Play(HashId("Shf"), NoEm(), nullptr); break;
        case 2: r.sys->Play(HashId("Cmb"), NoEm(), nullptr); break;
        case 3: {
          const snd_playback_handle h = r.sys->Play(HashId("Lay"), NoEm(), nullptr);
          r.sys->SetParamPlayback(h, HashId("L"), float(b % 7) / 6.0f);
          live.push_back(h);
          break;
        }
        case 4: actH = r.sys->Play(HashId("Act"), NoEm(), nullptr); break;
        case 5:
          for (int k = 0; k < 6; ++k) {
            char n[16];
            std::snprintf(n, sizeof n, "Grp%d", (b / 16 + k) % 4);
            r.sys->Play(HashId(n), NoEm(), nullptr);
          }
          break;
        case 6: r.sys->Play(HashId("Loop"), NoEm(), nullptr); break;
        case 7:
          for (int k = 0; k < 12; ++k) {
            char n[16];
            std::snprintf(n, sizeof n, "Vr%d", k % 3);
            live.push_back(r.sys->Play(HashId(n), NoEm(), nullptr));
          }
          break;
        case 8: {
          const snd_when w = AtClock(r.Now() + 300);
          live.push_back(r.sys->PlayFade(HashId("Fade"), NoEm(), &w, 0.012f));
          break;
        }
        case 9: r.sys->SetParamGlobal(HashId("L"), float(b % 5) / 4.0f); break;
        case 10:
          if (!live.empty()) r.sys->Pause(live.front(), 0.0f);
          break;
        case 11:
          if (!live.empty()) r.sys->ResumePlayback(live.front(), 0.0f);
          break;
        case 12:
          if (live.size() > 4) {
            r.sys->Stop(live[live.size() - 3], 0.01f, nullptr);
            r.sys->Stop(live[1], 0.0f, nullptr);
          }
          break;
        case 13: {
          const snd_vec3 p{float(b % 9) - 4.0f, 0.0f, 2.0f};
          r.sys->SetTransform(em, &p, nullptr, nullptr);
          break;
        }
        case 14:  // re-check 1: pause an action parent mid-track, a fade-in on a looping clip
          r.sys->Pause(actH, 0.0f);
          live.push_back(r.sys->PlayFade(HashId("Loop"), NoEm(), nullptr, 0.02f));
          live.push_back(r.sys->PlayFade(HashId("Nest"), em, nullptr, 0.005f));
          break;
        case 15: r.sys->ResumePlayback(actH, 0.0f); break;
        default: break;
      }
      r.sys->Update();
      if (r.sys->GetSpectrum(b % 2 == 0 ? SND_ID_NONE : HashId("Kid"), std::uint32_t(b % 2), spec.data(), 513) ==
          SND_OK) {
        ++run.spectra;
      }
      r.sys->GetMeter(HashId("Met"), &m);
      if (rt) {
        indep::tAudioThread = true;
        {
          SND_RT_SCOPE();
          r.sys->RenderBlock(r.out.data());
        }
        r.all.insert(r.all.end(), r.out.begin(), r.out.end());
        ++r.block;
      } else {
        r.Render();
      }
      if (live.size() > 40) live.erase(live.begin(), live.begin() + 20);
    }
    for (const snd_playback_handle& h : live) r.sys->Stop(h, 0.0f, nullptr);
    r.Steps(20);
    if (allocDelta != nullptr) *allocDelta = alloc->allocations.load() - before;
    run.out = r.all;
    run.violations = r.osc.ViolationSum();
    run.ends = r.ends.size();
    run.markers = r.markers.size();
    snd_stats st{};
    r.sys->Update();
    if (r.sys->GetStats(&st) == SND_OK) run.lateChunks = st.late_chunks;
  }
  return run;
}

}  // namespace

SND_TEST(IndepG4System, SameScriptSameBitsSerialAndParallel) {
  const Run a = RunScript(0), b = RunScript(0), c = RunScript(2);
  SND_REQUIRE_EQ(a.out.size(), b.out.size());
  SND_REQUIRE_EQ(a.out.size(), c.out.size());
  double energy = 0;
  for (float v : a.out) energy += double(v) * v;
  std::size_t diff = 0;
  for (std::size_t i = 0; i < a.out.size(); ++i) diff += std::memcmp(&a.out[i], &c.out[i], sizeof(float)) != 0 ? 1u : 0u;
  snd::test::Note("energy %g, ends %llu, markers %llu, spectra read %llu; serial vs parallel: %llu samples differ",
                  energy, static_cast<unsigned long long>(a.ends), static_cast<unsigned long long>(a.markers),
                  static_cast<unsigned long long>(a.spectra), static_cast<unsigned long long>(diff));
  SND_CHECK(energy > 1.0);
  SND_CHECK(a.markers > 0u);
  SND_CHECK(a.spectra > 100u);
  SND_CHECK(std::memcmp(a.out.data(), b.out.data(), a.out.size() * sizeof(float)) == 0);
  SND_CHECK_EQ(diff, 0u);
  SND_CHECK_EQ(a.ends, b.ends);
  SND_CHECK_EQ(a.ends, c.ends);
  SND_CHECK_EQ(a.markers, c.markers);
  SND_CHECK_EQ(a.violations + c.violations, 0u);
}

// Late chunks (no spin at the deadline): the new per-voice states keep the D-43 rules
// (IndepKind's checks), every path still ends cleanly (sanitizers).
SND_TEST(IndepG4System, LateChunksKeepTheD43Rules) {
  for (int i = 0; i < 3; ++i) {
    const Run r = RunScript(2, nullptr, nullptr, false, 0xFFFFu, true);
    snd::test::Note("late chunks %llu", static_cast<unsigned long long>(r.lateChunks));
    SND_CHECK_EQ(r.violations, 0u);
    SND_CHECK(r.ends > 0u);
  }
}

SND_TEST(IndepG4System, NoAllocationAfterInitialisation) {
  CountingAllocator alloc;
  std::uint64_t delta = 1;
  RunScript(2, &alloc, &delta);
  snd::test::Note("allocations after initialisation: %llu", static_cast<unsigned long long>(delta));
  SND_CHECK_EQ(delta, 0u);
  SND_CHECK_EQ(alloc.allocations.load(), alloc.frees.load());
}

SND_TEST(IndepG4System, RenderPathHasNoRtViolations) {
  const Run r = RunScript(2, nullptr, nullptr, true);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  SND_CHECK_EQ(r.violations, 0u);
}
