// Stage 3 group 2 (core): load with heavy voice effects (D-132). The standard path
// (every real voice with the standard LP / HP) is judged by CoreLoad.Real128VoicesWithinBudget;
// this file measures and reports 128 real voices with the voice-effect pool full
// (reverb + lowpass per voice) for several pool sizes. Same load as core_load_test.cpp.
// No budget is enforced here (the numbers go to the stage-3 report).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/platform/clock.hpp"

using namespace coretest;
using snd::HashId;

namespace {
// CPU time of the rendering (the audio thread and the RT workers): process CPU
// time, which this test spends only in RenderBlock (no other thread is busy).
// Wall time is not used: a loaded machine (other builds) stretches it.
// Returns false when the clock is not available.
[[maybe_unused]] inline bool ProcessCpuNs(std::uint64_t* ns) {
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return false;
  *ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
  return true;
}
struct Timing {
  double avgUs, p99Us, maxUs;
  double cpuAvgUs;  // < 0: no CPU clock
};
Timing RunLoad(std::uint32_t workers, std::uint32_t requested, std::uint32_t* realOut, std::uint32_t vfxSlots,
               snd::core::System::VoiceFxStats* vfxOut) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_real_voices = 128;
  cfg.max_virtual_voices = 1024;
  cfg.max_playbacks = 1024;
  cfg.jobs.realtime_workers = workers;
  cfg.max_voice_channels = 2;
  cfg.tuning.lod_down_blocks = 0;  // measure without the load LOD (2.4)
  cfg.max_voice_effects = vfxSlots;      // D-132: heavy voice effects on this many voices
  cfg.voice_effect_bytes = 192u << 10;   // reverb + lowpass at 48 kHz
  Harness h(cfg);
  TestBank b;
  b.effects.push_back(TEffect{snd::dsp::kEffectLowpass, {6000.0f, 0.707f}});
  b.effects.push_back(TEffect{snd::dsp::kEffectCompressor, {}});
  b.buses.push_back(TBus{HashId("A"), 2u, 2, 0, -3.0f, {0}, {}});
  b.buses.push_back(TBus{HashId("B"), 2u, 2, 0, -3.0f, {1}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  std::vector<float> st(2 * 88200);
  for (std::size_t i = 0; i < 88200; ++i) {
    st[2 * i] = 0.05f * static_cast<float>(std::sin(0.031 * double(i)));
    st[2 * i + 1] = 0.05f * static_cast<float>(std::sin(0.047 * double(i)));
  }
  b.assets.push_back(TAsset{HashId("st"), 2, 44100, st, 0, 88200});
  b.curves.push_back(TCurve{HashId("Cut"), static_cast<std::uint32_t>(bf::CurveTarget::Lowpass),
                            static_cast<std::uint32_t>(bf::CurveCompose::Min), {{0.0f, 2000.0f}, {1.0f, 8000.0f}}});
  for (int k = 0; k < 4; ++k) {
    TEvent e = SimpleEvent(("L" + std::to_string(k)).c_str(), 0, static_cast<std::uint32_t>(k & 1));
    e.tracks[0].clips[0].pitchCents = 50.0f * k;
    e.tracks[0].clips[0].loopCount = 0;
    if ((k & 1) != 0) e.curves.assign(1, 0u);
    if (k == 3) e.flags = bf::kEvent3D;
    b.events.push_back(e);
  }
  h.Load("b", b.Build(HashId("Load")));
  snd_listener_desc l{};
  l.forward = snd_vec3{0, 0, 1};
  l.up = snd_vec3{0, 1, 0};
  l.enabled = 1;
  h.sys->SetListener(0, &l);
  const snd_emitter_handle em = h.sys->CreateEmitter();
  const snd_vec3 pos{3, 0, 2};
  h.sys->SetTransform(em, &pos, nullptr, nullptr);
  h.sys->SetParamGlobal(HashId("Cut"), 0.5f);
  if (vfxSlots != 0u) {
    const float lp[2] = {3000.0f, 0.707f};
    for (int k = 0; k < 4; ++k) {
      const snd::Id ev = HashId(("L" + std::to_string(k)).c_str());
      h.sys->SetEventVoiceEffect(ev, 0, snd::dsp::kEffectReverb, nullptr, 0);
      h.sys->SetEventVoiceEffect(ev, 1, snd::dsp::kEffectLowpass, lp, 2);
    }
    h.Steps(1);
  }
  for (std::uint32_t i = 0; i < requested; ++i) {
    h.sys->Play(HashId(("L" + std::to_string(i % 4)).c_str()), (i % 4) == 3 ? em : snd_emitter_handle{0}, nullptr);
    if (i % 64 == 63) h.Steps(1);
  }
  h.Steps(20);
  std::vector<double> us;
  std::uint64_t cpuSum = 0;
  bool cpuOk = true;
  for (int i = 0; i < 300; ++i) {
    h.sys->Update();
    std::uint64_t c0 = 0, c1 = 0;
    cpuOk = ProcessCpuNs(&c0) && cpuOk;
    h.sys->RenderBlock(h.out.data());
    cpuOk = ProcessCpuNs(&c1) && cpuOk;
    cpuSum += c1 - c0;
    us.push_back(static_cast<double>(h.sys->LastBlock().totalNs) / 1000.0);
  }
  h.sys->Update();
  snd_stats s{};
  h.sys->GetStats(&s);
  *realOut = s.real_voices;
  *vfxOut = h.sys->GetVoiceFxStats();
  std::sort(us.begin(), us.end());
  double sum = 0;
  for (double v : us) sum += v;
  return Timing{sum / double(us.size()), us[us.size() * 99 / 100], us.back(),
                cpuOk ? static_cast<double>(cpuSum) / 1000.0 / double(us.size()) : -1.0};
}
}  // namespace

SND_TEST(CoreFxLoad, Real128VoicesWithTheVoiceEffectPoolFull) {
  const double blockUs = 256.0 * 1e6 / 48000.0;
  for (std::uint32_t slots : {0u, 4u, 16u, 32u}) {
    for (std::uint32_t workers : {0u, 2u}) {
      std::uint32_t real = 0;
      snd::core::System::VoiceFxStats vs{};
      const Timing t = RunLoad(workers, 128, &real, slots, &vs);
      snd::test::Note("128 voices, %u voice-effect slots (reverb + LP) in use %u, %u RT workers: CPU %.1f us (%.1f %%); "
                      "wall avg %.1f us, p99 %.1f us, max %.1f us, real %u",
                      slots, vs.slotsInUse, workers, t.cpuAvgUs, 100.0 * t.cpuAvgUs / blockUs, t.avgUs, t.p99Us,
                      t.maxUs, real);
      SND_CHECK_EQ(real, 128u);
      SND_CHECK_EQ(vs.slotsInUse, slots);  // the pool is full
    }
  }
}
