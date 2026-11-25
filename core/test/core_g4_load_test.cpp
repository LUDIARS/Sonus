// core, stage 3 group 4: load (design 14). 4x the real-voice limit spread over limit
// groups with the four steal modes; and 128 real voices into kBusMeter buses with the
// loudness meters and a spectrum reader running (D-147, D-148): one block must take
// < 30 % of its duration in optimised builds (CPU time of the rendering, as CoreLoad).
#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include "harness.hpp"

using namespace coretest;
using snd::HashId;

namespace {
[[maybe_unused]] bool ProcessCpuNs(std::uint64_t* ns) {
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return false;
  *ns = static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
  return true;
}

snd_config LoadConfig(std::uint32_t workers) {
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.max_real_voices = 128;
  cfg.max_virtual_voices = 1024;
  cfg.max_playbacks = 1024;
  cfg.jobs.realtime_workers = workers;
  cfg.max_voice_channels = 2;
  cfg.tuning.lod_down_blocks = 0;
  return cfg;
}

// 8 metered buses into the master; 4 events per steal mode, each in its own group of 64.
TestBank LoadBank() {
  TestBank b;
  for (int i = 0; i < 8; ++i) b.buses.push_back(TBus{HashId(("M" + std::to_string(i)).c_str()), 8u, 2, bf::kBusMeter, -6.0f, {}, {}});
  b.buses.push_back(TBus{HashId("Master"), kNone, 2, bf::kBusMaster, 0.0f, {}, {}});
  std::vector<float> st(2 * 88200);
  for (std::size_t i = 0; i < 88200; ++i) {
    st[2 * i] = 0.05f * static_cast<float>(std::sin(0.031 * double(i)));
    st[2 * i + 1] = 0.05f * static_cast<float>(std::sin(0.047 * double(i)));
  }
  b.assets.push_back(TAsset{HashId("st"), 2, 44100, st, 0, 88200});
  for (std::uint32_t mode = 0; mode < 4; ++mode) b.groups.push_back(TGroup{HashId(("G" + std::to_string(mode)).c_str()), 64, mode});
  for (int k = 0; k < 16; ++k) {
    TEvent e = SimpleEvent(("L" + std::to_string(k)).c_str(), 0, static_cast<std::uint32_t>(k % 8), -1.0f * float(k % 5));
    e.tracks[0].clips[0].pitchCents = 25.0f * float(k % 4);
    e.tracks[0].clips[0].loopCount = 0;
    e.group = static_cast<std::uint32_t>(k % 4);
    e.priority = 100u + static_cast<std::uint32_t>(k * 7 % 50);
    b.events.push_back(e);
  }
  return b;
}
}  // namespace

// 512 requests over groups with the four steal modes: bounded, real <= 128, no crash, and
// every refused / stolen play ends exactly once.
SND_TEST(CoreG4Load, FourTimesTheLimitWithStealModes) {
  Harness h(LoadConfig(2));
  h.Load("b", LoadBank().Build(HashId("Load")));
  std::vector<snd_playback_handle> plays;
  for (std::uint32_t i = 0; i < 512; ++i) {
    plays.push_back(h.sys->Play(HashId(("L" + std::to_string(i % 16)).c_str()), snd_emitter_handle{0}, nullptr));
    if (i % 32 == 31) h.Steps(1);
  }
  h.Steps(30);
  h.sys->Update();
  snd_stats s{};
  h.sys->GetStats(&s);
  snd::test::Note("512 requested over 4 groups of 64: real %u, virtual %u, playbacks %u", s.real_voices, s.virtual_voices,
                  s.playbacks);
  SND_CHECK(s.real_voices <= 128u);
  SND_CHECK(s.virtual_voices <= 4u * 64u);
  SND_CHECK(s.playbacks <= 4u * 64u + 8u);
  std::uint32_t rejected = 0, ended = 0;
  for (snd_playback_handle p : plays) {
    SND_CHECK(h.ends.CountOf(p) <= 1u);
    ended += h.ends.CountOf(p);
    rejected += h.ends.Has(p, SND_END_REJECTED, SND_END_DETAIL_LIMIT_GROUP) ? 1u : 0u;
  }
  SND_CHECK(rejected > 0u);  // the "never" group refuses (and "lowest priority" may)
  SND_CHECK_EQ(ended + s.playbacks, 512u);
  for (float v : h.out) SND_CHECK(std::isfinite(v));
}

// 128 real voices into 8 metered buses (loudness each), the output's loudness, and a
// spectrum read every block on the test thread (the rings published each block).
SND_TEST(CoreG4Load, Real128VoicesWithLoudnessAndSpectrum) {
  const double blockUs = 256.0 * 1e6 / 48000.0;
  for (std::uint32_t workers : {0u, 2u}) {
    Harness h(LoadConfig(workers));
    h.Load("b", LoadBank().Build(HashId("Load")));
    for (std::uint32_t i = 0; i < 128; ++i) {
      h.sys->Play(HashId(("L" + std::to_string(i % 16)).c_str()), snd_emitter_handle{0}, nullptr);
      if (i % 64 == 63) h.Steps(1);
    }
    h.Steps(20);
    std::vector<float> mag(513);
    std::vector<double> us;
    std::uint64_t cpuSum = 0;
    bool cpuOk = true;
    std::uint32_t spectra = 0;
    for (int i = 0; i < 300; ++i) {
      h.sys->Update();
      std::uint64_t c0 = 0, c1 = 0;
      cpuOk = ProcessCpuNs(&c0) && cpuOk;
      h.sys->RenderBlock(h.out.data());
      cpuOk = ProcessCpuNs(&c1) && cpuOk;
      cpuSum += c1 - c0;
      us.push_back(static_cast<double>(h.sys->LastBlock().totalNs) / 1000.0);
      spectra += h.sys->GetSpectrum(HashId(("M" + std::to_string(i % 8)).c_str()), 0u, mag.data(), 513u) == SND_OK ? 1u : 0u;
    }
    h.sys->Update();
    snd_stats s{};
    h.sys->GetStats(&s);
    std::sort(us.begin(), us.end());
    double sum = 0;
    for (double v : us) sum += v;
    const double cpuAvg = cpuOk ? static_cast<double>(cpuSum) / 1000.0 / double(us.size()) : -1.0;
    snd::test::Note("128 voices, 8 metered buses + spectrum, %u RT workers: CPU %.1f us (%.1f %%); wall avg %.1f us, "
                    "p99 %.1f us, max %.1f us, real %u, spectra %u",
                    workers, cpuAvg, 100.0 * cpuAvg / blockUs, sum / double(us.size()), us[us.size() * 99 / 100],
                    us.back(), s.real_voices, spectra);
    SND_CHECK_EQ(s.real_voices, 128u);
    SND_CHECK(spectra > 290u);
    snd_meter m{};
    SND_REQUIRE_EQ(h.sys->GetMeter(HashId("M3"), &m), SND_OK);
    SND_CHECK(std::isfinite(m.loudness_short) && m.loudness_short > -70.0f);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    if (cpuAvg >= 0.0) SND_CHECK(cpuAvg < 0.30 * blockUs);
#endif
  }
}
