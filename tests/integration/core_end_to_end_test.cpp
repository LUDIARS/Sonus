// End to end through core (stage 2): project (JSON + WAV) -> buildtool ->
// bank file -> memory FS -> io::IoScheduler (IO thread) -> core's
// BankManager (Scheduler mode, jobs::WorkerPool) -> registration at a block
// boundary -> Play -> RunEventLogic / voices / Render (sinc resampling
// 44.1 -> 48 kHz) -> bus lowpass effect in DSP pool memory (D-54) -> master.
// Checked analytically (Goertzel tone levels), then Stop, StopAndUnload and
// the bank release through core's epoch. The source kind is the core tests'
// PCMF reader (the Sample kind of the source module is the next group).
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "mini_project.hpp"
#include "snd/base/allocator.hpp"
#include "snd/buildtool/build.hpp"
#include "snd/core/core.hpp"
#include "snd/io/io_scheduler.hpp"
#include "snd/io/stream_pool.hpp"
#include "snd/jobs/worker_pool.hpp"
#include "snd/mocks/memory_file_system.hpp"
#include "snd/test/test.hpp"
#include "test_sources.hpp"

namespace bt = snd::buildtool;
using snd::HashId;

namespace {
bt::PcmData TwoTones(std::uint32_t rate, std::uint64_t frames) {
  bt::PcmData p;
  p.channels = 1;
  p.sampleRate = rate;
  p.frames = frames;
  p.samples.resize(static_cast<std::size_t>(frames));
  for (std::uint64_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f) / rate;
    p.samples[static_cast<std::size_t>(f)] =
        static_cast<float>(0.3 * std::sin(6.283185307179586 * 300.0 * t) + 0.3 * std::sin(6.283185307179586 * 9000.0 * t));
  }
  return p;
}
double Tone(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
  const double w = 6.283185307179586 * hz / 48000.0;
  double re = 0, im = 0, ws = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double win = 0.5 - 0.5 * std::cos(6.283185307179586 * double(i) / double(n - 1));
    const double v = x[(from + i) * 2] * win;  // left channel
    re += v * std::cos(w * double(i));
    im -= v * std::sin(w * double(i));
    ws += win;
  }
  return 2.0 * std::sqrt(re * re + im * im) / ws;
}
}  // namespace

SND_TEST(CoreEndToEnd, ProjectToSpeakerThroughCore) {
  mini::Project proj("core-e2e");
  proj.Add("banks/Main.json", "bank", "Main");
  proj.Add("buses/Master.json", "bus", "Master", "\"master\": true");
  proj.Add("buses/Sfx.json", "bus", "Sfx",
           "\"parent\": \"Master\", \"channels\": 2, \"effects\": [{\"type\": \"snd.fx.lowpass\", \"params\": [1500, 0.707]}]");
  proj.Wav("audio/tones.wav", TwoTones(44100, 88200), bt::WavSampleFormat::Float32);
  proj.Add("assets/tones.json", "asset", "tones",
           "\"file\": \"audio/tones.wav\", \"loadForm\": \"pcm\", \"convert\": {\"default\": {\"codec\": \"PCMF\"}}");
  proj.Add("events/Shot.json", "event", "Shot",
           "\"bank\": \"Main\", \"bus\": \"Sfx\", \"tracks\": [{\"clips\": [{\"asset\": \"tones\"}]}]");
  bt::BuildOptions o = proj.Options();
  o.encoders = &bt::CodecEncoders();
  bt::Diagnostics diag;
  if (!proj.Build(o, &diag)) {
    SND_CHECK_MSG(false, "%s", mini::Dump(diag).c_str());
    return;
  }

  snd::IAllocator& alloc = snd::base::DefaultAllocator();
  snd::mock::MemoryFileSystem fs;
  fs.SetDefaultLatency(1);
  fs.AddFile("Main.sndb", mini::ReadBytes(proj.out() / "Main.sndb"));
  snd::jobs::WorkerPool workers;
  snd::jobs::WorkerPoolConfig wc;
  wc.workers = 2;
  SND_REQUIRE(workers.Init(alloc, wc));
  snd::io::StreamPool streamPool;
  SND_REQUIRE(streamPool.Init(alloc, 64u << 10, 32u << 10, 2));
  snd::io::IoScheduler io;
  snd::io::IoSchedulerConfig ic;
  ic.maxStreams = 2;
  SND_REQUIRE(io.Init(alloc, fs, streamPool, ic));
  SND_REQUIRE(io.Start());

  coretest::TestSampleKind sample;
  snd::source::SourceKindSet kinds{};
  kinds.kinds[static_cast<int>(snd::SourceKind::Sample)] = &sample;
  snd_config cfg;
  snd_config_init(&cfg);
  cfg.jobs.realtime_workers = 2;
  snd::core::CoreDesc d;
  d.config = &cfg;
  d.allocator = &alloc;
  d.sources = &kinds;
  d.jobs = &workers;
  d.bankScheduler = &io;
  d.rtWorkerRealtimePriority = false;
  snd::core::System* sys = nullptr;
  SND_REQUIRE_EQ(snd::core::System::Create(d, &sys), SND_OK);
  std::vector<float> out(256 * 2), all;
  auto step = [&] {
    sys->Update();
    sys->RenderBlock(out.data());
    all.insert(all.end(), out.begin(), out.end());
  };
  const snd_bank_handle bank = sys->LoadBankAsync("Main.sndb");
  SND_REQUIRE(bank.value != 0u);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (sys->GetBankState(bank) != SND_BANK_STATE_LOADED && std::chrono::steady_clock::now() < until) {
    step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SND_REQUIRE_EQ(sys->GetBankState(bank), SND_BANK_STATE_LOADED);
  all.clear();
  const snd_playback_handle p = sys->Play(HashId("Shot"), snd_emitter_handle{0}, nullptr);
  for (int i = 0; i < 40; ++i) step();
  // Mono -> stereo 0.7071; the 1.5 kHz lowpass passes 300 Hz (-0.03 dB) and removes 9 kHz (> -30 dB).
  const double low = Tone(all, 4096, 4096, 300.0);
  const double high = Tone(all, 4096, 4096, 9000.0);
  SND_CHECK_NEAR(low, 0.3 * 0.70710678, 0.01);
  SND_CHECK(high < 0.3 * 0.70710678 * 0.04);
  sys->Update();
  SND_CHECK_EQ(sys->GetPlaybackState(p), SND_PLAYBACK_STATE_PLAYING);
  snd_meter m{};
  SND_CHECK_EQ(sys->GetMeter(HashId("Sfx"), &m), SND_OK);
  SND_CHECK(m.peak[0] > 0.1f);
  // Unload while playing: the playback is discarded, the bank goes back after core's epoch.
  SND_CHECK_EQ(sys->UnloadBank(bank, SND_UNLOAD_STOP_AND_UNLOAD), SND_OK);
  for (int i = 0; i < 20 && sys->GetBankState(bank) != SND_BANK_STATE_UNLOADED; ++i) step();
  SND_CHECK_EQ(sys->GetBankState(bank), SND_BANK_STATE_UNLOADED);
  SND_CHECK_EQ(sys->GetPlaybackState(p), SND_PLAYBACK_STATE_NONE);
  // Teardown order (bank_manager.hpp): audio stops, IO scheduler, jobs, core, then the scheduler memory.
  io.Shutdown();
  workers.Shutdown();
  snd::core::System::Destroy(sys);
  io.Destroy();
  streamPool.Destroy();
}
