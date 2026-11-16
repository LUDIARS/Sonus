// core: load (design 14, stage-2 completion condition): 128 real voices,
// 1 block must take < 30 % of the block duration (5.33 ms at 256 / 48 kHz).
// Sources are stereo 44.1 kHz samples with a pitch (sinc resampling), a
// voice lowpass on half of them, 3D on a quarter, two buses with effects.
// The timing check is enforced in optimised builds only (NDEBUG, no
// sanitizers); other builds report the numbers. 4x the real limit is
// requested to check the selection under pressure.
#include <algorithm>
#include <cmath>
#include <vector>

#include "harness.hpp"
#include "snd/dsp/effects.hpp"
#include "snd/platform/clock.hpp"

using namespace coretest;
using snd::HashId;

namespace {
struct Timing {
  double avgUs, p99Us, maxUs;
};
Timing RunLoad(std::uint32_t workers, std::uint32_t requested, std::uint32_t* realOut) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_real_voices = 128;
  cfg.max_virtual_voices = 1024;
  cfg.max_playbacks = 1024;
  cfg.jobs.realtime_workers = workers;
  cfg.max_voice_channels = 2;
  cfg.tuning.lod_down_blocks = 0;  // measure without the load LOD (2.4)
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
  for (std::uint32_t i = 0; i < requested; ++i) {
    h.sys->Play(HashId(("L" + std::to_string(i % 4)).c_str()), (i % 4) == 3 ? em : snd_emitter_handle{0}, nullptr);
    if (i % 64 == 63) h.Steps(1);
  }
  h.Steps(20);
  std::vector<double> us;
  for (int i = 0; i < 300; ++i) {
    h.sys->Update();
    h.sys->RenderBlock(h.out.data());
    us.push_back(static_cast<double>(h.sys->LastBlock().totalNs) / 1000.0);
  }
  h.sys->Update();
  snd_stats s{};
  h.sys->GetStats(&s);
  *realOut = s.real_voices;
  std::sort(us.begin(), us.end());
  double sum = 0;
  for (double v : us) sum += v;
  return Timing{sum / double(us.size()), us[us.size() * 99 / 100], us.back()};
}
}  // namespace

SND_TEST(CoreLoad, Real128VoicesWithinBudget) {
  const double blockUs = 256.0 * 1e6 / 48000.0;
  for (std::uint32_t workers : {0u, 2u}) {
    std::uint32_t real = 0;
    const Timing t = RunLoad(workers, 128, &real);
    snd::test::Note("128 voices, %u RT workers: avg %.1f us (%.1f %%), p99 %.1f us, max %.1f us, real %u", workers,
                    t.avgUs, 100.0 * t.avgUs / blockUs, t.p99Us, t.maxUs, real);
    SND_CHECK_EQ(real, 128u);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    SND_CHECK(t.avgUs < 0.30 * blockUs);
#endif
  }
}

SND_TEST(CoreLoad, FourTimesTheRealLimit) {
  std::uint32_t real = 0;
  const Timing t = RunLoad(2, 512, &real);
  snd::test::Note("512 requested (4x), 2 RT workers: avg %.1f us, p99 %.1f us, max %.1f us, real %u", t.avgUs,
                  t.p99Us, t.maxUs, real);
  SND_CHECK_EQ(real, 128u);
}
