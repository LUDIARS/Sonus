// One acoustic script for stage 3. A moving area source, a zone send, voice
// reverb/LP, snapshot contributions and block spectra are active together.
#include <array>
#include <ctime>
#include "indep_g2_fixture.hpp"
#include "snd/test/rt_guard.hpp"

namespace {
using namespace g2;
using snd::HashId;

struct Capture {
  std::vector<float> audio;
  std::vector<float> spectra;
  std::vector<End> ended;
};

void RenderScript(unsigned workers, unsigned voices, bool measure, Capture& capture) {
  auto cfg = G2Config();
  cfg.max_real_voices = voices;
  cfg.max_virtual_voices = 256;
  cfg.max_playbacks = 256;
  cfg.max_voice_effects = 4;
  cfg.voice_effect_bytes = 192u << 10;
  cfg.commands.arena_bytes = 64u << 10;
  cfg.memory.frame_arena_bytes = 2u << 20;
  cfg.memory.thread_arena_bytes = 512u << 10;
  cfg.jobs.realtime_workers = workers;
  cfg.jobs.voices_per_chunk = 16;
  CountingAllocator alloc;
  Fx h(cfg, {}, &alloc);
  SND_REQUIRE_EQ(h.created, SND_OK);
  FxBank b;
  b.b.buses = {{HashId("Dry"), 2, 2, bf::kBusMeter, -6, {}, {}},
               {HashId("Zone"), 2, 2, bf::kBusMeter, -10, {}, {}},
               {HashId("Master"), kNone, 2, bf::kBusMaster | bf::kBusMeter, 0, {}, {}}};
  b.b.snapshots = {{HashId("Near"), {{0, 1, 0, -6}}}};
  b.b.curves = {{HashId("Level"), 0, 0, {{0, -6}, {1, -16}}}};
  b.b.params = {{HashId("Level"), 0, 0, 1, 0, 0}};
  auto& tone = b.Event("Moving", 12);
  tone.flags = bf::kEvent3D;
  tone.curves = {0};
  tone.maxDistance = 20;
  auto& plain = b.Event("Bed", 8);
  plain.curves = {0};
  const auto loaded = h.Load("scene", b.Build("Scene"));
  SND_REQUIRE_EQ(h.sys->GetBankState(loaded), SND_BANK_STATE_LOADED);
  snd_listener_desc listener{};
  listener.enabled = 1;
  listener.forward.z = 1;
  listener.up.y = 1;
  listener.weight = 1;
  h.sys->SetListener(0, &listener);
  const auto emitter = h.sys->CreateEmitter();
  const auto zone = h.sys->CreateEmitter();
  snd_vec3 half{2, 1, 1};
  snd_vec3 pos{1, 0, 3};
  h.sys->SetTransform(emitter, &pos, nullptr, nullptr);
  h.sys->SetEmitterShape(emitter, SND_EMITTER_SHAPE_LINE, &half);
  h.sys->SetEmitterShape(zone, SND_EMITTER_SHAPE_BOX, &half);
  h.sys->SetReverbZone(zone, HashId("Zone"), 0.6f, 6);
  float lp[2]{4000, 0.707f};
  h.sys->SetEventVoiceEffect(HashId("Moving"), 0, dsp::kEffectReverb, nullptr, 0);
  h.sys->SetEventVoiceEffect(HashId("Moving"), 1, dsp::kEffectLowpass, lp, 2);
  std::array<float, 513> spectrum{};
  h.sys->GetSpectrum(HashId("Dry"), 0, spectrum.data(), 513);
  h.Steps(4);
  const auto initial = alloc.allocations.load();
  std::vector<snd_playback_handle> handles;
  for (unsigned i = 0; i < voices; ++i) {
    const auto id = i < 4 ? HashId("Moving") : HashId("Bed");
    handles.push_back(h.sys->PlayFade(id, emitter, nullptr, 0.003f));
  }
  h.Steps(12);
  SND_REQUIRE_EQ(h.sys->GetVoiceFxStats().slotsInUse, 4u);
  snd_stats stats{};
  h.sys->GetStats(&stats);
  SND_REQUIRE_EQ(stats.real_voices, voices);
  double cpuSec = 0;
  const unsigned blocks = measure ? 128 : 48;
  for (unsigned block = 0; block < blocks; ++block) {
    if (block % 8 == 0) {
      pos.x = static_cast<float>(block % 11) * 0.4f - 1;
      h.sys->SetTransform(emitter, &pos, nullptr, nullptr);
      h.sys->ApplySnapshot(HashId("Near"), static_cast<float>(block % 3) * 0.45f, 0.04f);
      h.sys->SetParamGlobal(HashId("Level"), static_cast<float>(block % 5) * 0.25f);
    }
    if (!measure && block == 20) {
      const auto when = AtClock(h.Total() + block * h.Frames() + 79);
      h.sys->SetScheduledStop(handles.back(), &when, 0);
    }
    h.sys->Update();
    const auto before = std::clock();
    {
      SND_RT_SCOPE();
      h.sys->RenderBlock(h.out.data());
    }
    const auto after = std::clock();
    SND_REQUIRE(before != std::clock_t(-1) && after >= before);
    cpuSec += double(after - before) / CLOCKS_PER_SEC;
    if (!measure) capture.audio.insert(capture.audio.end(), h.out.begin(), h.out.end());
    SND_REQUIRE_EQ(h.sys->GetSpectrum(HashId("Dry"), 0, spectrum.data(), 513), SND_OK);
    if (!measure) capture.spectra.insert(capture.spectra.end(), spectrum.begin(), spectrum.end());
  }
  h.sys->Update();
  h.sys->GetStats(&stats);
  SND_CHECK_EQ(stats.late_chunks, 0u);
  SND_CHECK_EQ(alloc.allocations.load() - initial, 0u);
  SND_CHECK_EQ(snd::test::rt::UnexpectedTotal(), 0u);
  if (measure) {
    const double blockUs = 1e6 * cpuSec / blocks;
    const double pct = blockUs * 100 / (1e6 * h.Frames() / h.sys->SampleRate());
    snd::test::Note("workers=%u voices=%u FX=%u CPU %.2f us/block (%.2f%%) allocations=%llu RT=%u",
        workers, stats.real_voices, h.sys->GetVoiceFxStats().slotsInUse, blockUs, pct,
        static_cast<unsigned long long>(alloc.allocations.load() - initial), snd::test::rt::UnexpectedTotal());
    SND_CHECK_EQ(stats.real_voices, voices);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    SND_CHECK(pct < 30.0);
#endif
  } else {
    capture.ended = h.ends;
    SND_CHECK(h.Ended(handles.back().value, SND_END_STOPPED));
  }
}

SND_TEST(IndepCrossStage3, SerialReplayAndTwoWorkersMatchAudioNotificationsAndSpectrum) {
  Capture a, b, c;
  RenderScript(0, 24, false, a);
  RenderScript(0, 24, false, b);
  RenderScript(2, 24, false, c);
  SND_REQUIRE(MaxAbsAll(a.audio) > 0.005f);
  SND_REQUIRE(MaxAbsAll(a.spectra) > 0);
  SND_REQUIRE(!a.ended.empty());
  for (const auto* other : {&b, &c}) {
    SND_REQUIRE_EQ(a.audio.size(), other->audio.size());
    SND_REQUIRE_EQ(a.spectra.size(), other->spectra.size());
    SND_CHECK(std::memcmp(a.audio.data(), other->audio.data(), a.audio.size() * sizeof(float)) == 0);
    SND_CHECK(std::memcmp(a.spectra.data(), other->spectra.data(), a.spectra.size() * sizeof(float)) == 0);
    SND_REQUIRE_EQ(a.ended.size(), other->ended.size());
    for (std::size_t i = 0; i < a.ended.size(); ++i) {
      SND_CHECK_EQ(a.ended[i].handle, other->ended[i].handle);
      SND_CHECK_EQ(a.ended[i].reason, other->ended[i].reason);
      SND_CHECK_EQ(a.ended[i].detail, other->ended[i].detail);
    }
  }
}

SND_TEST(IndepCrossStage3, Real128VoicesRunWithinBudgetWithoutAllocationOrRtViolation) {
  Capture unused;
  for (unsigned workers : {0u, 2u}) RenderScript(workers, 128, true, unused);
}
} // namespace
